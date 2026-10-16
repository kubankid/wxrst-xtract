#pragma once
#include "PluginProcessor.h"
#include "LookAndFeel.h"

struct Panel : public juce::Component
{
    Panel (const juce::String& title) : title (title) {}
    void paint (juce::Graphics& g) override;
    juce::String title;
};

// Waveform of one clip. The source view also edits the region (click-drag to select, drag the edges to adjust).
class WaveView : public juce::Component, private juce::Timer
{
public:
    WaveView (XtractProcessor& p, bool editable) : proc (p), editable (editable) { startTimerHz (30); }
    void setClip (std::shared_ptr<const Clip> c, juce::Colour colour, double offsetSeconds);
    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent&) override { if (editable) { proc.resetRegion(); repaint(); } }
private:
    void timerCallback() override;
    double xToSec (float x) const;
    float secToX (double s) const;
    XtractProcessor& proc;
    bool editable;
    std::shared_ptr<const Clip> clip;
    juce::Colour colour { 0xff6fe3ff };
    double offset = 0.0;              // seconds: where this clip sits in the source timeline
    int dragMode = 0;                 // 0 none, 1 new selection, 2 move start, 3 move end
    double dragAnchor = 0.0;
    double lastPlayPos = -1.0;
    bool lastPlaying = false;
};

// Drag a file (WAV or MIDI) out into FL Studio
class FileDragPad : public juce::Component
{
public:
    FileDragPad (const juce::String& label) : label (label) {}
    void setFile (const juce::File& f) { file = f; repaint(); }
    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent&) override { dragging = false; }
    void mouseDrag (const juce::MouseEvent& e) override;
private:
    juce::String label;
    juce::File file;
    bool dragging = false;
};

// One stem: name, waveform, mute/solo, level, TO MIDI, drag pads, send buttons
class StemRow : public juce::Component
{
public:
    StemRow (XtractProcessor& p, int index);
    void resized() override;
    void paint (juce::Graphics& g) override;
    void update (std::shared_ptr<const Clip> clip, double offset);
    int index;
private:
    XtractProcessor& proc;
    std::shared_ptr<const Clip> clip;
    juce::Colour colour;
    WaveView wave;
    juce::TextButton mute { "M" }, solo { "S" }, toMidi { "TO MIDI" }, sendChords { "CHORDS" }, sendMelody { "MELODY" }, sendDrums { "DRUMS" };
    juce::Slider level;
    FileDragPad audioPad { "DRAG WAV" }, midiPad { "DRAG MIDI" };
};

class XtractEditor : public juce::AudioProcessorEditor, public juce::DragAndDropContainer,
                     public juce::FileDragAndDropTarget, private juce::Timer
{
public:
    explicit XtractEditor (XtractProcessor&);
    ~XtractEditor() override;
    void paint (juce::Graphics& g) override;
    void resized() override;
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray&, int, int) override { dropHover = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override { dropHover = false; repaint(); }
    void filesDropped (const juce::StringArray& files, int, int) override;

private:
    void timerCallback() override;
    void refreshSession();
    void updateSeparateButton();
    void styleButton (juce::TextButton& b, bool accent = false);

    XtractProcessor& proc;
    IceLookAndFeel lnf;
    std::unique_ptr<juce::FileChooser> chooser;
    bool dropHover = false;
    int seenVersion = -1;
    bool lastBusy = false;
    int snapshotTicks = 0, autoStage = 0;

    Panel sourceP { "SOURCE  -  drop a sample here (WAV / MP3 / AIFF / FLAC), or hit REC to grab what is playing through this insert" };
    Panel stemsP { "STEMS  -  separated parts of the selected region. M = mute, S = solo. TO MIDI gives you notes (drums become a kit pattern)." };

    juce::TextButton loadBtn { "LOAD" }, recBtn { "REC" }, playBtn { "PLAY" }, loopBtn { "LOOP" }, origBtn { "HEAR: ORIGINAL" },
                     separateBtn { "SEPARATE" }, srcMidiBtn { "TO MIDI" }, snapBtn { "SNAP 1/16" }, monoBtn { "MONO" }, srcChords { "CHORDS" }, srcMelody { "MELODY" }, srcDrums { "DRUMS" };
    juce::ComboBox modelBox;
    juce::Label bpmLabel, bpmCaption, statusLabel, regionLabel, sendLabel;
    WaveView sourceWave { proc, true };
    FileDragPad srcAudioPad { "DRAG WAV" }, srcMidiPad { "DRAG MIDI" };
    juce::OwnedArray<StemRow> rows;
};
