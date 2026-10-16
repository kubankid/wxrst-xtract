// Bench: renders known MIDI through a cheap synth, runs the transcriber, scores it.
//   NotesBench                -> synthetic suites
//   NotesBench <audio> [bpm]  -> prints the notes for a real file (grid offsets at the given bpm)
#include <juce_audio_formats/juce_audio_formats.h>
#include "Transcriber.h"
#include <cstdio>
#include <cmath>

struct GT { double t, len; int pitch; };
static const char* NAMES[] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };

// additive-ish synth: harmonics with 1/n rolloff + exp decay, 44.1k stereo
static juce::AudioBuffer<float> render (const std::vector<GT>& gt, double secs, double decay, int nHarm, float noise = 0.f)
{
    const double sr = 44100.0;
    juce::AudioBuffer<float> b (2, (int) (sr * secs)); b.clear();
    juce::Random r (3);
    for (auto& n : gt)
    {
        const double f = 440.0 * std::pow (2.0, (n.pitch - 69) / 12.0);
        const int a = (int) (n.t * sr), e = juce::jmin (b.getNumSamples(), (int) ((n.t + n.len) * sr));
        for (int i = a; i < e; ++i)
        {
            const double t = (i - a) / sr, rel = (e - i) / sr;
            double v = 0;
            for (int h = 1; h <= nHarm; ++h) v += std::sin (2 * juce::MathConstants<double>::pi * f * h * t) / h * std::exp (-t * decay * h * 0.5);
            v *= juce::jmin (1.0, t * 400.0) * juce::jmin (1.0, rel * 50.0) * 0.25;
            b.addSample (0, i, (float) v); b.addSample (1, i, (float) v);
        }
    }
    if (noise > 0) for (int i = 0; i < b.getNumSamples(); ++i) { float x = (r.nextFloat() * 2 - 1) * noise; b.addSample (0, i, x); b.addSample (1, i, x); }
    return b;
}

static void score (const char* name, const std::vector<GT>& gt, const NoteList& got, bool verbose)
{
    int hit = 0; double offSum = 0, offAbs = 0; std::vector<bool> used (got.notes.size(), false);
    int octaveErr = 0;
    for (auto& g : gt)
    {
        int best = -1; double bd = 1e9;
        for (size_t i = 0; i < got.notes.size(); ++i)
        {
            if (used[i] || got.notes[i].pitch != g.pitch) continue;
            const double d = std::abs (got.notes[i].startSec - g.t);
            if (d < 0.08 && d < bd) { bd = d; best = (int) i; }
        }
        if (best >= 0) { used[(size_t) best] = true; ++hit; offSum += got.notes[(size_t) best].startSec - g.t; offAbs += bd; }
        else if (verbose) std::printf ("    MISS %s%d @%.3f\n", NAMES[g.pitch % 12], g.pitch / 12 - 1, g.t);
    }
    for (size_t i = 0; i < got.notes.size(); ++i) if (! used[i])
    {
        auto& n = got.notes[i];
        bool oct = false;
        for (auto& g : gt) if (std::abs (n.startSec - g.t) < 0.08 && (n.pitch - g.pitch) % 12 == 0) oct = true;
        octaveErr += oct;
        if (verbose) std::printf ("    EXTRA %s%d @%.3f len %.3f v%d%s\n", NAMES[n.pitch % 12], n.pitch / 12 - 1, n.startSec, n.lenSec, n.velocity, oct ? " (octave)" : "");
    }
    const int extra = (int) got.notes.size() - hit;
    const double p = got.notes.empty() ? 0 : hit / (double) got.notes.size(), rc = hit / (double) gt.size();
    std::printf ("  %-28s recall %3.0f%%  precision %3.0f%%  F %.2f  (%d gt, %d got, %d extra of which %d octave)  mean onset offset %+.1f ms (abs %.1f)\n",
                 name, rc * 100, p * 100, p + rc > 0 ? 2 * p * rc / (p + rc) : 0, (int) gt.size(), (int) got.notes.size(), extra, octaveErr,
                 hit ? offSum / hit * 1000 : 0.0, hit ? offAbs / hit * 1000 : 0.0);
}

