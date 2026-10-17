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

    struct NoteOptions
    {
        bool cleanup = true;   // drop octave / harmonic ghosts, re-join held notes the model split at other notes' attacks
        bool mono = false;     // one note at a time (bass lines, leads): the louder of two near-simultaneous notes wins
        bool debug = false;    // print what the cleanup removed (console tests)
        bool hybrid = true;    // polyphonic: model finds the attacks, the audio's own spectrum fills in chord tones + durations
    };
    bool notesFromAudio (const juce::AudioBuffer<float>& in, double rate, NoteList& out, juce::String& error, NoteOptions opts = {});
    bool drumsFromAudio (const juce::AudioBuffer<float>& in, double rate, NoteList& out, juce::String& error);

    // Snaps note starts and ends to a 1/snapDiv-beat grid at the given tempo (0 = leave as detected).
    NoteList snapToGrid (const NoteList& notes, double bpm, int snapDiv);

    // Writes a type-0 MIDI file at the given tempo so beat positions line up with the audio in the DAW.
    bool writeMidiFile (const NoteList& notes, double bpm, const juce::File& dest);
}
