#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>
#include <functional>
#include <vector>

// Wraps demucs.cpp (Hybrid Transformer Demucs v4) for stem separation.
// Everything here runs synchronously and is meant to be called from a worker thread.
struct StemSet
{
    std::vector<juce::String> names;                 // "drums", "bass", "other", "vocals" [, "guitar", "piano"]
    std::vector<juce::AudioBuffer<float>> audio;     // stereo, 44100 Hz, same length as the input region
};

namespace Separator
{
    constexpr double kModelRate = 44100.0;

    // Where the ggml weight files live (user-writable, outside the bundle so the repo/CI stays small).
    juce::File modelsDir();
    juce::File modelFile (bool sixStem);
    bool modelAvailable (bool sixStem);
    juce::URL modelURL (bool sixStem);
    juce::int64 modelSizeBytes (bool sixStem);

    using Progress = std::function<void (float, const juce::String&)>;

    // in: 1 or 2 channels at any sample rate. Output stems are stereo 44.1 kHz.
    bool separate (const juce::AudioBuffer<float>& in, double inRate, bool sixStem, int threads,
                   const Progress& progress, StemSet& out, juce::String& error);

    // Helpers shared with the transcriber / processor
    juce::AudioBuffer<float> resample (const juce::AudioBuffer<float>& in, double inRate, double outRate, int outChannels);
    juce::Colour stemColour (const juce::String& name);
}
