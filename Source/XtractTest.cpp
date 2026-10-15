// Console test: XtractTest <audio file> [--6] [out dir]
// Separates the file into stems, transcribes each one, and reports timings + note counts.
#include <juce_audio_formats/juce_audio_formats.h>
#include "Separator.h"
#include "Transcriber.h"

static bool writeWav (const juce::AudioBuffer<float>& b, double rate, const juce::File& f)
{
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (f), rate, (unsigned) b.getNumChannels(), 24, {}, 0));
    return w != nullptr && w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
}

int main (int argc, char** argv)
{
    if (argc < 2) { std::cout << "usage: XtractTest <audio file> [--6] [out dir]\n"; return 1; }
    bool six = false, drumsOnly = false; juce::String outDir;
    for (int i = 2; i < argc; ++i) { juce::String a (argv[i]); if (a == "--6") six = true; else if (a == "--drums") drumsOnly = true; else outDir = a; }
    juce::File in { juce::String (argv[1]) };
    juce::File out = outDir.isEmpty() ? in.getParentDirectory().getChildFile (in.getFileNameWithoutExtension() + " stems") : juce::File (outDir);
    out.createDirectory();

    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (in));
    if (! r) { std::cout << "cannot read " << in.getFullPathName() << "\n"; return 1; }
    juce::AudioBuffer<float> audio ((int) r->numChannels, (int) r->lengthInSamples);
    r->read (&audio, 0, (int) r->lengthInSamples, 0, true, true);
    std::cout << "Loaded " << in.getFileName() << ": " << r->lengthInSamples / r->sampleRate << " s, " << r->numChannels << " ch @ " << r->sampleRate << "\n";

    if (drumsOnly)
    {
        NoteList notes; juce::String e;
        if (! Transcriber::drumsFromAudio (audio, r->sampleRate, notes, e)) { std::cout << "no MIDI: " << e << "\n"; return 1; }
        int k = 0, s = 0, h = 0, o = 0;
        for (auto& nt : notes.notes) { if (nt.pitch == 36) ++k; else if (nt.pitch == 38) ++s; else if (nt.pitch == 42) ++h; else ++o; }
        std::cout << notes.notes.size() << " hits: kick " << k << ", snare " << s << ", hat " << h << ", open " << o << "\n";
        for (auto& nt : notes.notes) if (nt.startSec < 4.0)
            std::cout << "  " << juce::String (nt.startSec, 3) << "  " << (nt.pitch == 36 ? "KICK" : nt.pitch == 38 ? "SNARE" : nt.pitch == 42 ? "hat" : "OPEN") << "  v" << nt.velocity << "\n";
        Transcriber::writeMidiFile (notes, 120.0, out.getChildFile ("drums.mid"));
        return 0;
    }
    StemSet stems; juce::String err;
    auto t0 = juce::Time::getMillisecondCounterHiRes();
    float lastShown = -1.f;
    bool ok = Separator::separate (audio, r->sampleRate, six, 4, [&] (float p, const juce::String&)
    {
        if (p - lastShown >= 0.1f) { std::cout << "  " << (int) (p * 100) << "%" << std::flush; lastShown = p; }
    }, stems, err);
    auto t1 = juce::Time::getMillisecondCounterHiRes();
    std::cout << "\nSeparation " << (ok ? "ok" : ("FAILED: " + err).toStdString()) << " in " << (t1 - t0) / 1000.0 << " s\n";
    if (! ok) return 1;

    double bpm = 120.0;
    for (size_t i = 0; i < stems.names.size(); ++i)
    {
        auto wavF = out.getChildFile (stems.names[i] + ".wav");
        writeWav (stems.audio[i], 44100.0, wavF);
        NoteList notes; juce::String e;
        auto a = juce::Time::getMillisecondCounterHiRes();
        bool got = stems.names[i] == "drums" ? Transcriber::drumsFromAudio (stems.audio[i], 44100.0, notes, e)
                                             : Transcriber::notesFromAudio (stems.audio[i], 44100.0, notes, e);
        auto b = juce::Time::getMillisecondCounterHiRes();
        float rms = stems.audio[i].getRMSLevel (0, 0, stems.audio[i].getNumSamples());
        std::cout << "  " << stems.names[i].paddedRight (' ', 7) << " rms " << juce::Decibels::gainToDecibels (rms) << " dB  ";
        if (got)
        {
            Transcriber::writeMidiFile (notes, bpm, out.getChildFile (stems.names[i] + ".mid"));
            std::cout << notes.notes.size() << " notes in " << (b - a) / 1000.0 << " s";
            if (notes.isDrums)
            {
                int k = 0, s = 0, h = 0, o = 0;
                for (auto& nt : notes.notes) { if (nt.pitch == 36) ++k; else if (nt.pitch == 38) ++s; else if (nt.pitch == 42) ++h; else ++o; }
                std::cout << " (kick " << k << ", snare " << s << ", hat " << h << ", open " << o << ")";
            }
        }
        else std::cout << "no MIDI: " << e;
        std::cout << "\n";
    }
    std::cout << "Wrote to " << out.getFullPathName() << "\n";
    return 0;
}
