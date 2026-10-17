#include "Transcriber.h"
#include "Separator.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include "basicpitch.hpp"
#include <cmath>
#include <cstdio>
#include <algorithm>

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
namespace
{
    struct RawNote { int s, e, pitch; float amp; bool alive = true; };
    constexpr int kFrameTol = 3;   // ~35 ms at the model's 86 fps

    const char* noteName (int p) { static const char* n[] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" }; return n[(p % 12 + 12) % 12]; }
}

// ---------------------------------------------------------------------------
// Hybrid tracker. Basic Pitch is good at note ATTACKS but its "note sounding" confidence collapses on held piano /
// pad chords (a whole-bar A minor chord came out as a couple of short C4s). So: per model frame, measure the audio's
// energy per semitone (FFT 8192 @ 44.1k, hop 512 = the model's 86 fps). A note starts where the model has an onset
// for that pitch, or where a semitone jumps up sharply and is not just a harmonic of a louder lower note. It lasts
// while that semitone stays within `kHoldDb` of its level at the attack. Velocity = level at the attack.
namespace
{
    constexpr float kHoldDb = 15.f, kFloorDb = -42.f, kAttackDb = 7.f;
    constexpr int kMaxPerAttack = 6;
    constexpr int kLo = 36, kHi = 103;   // C2 .. G7 (below C2 the FFT bins no longer separate semitones)
    constexpr float kQuietDb = 26.f;     // a note settling more than this under the clip's loudest moment is not worth writing

    struct Spectrogram
    {
        int frames = 0;
        std::vector<float> db;                 // [frame * 128 + midi]
        float at (int f, int m) const { return db[(size_t) f * 128 + (size_t) m]; }
    };

    Spectrogram analyse (const float* x, int n, double sr)
    {
        constexpr int order = 13, N = 1 << order, hop = 512;
        juce::dsp::FFT fft (order);
        std::vector<float> win ((size_t) N), buf ((size_t) N * 2);
        for (int i = 0; i < N; ++i) win[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (N - 1));
        // bins -> semitone (nearest), accumulate power
        std::vector<int> binNote ((size_t) N / 2, -1);
        for (int k = 1; k < N / 2; ++k)
        {
            const double hz = k * sr / N;
            const int m = (int) std::round (69.0 + 12.0 * std::log2 (hz / 440.0));
            if (m >= kLo && m <= kHi) binNote[(size_t) k] = m;
        }
        Spectrogram sg;
        sg.frames = juce::jmax (1, (n - N / 4) / hop + 1);
        sg.db.assign ((size_t) sg.frames * 128, -120.f);
        std::vector<float> e (128);
        for (int f = 0; f < sg.frames; ++f)
        {
            const int off = f * hop - N / 2;   // window centred on the frame
            std::fill (buf.begin(), buf.end(), 0.f);
            for (int i = 0; i < N; ++i) { const int j = off + i; if (j >= 0 && j < n) buf[(size_t) i] = x[j] * win[(size_t) i]; }
            fft.performFrequencyOnlyForwardTransform (buf.data());
            std::fill (e.begin(), e.end(), 0.f);
            for (int k = 1; k < N / 2; ++k) if (binNote[(size_t) k] >= 0) e[(size_t) binNote[(size_t) k]] += buf[(size_t) k] * buf[(size_t) k];
            for (int m = kLo; m <= kHi; ++m) sg.db[(size_t) f * 128 + (size_t) m] = 10.f * std::log10 (e[(size_t) m] + 1e-10f);
        }
        return sg;
    }

    bool isHarmonicInterval (int iv) { static const int h[] = { 12, 19, 24, 28, 31, 34, 36 }; return std::find (std::begin (h), std::end (h), iv) != std::end (h); }

