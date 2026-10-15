#include "Transcriber.h"
#include "Separator.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include "basicpitch.hpp"
#include <cmath>

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace Transcriber
{

void initRuntime()
{
#if JUCE_WINDOWS
    static bool done = false;
    if (done) return;
    done = true;
    // The plugin DLL lives in <x>.vst3/Contents/x86_64-win/; onnxruntime.dll is copied next to it at build time.
    auto here = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();
    auto dll = here.getChildFile ("onnxruntime.dll");
    if (dll.existsAsFile()) LoadLibraryW (dll.getFullPathName().toWideCharPointer());
#endif
}

// ---------------------------------------------------------------------------
bool notesFromAudio (const juce::AudioBuffer<float>& in, double rate, NoteList& out, juce::String& error)
{
    initRuntime();
    out = {};
    if (in.getNumSamples() < 4096) { error = "Clip is too short"; return false; }

    auto mono = Separator::resample (in, rate, (double) basic_pitch::constants::SAMPLE_RATE, 1);
    std::vector<float> audio (mono.getReadPointer (0), mono.getReadPointer (0) + mono.getNumSamples());

    std::vector<uint8_t> bytes;
    try
    {
        auto result = basic_pitch::ort_inference (audio);
        bytes = basic_pitch::convert_to_midi (result, true, false);
    }
    catch (const std::exception& e) { error = juce::String ("Basic Pitch failed: ") + e.what(); return false; }
    catch (...) { error = "Basic Pitch failed"; return false; }

    juce::MemoryInputStream mis (bytes.data(), bytes.size(), false);
    juce::MidiFile mf;
    if (! mf.readFrom (mis)) { error = "Could not parse MIDI from Basic Pitch"; return false; }
    mf.convertTimestampTicksToSeconds();
    for (int t = 0; t < mf.getNumTracks(); ++t)
    {
        auto* seq = mf.getTrack (t);
        for (int i = 0; i < seq->getNumEvents(); ++i)
        {
            auto* ev = seq->getEventPointer (i);
            if (! ev->message.isNoteOn()) continue;
            const double start = ev->message.getTimeStamp();
            double end = start + 0.25;
            if (ev->noteOffObject != nullptr) end = ev->noteOffObject->message.getTimeStamp();
            out.notes.push_back ({ start, juce::jmax (0.03, end - start), ev->message.getNoteNumber(), juce::jlimit (1, 127, (int) ev->message.getVelocity()) });
        }
    }
    std::sort (out.notes.begin(), out.notes.end(), [] (auto& a, auto& b) { return a.startSec < b.startSec || (a.startSec == b.startSec && a.pitch < b.pitch); });
    if (out.notes.empty()) { error = "No notes detected"; return false; }
    return true;
}

// ---------------------------------------------------------------------------
// Drum transcription: 3-band spectral-flux onset detection.
//   low  (30-150 Hz)   -> kick  (36)
//   mid  (150-2.5 kHz) -> snare (38)
//   high (5-16 kHz)    -> closed hat (42) / open hat (46 when the high band rings on)
bool drumsFromAudio (const juce::AudioBuffer<float>& in, double rate, NoteList& out, juce::String& error)
{
    out = {};
    out.isDrums = true;
    if (in.getNumSamples() < 4096) { error = "Clip is too short"; return false; }

    const double sr = 44100.0;
    auto mono = Separator::resample (in, rate, sr, 1);
    const float* x = mono.getReadPointer (0);
    const int n = mono.getNumSamples();

    constexpr int order = 10, N = 1 << order, hop = 256;
    juce::dsp::FFT fft (order);
    std::vector<float> win ((size_t) N);
    for (int i = 0; i < N; ++i) win[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (N - 1));
    std::vector<float> buf ((size_t) N * 2);

    const int frames = juce::jmax (1, (n - N) / hop + 1);
    const float binHz = (float) sr / (float) N;
    auto bin = [binHz] (float hz) { return juce::jlimit (1, N / 2 - 1, (int) std::round (hz / binHz)); };
    const int loA = bin (30.f),  loB = bin (150.f);
    const int miA = bin (150.f), miB = bin (2500.f);
    const int hiA = bin (5000.f), hiB = bin (16000.f);

    std::vector<float> E[3], dB[3];
    for (auto& e : E) e.assign ((size_t) frames, 0.f);
    for (auto& e : dB) e.assign ((size_t) frames, -100.f);
    for (int f = 0; f < frames; ++f)
    {
        std::fill (buf.begin(), buf.end(), 0.f);
        const int off = f * hop;
        for (int i = 0; i < N && off + i < n; ++i) buf[(size_t) i] = x[off + i] * win[(size_t) i];
        fft.performFrequencyOnlyForwardTransform (buf.data());
        auto band = [&] (int a, int b) { float s = 0.f; for (int k = a; k <= b; ++k) s += buf[(size_t) k] * buf[(size_t) k]; return s / (float) (b - a + 1); };
        const float p[3] = { band (loA, loB), band (miA, miB), band (hiA, hiB) };
        for (int b = 0; b < 3; ++b)
        {
            E[b][(size_t) f] = std::log1p (200.f * p[b]);
            dB[b][(size_t) f] = 10.f * std::log10 (p[b] + 1e-12f);
        }
    }

    struct Onset { int frame; int band; float strength; };
    std::vector<Onset> onsets;
    const int minGapFrames = (int) (0.06 * sr / hop);       // 60 ms per band
    const float absThreshold[3] = { 0.6f, 0.5f, 0.4f };
    const float relThreshold[3] = { 0.30f, 0.30f, 0.25f };    // fraction of the band's strongest attack
    for (int b = 0; b < 3; ++b)
    {
        // attack = rise over the last ~17 ms, so a real hit stands out from decay wobble
        std::vector<float> flux ((size_t) frames, 0.f);
        float fluxMax = 0.f;
        for (int f = 3; f < frames; ++f)
        {
            const float prev = juce::jmin (E[b][(size_t) f - 1], E[b][(size_t) f - 2], E[b][(size_t) f - 3]);
            flux[(size_t) f] = juce::jmax (0.f, E[b][(size_t) f] - prev);
            fluxMax = juce::jmax (fluxMax, flux[(size_t) f]);
        }
        const float thr = juce::jmax (absThreshold[b], fluxMax * relThreshold[b]);
        int last = -minGapFrames;
        for (int f = 3; f < frames - 3; ++f)
        {
            const float v = flux[(size_t) f];
            if (v < thr) continue;
            bool localMax = true;
            for (int k = f - 3; k <= f + 3; ++k) if (k != f && flux[(size_t) k] > v) localMax = false;
            if (! localMax || f - last < minGapFrames) continue;
            last = f;
            onsets.push_back ({ f, b, v });
        }
    }
    if (onsets.empty()) { error = "No drum hits detected"; return false; }

    float maxStrength[3] = { 0.f, 0.f, 0.f };
    for (auto& o : onsets) maxStrength[o.band] = juce::jmax (maxStrength[o.band], o.strength);

    // peak level of a band in the 25 ms after an onset
    auto peakDb = [&] (int band, int frame)
    {
        float m = -100.f;
        for (int k = frame; k <= juce::jmin (frames - 1, frame + 4); ++k) m = juce::jmax (m, dB[band][(size_t) k]);
        return m;
    };
    const int coincide = (int) (0.015 * sr / hop); // 15 ms
    auto hasOnset = [&] (int band, int frame)
    {
        for (auto& o : onsets) if (o.band == band && std::abs (o.frame - frame) <= coincide) return true;
        return false;
    };

    for (auto& o : onsets)
    {
        const double t = (double) (o.frame * hop + N / 4) / sr;
        const int vel = juce::jlimit (40, 127, (int) std::round (50.f + 77.f * o.strength / juce::jmax (0.01f, maxStrength[o.band])));
        const float lo = peakDb (0, o.frame), mid = peakDb (1, o.frame), hi = peakDb (2, o.frame);
        if (o.band == 0)
        {
            // a kick's low end dominates its mids; a snare/tom body with mids on top is not a kick
            if (lo < mid - 3.f) continue;
            out.notes.push_back ({ t, 0.10, 36, vel });
        }
        else if (o.band == 1)
        {
            if (hasOnset (0, o.frame) && mid < lo - 8.f) continue;   // just the click of a kick
            if (! hasOnset (0, o.frame) && mid < hi - 6.f) continue; // just the body of a hat
            out.notes.push_back ({ t, 0.10, 38, vel });
        }
        else
        {
            const bool snareHere = hasOnset (1, o.frame) && mid >= lo - 8.f;
            if (snareHere && hi < mid - 2.f) continue;              // the snare's own crack
            if (hi < lo - 30.f) continue;                            // buried under the kick
            // open hat: the top end is still ringing 120 ms later
            const int later = o.frame + (int) (0.12 * sr / hop);
            const bool open = later < frames && dB[2][(size_t) later] > hi - 6.f;
            out.notes.push_back ({ t, open ? 0.25 : 0.06, open ? 46 : 42, vel });
        }
    }
    std::sort (out.notes.begin(), out.notes.end(), [] (auto& a, auto& b) { return a.startSec < b.startSec || (a.startSec == b.startSec && a.pitch < b.pitch); });
    if (out.notes.empty()) { error = "No drum hits detected"; return false; }
    return true;
}

// ---------------------------------------------------------------------------
bool writeMidiFile (const NoteList& list, double bpm, const juce::File& dest)
{
    const int tpq = 960;
    const double secPerBeat = 60.0 / juce::jlimit (20.0, 300.0, bpm);
    juce::MidiMessageSequence seq;
    seq.addEvent (juce::MidiMessage::tempoMetaEvent ((int) std::round (secPerBeat * 1000000.0)), 0.0);
    seq.addEvent (juce::MidiMessage::timeSignatureMetaEvent (4, 4), 0.0);
    const int channel = list.isDrums ? 10 : 1;
    for (auto& nt : list.notes)
    {
        const double startTicks = nt.startSec / secPerBeat * tpq;
        const double endTicks = (nt.startSec + nt.lenSec) / secPerBeat * tpq;
        seq.addEvent (juce::MidiMessage::noteOn (channel, nt.pitch, (juce::uint8) nt.velocity), startTicks);
        seq.addEvent (juce::MidiMessage::noteOff (channel, nt.pitch), juce::jmax (startTicks + 1.0, endTicks));
    }
    seq.updateMatchedPairs();
    juce::MidiFile mf;
    mf.setTicksPerQuarterNote (tpq);
    mf.addTrack (seq);
    dest.getParentDirectory().createDirectory();
    dest.deleteFile();
    juce::FileOutputStream os (dest);
    if (! os.openedOk()) return false;
    return mf.writeTo (os, 0);
}

} // namespace Transcriber