int main (int argc, char** argv)
{
    bool verbose = false, raw = false, mono = false; int snap = 0; std::vector<juce::String> args;
    for (int i = 1; i < argc; ++i)
    {
        juce::String a (argv[i]);
        if (a == "-v") verbose = true; else if (a == "--raw") raw = true; else if (a == "--mono") mono = true;
        else if (a.startsWith ("--snap")) snap = a.fromFirstOccurrenceOf ("=", false, false).getIntValue();
        else args.push_back (a);
    }
    Transcriber::NoteOptions fileOpts; fileOpts.cleanup = ! raw; fileOpts.mono = mono; fileOpts.debug = verbose;
    if (! args.empty())
    {
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (juce::File (args[0])));
        if (! r) { std::printf ("cannot read\n"); return 1; }
        juce::AudioBuffer<float> a ((int) r->numChannels, (int) r->lengthInSamples);
        r->read (&a, 0, (int) r->lengthInSamples, 0, true, true);
        const double bpm = args.size() > 1 ? args[1].getDoubleValue() : 120.0;
        NoteList nl; juce::String e;
        if (! Transcriber::notesFromAudio (a, r->sampleRate, nl, e, fileOpts)) { std::printf ("fail: %s\n", e.toRawUTF8()); return 1; }
        if (snap > 0) nl = Transcriber::snapToGrid (nl, bpm, snap);
        if (args.size() > 2) Transcriber::writeMidiFile (nl, bpm, juce::File (args[2]));
        std::printf ("%d notes\n", (int) nl.notes.size());
        double gridAbs = 0;
        for (auto& n : nl.notes)
        {
            const double beat = n.startSec * bpm / 60.0, g = std::round (beat * 4) / 4;
            gridAbs += std::abs (beat - g);
            std::printf ("  %7.3f beat %6.2f (%+5.2f)  %-3s%d len %.2f v%d\n", n.startSec, beat, beat - g, NAMES[n.pitch % 12], n.pitch / 12 - 1, n.lenSec * bpm / 60.0, n.velocity);
        }
        std::printf ("mean |offset from 1/16 grid| = %.3f beats\n", gridAbs / juce::jmax<size_t> (1, nl.notes.size()));
        return 0;
    }

    // --- suite 1: monophonic lead, 16ths and 8ths at 140 bpm, E minor ---
    const double bpm = 140.0, spb = 60.0 / bpm;
    std::vector<GT> lead;
    { const int ps[] = { 76, 79, 83, 79, 86, 83, 79, 76, 74, 76, 79, 83, 88, 86, 83, 79 };
      double t = 0.25; for (int i = 0; i < 16; ++i) { const double len = (i % 4 == 3) ? spb : spb * 0.5; lead.push_back ({ t, len * 0.9, ps[i] }); t += len; } }
    // --- suite 2: block chords (triads + 7ths) every bar at 90 bpm ---
    std::vector<GT> chords;
    { const int c[4][4] = { { 57, 60, 64, 67 }, { 53, 57, 60, 64 }, { 60, 64, 67, 71 }, { 55, 59, 62, 65 } };
      for (int bar = 0; bar < 4; ++bar) for (int k = 0; k < 4; ++k) chords.push_back ({ 0.2 + bar * 4 * 60.0 / 90.0, 4 * 60.0 / 90.0 * 0.95, c[bar][k] }); }
    // --- suite 3: bass line with octave jumps at 140 bpm ---
    std::vector<GT> bass;
    { const int ps[] = { 40, 40, 52, 40, 43, 43, 55, 43, 45, 45, 57, 45, 47, 38, 50, 38 };
      double t = 0.25; for (int i = 0; i < 16; ++i) { bass.push_back ({ t, spb * 0.45, ps[i] }); t += spb * 0.5; } }
    // --- suite 4: comped chords + melody on top (the "other" stem case) ---
    std::vector<GT> mix = chords;
    for (auto& n : lead) mix.push_back ({ n.t + 0.2, n.len, n.pitch });

    struct Case { const char* name; std::vector<GT> gt; double secs, decay; int harm; float noise; bool mono; };
    std::vector<Case> cases = {
        { "lead saw-ish",           lead,   6.0, 1.0, 12, 0.f, true },
        { "lead + hiss",            lead,   6.0, 1.0, 12, 0.01f, true },
        { "block chords piano-ish", chords, 11.5, 2.5, 8, 0.f, false },
        { "bass octaves",           bass,   4.5, 0.8, 6, 0.f, true },
        { "chords + melody",        mix,    11.5, 1.5, 10, 0.f, false },
    };
    for (auto& c : cases)
    {
        auto audio = render (c.gt, c.secs, c.decay, c.harm, c.noise);
        for (int pass = 0; pass < (c.mono ? 3 : 2); ++pass)
        {
            Transcriber::NoteOptions o; o.cleanup = pass > 0; o.mono = pass == 2; o.debug = verbose && pass > 0;
            NoteList nl; juce::String e;
            if (verbose) std::printf ("%s %s\n", c.name, pass == 0 ? "RAW" : pass == 1 ? "CLEAN" : "CLEAN+MONO");
            if (! Transcriber::notesFromAudio (audio, 44100.0, nl, e, o)) { std::printf ("  %s: FAIL %s\n", c.name, e.toRawUTF8()); continue; }
            score ((juce::String (c.name) + (pass == 0 ? " RAW" : pass == 1 ? " CLEAN" : " MONO")).toRawUTF8(), c.gt, nl, verbose);
        }
    }
    return 0;
}