    // model frame index <-> spectrogram frame: both run at the same 86.13 fps, offset by the model's window shift
    int modelToSpec (int modelFrame, const std::vector<float>& times, double sr) { return (int) std::round (times[(size_t) modelFrame] * sr / 512.0); }
    int specToModel (int specFrame, const std::vector<float>& times, double sr)
    {
        const double t = specFrame * 512.0 / sr;
        int lo = 0, hi = (int) times.size() - 1;
        while (lo < hi) { const int mid = (lo + hi) / 2; if (times[(size_t) mid] < t) lo = mid + 1; else hi = mid; }
        return lo;
    }
}

bool notesFromAudio (const juce::AudioBuffer<float>& in, double rate, NoteList& out, juce::String& error, NoteOptions opts)
{
    initRuntime();
    out = {};
    if (in.getNumSamples() < 4096) { error = "Clip is too short"; return false; }

    auto mono = Separator::resample (in, rate, (double) basic_pitch::constants::SAMPLE_RATE, 1);
    std::vector<float> audio (mono.getReadPointer (0), mono.getReadPointer (0) + mono.getNumSamples());

    std::vector<RawNote> notes;
    int nFrames = 0;
    basic_pitch::InferenceResult result;
    try
    {
        result = basic_pitch::ort_inference (audio);
        nFrames = (int) result.notes.dimension (0);
        if (! opts.hybrid || opts.mono)
            for (auto& ev : basic_pitch::infer_note_events (result, true))
                notes.push_back ({ ev.start_idx, ev.end_idx, ev.pitch, ev.amplitude });
    }
    catch (const std::exception& e) { error = juce::String ("Basic Pitch failed: ") + e.what(); return false; }
    catch (...) { error = "Basic Pitch failed"; return false; }

    if (opts.hybrid && ! opts.mono)
    {
        constexpr double sr = 44100.0;
        auto mono44 = Separator::resample (in, rate, sr, 1);
        const auto sg = analyse (mono44.getReadPointer (0), mono44.getNumSamples(), sr);
        auto times = basic_pitch::frame_times (nFrames + 1);
        float globalMax = -120.f;
        for (float v : sg.db) globalMax = juce::jmax (globalMax, v);
        const float floorDb = globalMax + kFloorDb;

        // per-pitch "sounding" state while scanning frames
        struct Live { int start = -1; float startDb = -120.f; int below = 0; int firstBelow = -1; bool fromModel = false; };
        std::vector<Live> live (128);
        std::vector<int> lastOnsetSpec (128, -1000);
        const int nSpec = sg.frames;
        const int minGap = 5;    // ~60 ms between two attacks of one pitch (a model onset may restart an early spectral start)

        auto modelOnsetAt = [&] (int specFrame, int m)
        {
            const int mf = specToModel (specFrame, times, sr), p = m - basic_pitch::constants::MIDI_OFFSET;
            if (p < 0 || p >= (int) result.onsets.dimension (1)) return 0.f;
            float v = 0.f;
            for (int f = juce::jmax (0, mf - 2); f <= juce::jmin (nFrames - 1, mf + 2); ++f) v = juce::jmax (v, result.onsets (f, p));
            return v;
        };
        // the model really heard a note here: an onset peak AND it keeps the pitch "sounding" right after
        auto modelSupports = [&] (int specFrame, int m)
        {
            if (modelOnsetAt (specFrame, m) < 0.5f) return false;
            const int mf = specToModel (specFrame, times, sr), p = m - basic_pitch::constants::MIDI_OFFSET;
            float mean = 0.f; int cnt = 0;
            for (int f = mf; f <= juce::jmin (nFrames - 1, mf + 6); ++f) { mean += result.notes (f, p); ++cnt; }
            return cnt > 0 && mean / (float) cnt > 0.3f;
        };
        auto finish = [&] (int m, int endFrame)
        {
            auto& L = live[(size_t) m];
            if (L.start < 0) return;
            const bool strong = L.startDb > globalMax - kQuietDb;
            const int minLen = L.fromModel ? 3 : (strong ? 6 : 9);
            if (endFrame - L.start >= minLen && L.startDb > floorDb + 10.f)
            {
                const float amp = juce::jlimit (0.05f, 1.f, (L.startDb - floorDb) / (globalMax - floorDb));
                notes.push_back ({ specToModel (L.start, times, sr), specToModel (endFrame, times, sr), m, amp });
            }
            L.start = -1;
        };

        // Harmonic or chord tone? A candidate at a harmonic interval above a note that is attacking at the SAME time
        // is a harmonic unless the model heard an onset for it too, or it is louder than the fundamental. Above a note
        // that has been sounding for a while, a fresh attack is its own note unless it is far weaker.
        auto explainedByHarmonic = [&] (int f, int m, const std::vector<int>& lowerNotes)
        {
            for (int n : lowerNotes)
            {
                if (! isHarmonicInterval (m - n)) continue;
                const bool coAttack = live[(size_t) n].start >= f - 8;   // the 186 ms window smears attacks by up to ~90 ms
                if (coAttack) { if (! modelSupports (f, m) && sg.at (f, m) <= sg.at (f, n) + 2.f) return true; }
                else if (sg.at (f, m) < sg.at (f, n) - 12.f) return true;
            }
            return false;
        };
        // a semitone is a real spectral peak (not leakage from a neighbour) when it beats both neighbours
        auto isPeak = [&] (int f, int m) { return sg.at (f, m) > sg.at (f, m - 1) && sg.at (f, m) >= sg.at (f, m + 1); };
        auto riseAt = [&] (int f, int m, int span)
        {
            float lo = 120.f;
            for (int k = juce::jmax (0, f - span); k < f; ++k) lo = juce::jmin (lo, sg.at (k, m));
            return sg.at (f, m) - lo;
        };
        // level the note settles to right after the attack (the transient itself overshoots)
        auto settledDb = [&] (int f, int m)
        {
            float sum = 0; int cnt = 0;
            for (int k = f + 3; k <= juce::jmin (nSpec - 1, f + 7); ++k) { sum += sg.at (k, m); ++cnt; }
            return cnt > 0 ? sum / (float) cnt : sg.at (f, m);
        };

        // notes already sounding when the clip starts (a loop cut mid-chord)
        {
            const int f = 8;
            float fmax = -120.f;
            for (int m = kLo; m <= kHi; ++m) fmax = juce::jmax (fmax, sg.at (f, m));
            std::vector<int> lower;
            for (int m = kLo + 1; m < kHi; ++m)
                if (sg.at (f, m) > fmax - 20.f && isPeak (f, m) && ! explainedByHarmonic (f, m, lower))
                { live[(size_t) m] = { 0, settledDb (f, m) }; lastOnsetSpec[(size_t) m] = 0; lower.push_back (m); }
        }
        for (int f = 8; f < nSpec; ++f)
        {
            float fmax = -120.f;
            for (int m = kLo; m <= kHi; ++m) fmax = juce::jmax (fmax, sg.at (f, m));
            // which pitches attack here: model onset, or a sharp spectral rise on a real peak
            struct Att { int m; float level; };
            std::vector<Att> attacks;
            bool anyModelOnset = false;
            for (int m = kLo + 1; m < kHi; ++m)
            {
                const float d = sg.at (f, m);
                if (d < floorDb || d < fmax - 30.f || ! isPeak (f, m)) continue;
                const bool model = modelOnsetAt (f, m) > 0.5f;
                if (model) anyModelOnset = true;
                if ((model && riseAt (f, m, 3) > 2.f) || riseAt (f, m, 6) > kAttackDb) attacks.push_back ({ m, d });
            }
            // a model onset on any pitch = a chord is being struck: pull in every strong peak that rose at all
            if (anyModelOnset)
                for (int m = kLo + 1; m < kHi; ++m)
                {
                    const float d = sg.at (f, m);
                    if (d < floorDb || d < fmax - 14.f || ! isPeak (f, m)) continue;
                    bool have = false; for (auto& a : attacks) have |= a.m == m;
                    if (! have && riseAt (f, m, 4) > 3.f) attacks.push_back ({ m, d });
                }
            std::sort (attacks.begin(), attacks.end(), [] (auto& a, auto& b) { return a.m < b.m; });
            std::vector<int> lower;   // notes already sounding or accepted this frame (for the harmonic test)
            for (int m = kLo; m <= kHi; ++m) if (live[(size_t) m].start >= 0) lower.push_back (m);
            int started = 0;
            for (auto& a : attacks)
            {
                const int m = a.m;
                const bool modelSaysSo = modelSupports (f, m);
                // the window keeps "rising" for ~100 ms after an attack: no spectral re-trigger in that time
                if (f - lastOnsetSpec[(size_t) m] < (modelSaysSo ? minGap : 12)) continue;
                if (! modelSaysSo && explainedByHarmonic (f, m, lower)) continue;
                if (started >= kMaxPerAttack && ! modelSaysSo) continue;
                const float settled = settledDb (f, m);
                if (settled < fmax - 30.f) continue;
                auto& L = live[(size_t) m];
                if (modelSaysSo && L.start >= 0 && L.start >= f - 8 && ! L.fromModel)
                {
                    // the spectral rise fired early (window smear); the model's timing is the real attack
                    L.start = f; L.fromModel = true; L.below = 0;
                    lastOnsetSpec[(size_t) m] = f;
                    lower.push_back (m);
                    continue;
                }
                finish (m, f);
                live[(size_t) m] = { f, settled, 0, -1, modelSaysSo };
                lastOnsetSpec[(size_t) m] = f;
                lower.push_back (m);
                ++started;
                // high harmonics rise a frame or two before the fundamental: anything at a harmonic interval above
                // this note that started in the last few frames, is not louder, and has no model onset, was its harmonic
                for (int h = m + 12; h <= kHi; ++h)
                {
                    auto& H = live[(size_t) h];
                    if (H.start < 0 || H.start < f - 8 || H.start == f || ! isHarmonicInterval (h - m)) continue;
                    if (H.fromModel || sg.at (f, h) > sg.at (f, m) + 2.f) continue;
                    H.start = -1;
                }
            }
            // sustain / release
            for (int m = kLo; m <= kHi; ++m)
            {
                auto& L = live[(size_t) m];
                if (L.start < 0 || L.start == f) continue;
                if (f < L.start + 8) continue;    // settling period
                const float d = sg.at (f, m);
                const bool gone = d < L.startDb - kHoldDb || d < floorDb || ! (d >= sg.at (f, m - 1) - 1.f && d >= sg.at (f, m + 1) - 1.f);
                // a momentary dip (string beating, tremolo) must not end the note: it has to stay gone for ~80 ms
                if (gone) { if (L.below++ == 0) L.firstBelow = f; if (L.below >= 7) finish (m, L.firstBelow); }
                else L.below = 0;
            }
        }
        for (int m = kLo; m <= kHi; ++m) finish (m, live[(size_t) m].below > 0 ? live[(size_t) m].firstBelow : nSpec - 1);
        // the 186 ms analysis window smears an attack onto the neighbouring semitone a few frames early: drop the
        // weaker of two notes a semitone apart that start within ~50 ms of each other
        for (auto& n : notes)
            for (auto& m : notes)
                if (&n != &m && n.alive && m.alive && std::abs (n.pitch - m.pitch) == 1 && std::abs (n.s - m.s) <= 8 && n.amp < m.amp - 0.05f)
                { n.alive = false; break; }
        // two starts of one pitch within ~90 ms = one attack seen twice (spectral precursor + model): keep one
        std::sort (notes.begin(), notes.end(), [] (auto& a, auto& b) { return a.pitch < b.pitch || (a.pitch == b.pitch && a.s < b.s); });
        for (size_t i = 0; i + 1 < notes.size(); ++i)
        {
            auto& a = notes[i]; auto& b = notes[i + 1];
            if (a.alive && b.alive && a.pitch == b.pitch && b.s - a.s <= 8) { a.e = juce::jmax (a.e, b.e); a.amp = juce::jmax (a.amp, b.amp); b.alive = false; }
        }
        if (opts.debug) std::printf ("    hybrid: %d raw notes (global max %.1f dB)\n", (int) notes.size(), globalMax);
    }

    auto onsetAt = [&] (const RawNote& n)
    {
        const int f = n.pitch - basic_pitch::constants::MIDI_OFFSET;
        if (f < 0 || f >= (int) result.onsets.dimension (1) || n.s < 0 || n.s >= nFrames) return 0.f;
        float v = 0.f;
        for (int t = juce::jmax (0, n.s - 1); t <= juce::jmin (nFrames - 1, n.s + 1); ++t) v = juce::jmax (v, result.onsets (t, f));
        return v;
    };
    auto dbg = [&] (const char* why, const RawNote& n)
    {
        if (opts.debug) std::printf ("    cleanup: %-14s %s%d @%.3f len %.3f amp %.2f\n", why, noteName (n.pitch), n.pitch / 12 - 1,
                                     n.s / 86.13, (n.e - n.s) / 86.13, n.amp);
    };

    static const int kHarmonics[] = { 12, 19, 24, 28, 31 };
    // an onset on a held note that lines up with a louder attack a harmonic interval above (or an octave below)
    // is that other note bleeding into this pitch, not a re-strike
    auto onsetExplained = [&] (const RawNote& b)
    {
        const float ob = onsetAt (b);
        for (auto& m : notes)
        {
            if (&m == &b || std::abs (m.s - b.s) > 2) continue;
            const int iv = m.pitch - b.pitch;
            const bool harmonic = std::find (std::begin (kHarmonics), std::end (kHarmonics), iv) != std::end (kHarmonics) || iv == -12;
            if (harmonic && onsetAt (m) >= ob * 0.9f) return true;
        }
        return false;
    };

    std::sort (notes.begin(), notes.end(), [] (auto& a, auto& b) { return a.pitch < b.pitch || (a.pitch == b.pitch && a.s < b.s); });

    if (opts.cleanup)
    {
        // 1. A held note that the model split where another note attacked: same pitch, (almost) no gap,
        //    and the second piece has no real onset of its own -> one note.
        for (size_t i = 0; i + 1 < notes.size(); ++i)
        {
            auto& a = notes[i];
            if (! a.alive) continue;
            for (size_t j = i + 1; j < notes.size() && notes[j].pitch == a.pitch; ++j)
            {
                auto& b = notes[j];
                if (! b.alive) continue;
                if (b.s - a.e > kFrameTol) break;
                if (onsetAt (b) > 0.6f && ! onsetExplained (b)) break;   // a genuine re-strike
                a.e = juce::jmax (a.e, b.e);
                a.amp = juce::jmax (a.amp, b.amp);
                b.alive = false;
                dbg ("joined", b);
            }
        }

        // 2. Octave / harmonic ghosts: a clearly weaker note that starts with a louder one at a harmonic
        //    interval (2nd..6th harmonic above, or an octave / two below).
        static const int kIntervals[] = { 12, 19, 24, 28, 31, -12, -24 };
        for (auto& n : notes)
        {
            if (! n.alive) continue;
            for (auto& m : notes)
            {
                if (! m.alive || &m == &n) continue;
                const int iv = n.pitch - m.pitch;
                if (std::find (std::begin (kIntervals), std::end (kIntervals), iv) == std::end (kIntervals)) continue;
                if (std::abs (n.s - m.s) > 2 * kFrameTol) continue;
                if (m.amp < n.amp * 1.4f) continue;
                if (m.e < n.e - 2 * kFrameTol && m.amp < n.amp * 2.f) continue;   // the loud one stops much earlier: probably two real notes
                n.alive = false;
                dbg ("ghost", n);
                break;
            }
        }
    }

    if (opts.mono)
    {
        std::sort (notes.begin(), notes.end(), [] (auto& a, auto& b) { return a.s < b.s || (a.s == b.s && a.amp > b.amp); });
        RawNote* cur = nullptr;
        for (auto& n : notes)
        {
            if (! n.alive) continue;
            if (cur != nullptr && n.s - cur->s <= kFrameTol)
            {
                // two notes at once: keep the louder
                if (n.amp > cur->amp) { cur->alive = false; dbg ("mono (quieter)", *cur); cur = &n; }
                else { n.alive = false; dbg ("mono (quieter)", n); }
                continue;
            }
            if (cur != nullptr && cur->e > n.s) cur->e = n.s;
            cur = &n;
        }
        for (auto& n : notes) if (n.alive && n.e - n.s < kFrameTol) { n.alive = false; dbg ("mono (cut)", n); }
    }

    auto times = basic_pitch::frame_times (nFrames + 1);
    for (auto& n : notes)
    {
        if (! n.alive) continue;
        const double start = juce::jmax (0.0, (double) times[(size_t) juce::jlimit (0, nFrames, n.s)]);
        const double end = times[(size_t) juce::jlimit (0, nFrames, n.e)];
        out.notes.push_back ({ start, juce::jmax (0.03, end - start), n.pitch, juce::jlimit (20, 127, (int) std::round (n.amp * 127.f)) });
    }
    std::sort (out.notes.begin(), out.notes.end(), [] (auto& a, auto& b) { return a.startSec < b.startSec || (a.startSec == b.startSec && a.pitch < b.pitch); });
    if (out.notes.empty()) { error = "No notes detected"; return false; }
    return true;
}

// ---------------------------------------------------------------------------
NoteList snapToGrid (const NoteList& in, double bpm, int snapDiv)
{
    if (snapDiv <= 0 || bpm <= 0.0) return in;
    NoteList out = in;
    const double spb = 60.0 / bpm, step = spb / snapDiv;
    for (auto& n : out.notes)
    {
        const double start = std::round (n.startSec / step) * step;
        double end = std::round ((n.startSec + n.lenSec) / step) * step;
        if (end < start + step * 0.999) end = start + step;
        n.startSec = start;
        n.lenSec = end - start;
    }
    std::sort (out.notes.begin(), out.notes.end(), [] (auto& a, auto& b) { return a.startSec < b.startSec || (a.startSec == b.startSec && a.pitch < b.pitch); });
    // two hits of one pitch that snapped onto the same step collapse into one; an overlap trims the first
    std::vector<NoteList::Note> clean;
    for (auto& n : out.notes)
    {
        bool merged = false;
        for (auto it = clean.rbegin(); it != clean.rend() && it->startSec + it->lenSec > n.startSec - 1e-6; ++it)
        {
            if (it->pitch != n.pitch) continue;
            if (std::abs (it->startSec - n.startSec) < 1e-6) { it->lenSec = juce::jmax (it->lenSec, n.lenSec); it->velocity = juce::jmax (it->velocity, n.velocity); merged = true; }
            else if (it->startSec + it->lenSec > n.startSec + 1e-6) it->lenSec = n.startSec - it->startSec;
            break;
        }
        if (! merged) clean.push_back (n);
    }
    out.notes.swap (clean);
    return out;
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
