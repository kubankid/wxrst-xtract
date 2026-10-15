#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <vector>

// Audio -> MIDI. Pitched material goes through Spotify's Basic Pitch model (ONNX Runtime),
// drums go through our own 3-band onset detector that maps hits to GM drum notes.
struct NoteList
{
    struct Note { double startSec, lenSec; int pitch; int velocity; };
    std::vector<Note> notes;
    bool isDrums = false;
};

namespace Transcriber
{
    // Must be called once before the first ONNX call (Windows: finds onnxruntime.dll next to the plugin).
    void initRuntime();

    bool notesFromAudio (const juce::AudioBuffer<float>& in, double rate, NoteList& out, juce::String& error);
    bool drumsFromAudio (const juce::AudioBuffer<float>& in, double rate, NoteList& out, juce::String& error);

    // Writes a type-0 MIDI file at the given tempo so beat positions line up with the audio in the DAW.
    bool writeMidiFile (const NoteList& notes, double bpm, const juce::File& dest);
}
