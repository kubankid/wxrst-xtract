#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "Separator.h"
#include "Transcriber.h"
#include "Updater.h"
#include <atomic>
#include <memory>
#include <functional>

// One audio clip (the loaded / captured source, or one separated stem). Always stereo 44.1 kHz.
struct Clip
{
    juce::String name;                 // "source", "drums", ...
    juce::AudioBuffer<float> audio;
    std::vector<float> peaks;          // 1024 min/max pairs for drawing
    juce::File wavFile;                // written copy on disk (needed for drag-out)
    juce::File midiFile;               // after TO MIDI
    int midiNotes = 0;
    bool midiIsDrums = false;
    double lengthSeconds() const { return audio.getNumSamples() / Separator::kModelRate; }
    void buildPeaks();
};

struct Session
{
    std::shared_ptr<Clip> source;
    std::vector<std::shared_ptr<Clip>> stems;
    juce::File folder;                 // ~/Documents/WXRST/Xtract/<name>/
    bool sixStem = false;
};

enum class Target { Chords = 0, Melody, Drums };
constexpr int kMaxStems = 6;

class XtractProcessor : public juce::AudioProcessor, private juce::Thread
{
public:
    XtractProcessor();
    ~XtractProcessor() override;

    // ---- AudioProcessor ----
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "WXRST XTRACT"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // ---- session (message thread reads; worker swaps under the lock) ----
    std::shared_ptr<const Session> getSession() const { juce::SpinLock::ScopedLockType l (sessionLock); return session; }
    bool hasSource() const { auto s = getSession(); return s && s->source != nullptr; }

    // ---- source loading / capture ----
    bool loadSourceFile (const juce::File& f);           // async decode on the worker
    void setCapturing (bool on);                         // effect mode: record the input
    bool isCapturing() const { return capturing.load(); }
    double capturedSeconds() const { return captureLen.load() / (hostRate > 0 ? hostRate : 44100.0); }

    // ---- region (seconds in the source clip) ----
    std::atomic<double> regionStart { 0.0 }, regionEnd { 0.0 };
    void setRegion (double start, double end);
    void resetRegion();

    // ---- playback (audio thread) ----
    std::atomic<bool> playing { false }, looping { true }, playStems { false };
    std::atomic<double> playPos { 0.0 };                 // seconds in the clip
    std::atomic<double> stemOffset { 0.0 };              // stems cover the region only; region start at the time they were made
    std::atomic<float> stemGain[kMaxStems];
    std::atomic<bool> stemMute[kMaxStems], stemSolo[kMaxStems];
    std::atomic<float> outputLevel { 0.f };
    void togglePlay();
    void stop();

    // ---- jobs (run on the worker thread) ----
    bool isBusy() const { return busy.load(); }
    float jobProgress() const { return progress.load(); }
    juce::String jobStatus() const { juce::SpinLock::ScopedLockType l (statusLock); return status; }
    juce::String lastError() const { juce::SpinLock::ScopedLockType l (statusLock); return error; }
    void separate (bool sixStem);
    void toMidi (int row);                               // 0 = source, 1.. = stems
    void sendTo (int row, Target target);                // copies wav + mid into ~/Documents/WXRST/Inbox/<target>/
    static juce::File inboxDir (Target t);
    static double bpmFromName (const juce::String& name);   // 0 if the file name carries no tempo
    static juce::File exportsRoot();

    // ---- model download ----
    bool modelReady (bool six) const { return Separator::modelAvailable (six); }
    void downloadModel (bool six);
    bool isDownloading() const { return download != nullptr && ! download->isFinished(); }
    float downloadProgress() const;
    juce::String downloadError() const { juce::SpinLock::ScopedLockType l (statusLock); return dlError; }

    // MIDI conversion options (saved in state). mono also kicks in automatically for the bass stem.
    std::atomic<bool> midiSnap { true }, midiMono { false };
    std::atomic<int> midiVoicing { 0 };   // 0 ALL, 1 LEAD, 2 CHORDS, 3 MONO (model-only single line). Bass stem always MONO.
    double hostBpm() const { return bpm.load(); }
    void setManualBpm (double b) { manualBpm.store (b); }
    double manualBpmValue() const { return manualBpm.load(); }
    std::atomic<int> sessionVersion { 0 };               // bumps whenever the session changes (UI polls)
    Updater updater { "xtract", DENNY_VERSION, { "WXRST XTRACT.vst3", "WXRST XTRACT.component" }, "XTRACT" };

private:
    void run() override;
    void post (std::function<void()> job);
    void setStatus (const juce::String& s) { juce::SpinLock::ScopedLockType l (statusLock); status = s; }
    void setError (const juce::String& e) { juce::SpinLock::ScopedLockType l (statusLock); error = e; }
    void publish (std::shared_ptr<Session> s);
    std::shared_ptr<Clip> makeClip (const juce::String& name, juce::AudioBuffer<float>&& audio, const juce::File& folder);
    bool decodeFile (const juce::File& f, juce::AudioBuffer<float>& out, double& rate, juce::String& err);
    void finishCapture();
    juce::AudioBuffer<float> regionAudio (const Clip& c) const;
    static bool writeWav (const juce::AudioBuffer<float>& b, double rate, const juce::File& f);

    mutable juce::SpinLock sessionLock;
    std::shared_ptr<Session> session;

    // worker
    juce::CriticalSection jobLock;
    std::vector<std::function<void()>> jobs;
    std::atomic<bool> busy { false };
    std::atomic<float> progress { 0.f };
    mutable juce::SpinLock statusLock;
    juce::String status, error, dlError;

    // capture
    std::atomic<bool> capturing { false };
    juce::AudioBuffer<float> captureBuf;
    std::atomic<int> captureLen { 0 };
    std::atomic<bool> captureDone { false };
    double hostRate = 44100.0;
    std::atomic<double> bpm { 120.0 }, manualBpm { 0.0 };
    juce::AudioFormatManager formats;

    std::unique_ptr<juce::URL::DownloadTask> download;
    juce::File downloadTarget;
    bool downloadSix = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (XtractProcessor)
};
