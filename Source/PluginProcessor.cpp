#include "PluginProcessor.h"
#include "PluginEditor.h"

// ---------------------------------------------------------------------------
void Clip::buildPeaks()
{
    const int bins = 1024;
    peaks.assign ((size_t) bins * 2, 0.f);
    const int n = audio.getNumSamples();
    if (n == 0) return;
    for (int b = 0; b < bins; ++b)
    {
        const int a = (int) ((juce::int64) b * n / bins), e = juce::jmax (a + 1, (int) ((juce::int64) (b + 1) * n / bins));
        float mn = 0.f, mx = 0.f;
        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
        {
            auto r = audio.findMinMax (ch, a, e - a);
            mn = juce::jmin (mn, r.getStart()); mx = juce::jmax (mx, r.getEnd());
        }
        peaks[(size_t) b * 2] = mn; peaks[(size_t) b * 2 + 1] = mx;
    }
}

// ---------------------------------------------------------------------------
XtractProcessor::XtractProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      juce::Thread ("XTRACT worker")
{
    for (int i = 0; i < kMaxStems; ++i) { stemGain[i] = 1.f; stemMute[i] = false; stemSolo[i] = false; }
    formats.registerBasicFormats();
    captureBuf.setSize (2, 44100 * 120);
    startThread (juce::Thread::Priority::normal);

    if (auto env = juce::SystemStats::getEnvironmentVariable ("XTRACT_FILE", {}); env.isNotEmpty())
        loadSourceFile (juce::File (env));
}

XtractProcessor::~XtractProcessor()
{
    signalThreadShouldExit();
    notify();
    stopThread (30000);
}

juce::File XtractProcessor::exportsRoot()
{
    auto d = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("WXRST").getChildFile ("Xtract");
    d.createDirectory();
    return d;
}

juce::File XtractProcessor::inboxDir (Target t)
{
    static const char* names[] = { "Chords", "Melody", "Drums" };
    auto d = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("WXRST").getChildFile ("Inbox").getChildFile (names[(int) t]);
    d.createDirectory();
    return d;
}

// ---------------------------------------------------------------------------
void XtractProcessor::prepareToPlay (double sampleRate, int)
{
    hostRate = sampleRate;
    captureBuf.setSize (2, (int) (sampleRate * 120.0), false, true);
}

bool XtractProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    auto out = l.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::stereo() && out != juce::AudioChannelSet::mono()) return false;
    auto in = l.getMainInputChannelSet();
    return in == juce::AudioChannelSet::stereo() || in == juce::AudioChannelSet::mono() || in.isDisabled();
}

void XtractProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals nd;
    const int n = buffer.getNumSamples();
    const int outCh = getTotalNumOutputChannels();
    const int inCh = getTotalNumInputChannels();

    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
            if (auto b = pos->getBpm()) if (*b > 20.0) bpm.store (*b);

    // capture the input (effect mode)
    if (capturing.load())
    {
        const int len = captureLen.load();
        const int room = captureBuf.getNumSamples() - len;
        const int take = juce::jmin (n, room);
        if (take > 0 && inCh > 0)
        {
            for (int ch = 0; ch < 2; ++ch)
                captureBuf.copyFrom (ch, len, buffer, juce::jmin (ch, inCh - 1), 0, take);
            captureLen.store (len + take);
        }
        if (take < n) { capturing.store (false); captureDone.store (true); }
    }

    // input passes through; our playback is added on top
    for (int ch = inCh; ch < outCh; ++ch) buffer.clear (ch, 0, n);

    float peak = 0.f;
    if (playing.load())
    {
        std::shared_ptr<const Session> s;
        {
            juce::SpinLock::ScopedTryLockType l (sessionLock);
            if (l.isLocked()) s = session;
        }
        if (s && s->source)
        {
            const double rs = regionStart.load(), re = juce::jmax (rs + 0.05, regionEnd.load());
            double pos = playPos.load();
            if (pos < rs || pos >= re) pos = rs;
            const double step = 1.0 / hostRate;
            const bool useStems = playStems.load() && ! s->stems.empty();
            const double so = stemOffset.load();
            bool anySolo = false;
            if (useStems) for (size_t i = 0; i < s->stems.size() && i < kMaxStems; ++i) if (stemSolo[i].load()) anySolo = true;

            for (int i = 0; i < n; ++i)
            {
                float l = 0.f, r = 0.f;
                auto mix = [&] (const Clip& c, double offset, float g)
                {
                    const double idx = (pos - offset) * Separator::kModelRate;
                    const int i0 = (int) idx;
                    if (idx < 0.0 || i0 + 1 >= c.audio.getNumSamples()) return;
                    const float fr = (float) (idx - i0);
                    const float* L = c.audio.getReadPointer (0);
                    const float* R = c.audio.getReadPointer (juce::jmin (1, c.audio.getNumChannels() - 1));
                    l += (L[i0] + (L[i0 + 1] - L[i0]) * fr) * g;
                    r += (R[i0] + (R[i0 + 1] - R[i0]) * fr) * g;
                };
                if (useStems)
                {
                    for (size_t k = 0; k < s->stems.size() && k < kMaxStems; ++k)
                    {
                        if (stemMute[k].load()) continue;
                        if (anySolo && ! stemSolo[k].load()) continue;
                        mix (*s->stems[k], so, stemGain[k].load());
                    }
                }
                else mix (*s->source, 0.0, 1.f);

                if (outCh > 0) buffer.addSample (0, i, l);
                if (outCh > 1) buffer.addSample (1, i, r);
                peak = juce::jmax (peak, std::abs (l), std::abs (r));
                pos += step;
                if (pos >= re)
                {
                    if (looping.load()) pos = rs;
                    else { playing.store (false); pos = rs; break; }
                }
            }
            playPos.store (pos);
        }
    }
    outputLevel.store (peak);
}

void XtractProcessor::togglePlay()
{
    if (playing.load()) { playing.store (false); return; }
    playPos.store (regionStart.load());
    playing.store (true);
}

void XtractProcessor::stop() { playing.store (false); playPos.store (regionStart.load()); }

void XtractProcessor::setRegion (double start, double end)
{
    auto s = getSession();
    const double len = (s && s->source) ? s->source->lengthSeconds() : 0.0;
    start = juce::jlimit (0.0, len, start);
    end = juce::jlimit (start + 0.05, len, end);
    regionStart.store (start); regionEnd.store (end);
}

void XtractProcessor::resetRegion()
{
    auto s = getSession();
    regionStart.store (0.0);
    regionEnd.store ((s && s->source) ? s->source->lengthSeconds() : 0.0);
}

// ---------------------------------------------------------------------------
void XtractProcessor::post (std::function<void()> job)
{
    { const juce::ScopedLock l (jobLock); jobs.push_back (std::move (job)); }
    notify();
}

void XtractProcessor::run()
{
    while (! threadShouldExit())
    {
        std::function<void()> job;
        {
            const juce::ScopedLock l (jobLock);
            if (! jobs.empty()) { job = std::move (jobs.front()); jobs.erase (jobs.begin()); }
        }
        if (! job) { wait (200); continue; }
        busy.store (true);
        progress.store (0.f);
        setError ({});
        job();
        busy.store (false);
        progress.store (0.f);
        setStatus ({});
    }
}

void XtractProcessor::publish (std::shared_ptr<Session> s)
{
    { juce::SpinLock::ScopedLockType l (sessionLock); session = std::move (s); }
    ++sessionVersion;
}

bool XtractProcessor::writeWav (const juce::AudioBuffer<float>& b, double rate, const juce::File& f)
{
    f.getParentDirectory().createDirectory();
    f.deleteFile();
    juce::WavAudioFormat wav;
    auto* os = new juce::FileOutputStream (f);
    if (! os->openedOk()) { delete os; return false; }
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os, rate, (unsigned) b.getNumChannels(), 24, {}, 0));
    return w != nullptr && w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
}

std::shared_ptr<Clip> XtractProcessor::makeClip (const juce::String& name, juce::AudioBuffer<float>&& audio, const juce::File& folder)
{
    auto c = std::make_shared<Clip>();
    c->name = name;
    c->audio = std::move (audio);
    c->buildPeaks();
    c->wavFile = folder.getChildFile (name + ".wav");
    writeWav (c->audio, Separator::kModelRate, c->wavFile);
    return c;
}

bool XtractProcessor::decodeFile (const juce::File& f, juce::AudioBuffer<float>& out, double& rate, juce::String& err)
{
    std::unique_ptr<juce::AudioFormatReader> r (formats.createReaderFor (f));
    if (! r) { err = "Can't read " + f.getFileName(); return false; }
    const juce::int64 maxLen = (juce::int64) (r->sampleRate * 180.0);   // 3 minutes max
    const int len = (int) juce::jmin (r->lengthInSamples, maxLen);
    out.setSize ((int) r->numChannels, len);
    r->read (&out, 0, len, 0, true, true);
    rate = r->sampleRate;
    return true;
}

juce::AudioBuffer<float> XtractProcessor::regionAudio (const Clip& c) const
{
    const int a = juce::jlimit (0, c.audio.getNumSamples(), (int) (regionStart.load() * Separator::kModelRate));
    const int e = juce::jlimit (a, c.audio.getNumSamples(), (int) (regionEnd.load() * Separator::kModelRate));
    juce::AudioBuffer<float> b (2, juce::jmax (0, e - a));
    for (int ch = 0; ch < 2; ++ch) b.copyFrom (ch, 0, c.audio, juce::jmin (ch, c.audio.getNumChannels() - 1), a, e - a);
    return b;
}

// ---------------------------------------------------------------------------
bool XtractProcessor::loadSourceFile (const juce::File& f)
{
    if (! f.existsAsFile()) return false;
    post ([this, f]
    {
        setStatus ("Loading " + f.getFileName());
        juce::AudioBuffer<float> raw; double rate = 44100.0; juce::String err;
        if (! decodeFile (f, raw, rate, err)) { setError (err); return; }
        auto audio = Separator::resample (raw, rate, Separator::kModelRate, 2);
        auto s = std::make_shared<Session>();
        auto base = juce::File::createLegalFileName (f.getFileNameWithoutExtension());
        s->folder = exportsRoot().getChildFile (base);
        s->folder.createDirectory();
        s->source = makeClip ("source", std::move (audio), s->folder);
        s->source->name = base;
        playing.store (false);
        playStems.store (false);
        publish (s);
        resetRegion();
    });
    return true;
}

void XtractProcessor::setCapturing (bool on)
{
    if (on)
    {
        playing.store (false);
        captureLen.store (0);
        captureDone.store (false);
        capturing.store (true);
    }
    else
    {
        capturing.store (false);
        finishCapture();
    }
}

void XtractProcessor::finishCapture()
{
    const int len = captureLen.load();
    if (len < 4096) return;
    post ([this, len]
    {
        setStatus ("Preparing capture");
        juce::AudioBuffer<float> raw (2, len);
        for (int ch = 0; ch < 2; ++ch) raw.copyFrom (ch, 0, captureBuf, ch, 0, len);
        auto audio = Separator::resample (raw, hostRate, Separator::kModelRate, 2);
        auto s = std::make_shared<Session>();
        auto base = "Capture " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H%M%S");
        s->folder = exportsRoot().getChildFile (base);
        s->folder.createDirectory();
        s->source = makeClip ("source", std::move (audio), s->folder);
        s->source->name = base;
        playStems.store (false);
        publish (s);
        resetRegion();
    });
}

// ---------------------------------------------------------------------------
void XtractProcessor::separate (bool six)
{
    post ([this, six]
    {
        auto old = getSession();
        if (! old || ! old->source) { setError ("Load a sample first"); return; }
        if (! Separator::modelAvailable (six)) { setError ("Download the model first"); return; }
        playing.store (false);
        auto audio = regionAudio (*old->source);
        StemSet stems; juce::String err;
        setStatus ("Separating");
        const int threads = juce::jlimit (1, 4, juce::SystemStats::getNumCpus() / 2);
        bool ok = Separator::separate (audio, Separator::kModelRate, six, threads,
            [this] (float p, const juce::String&) { progress.store (p); setStatus ("Separating " + juce::String ((int) (p * 100)) + "%"); },
            stems, err);
        if (! ok) { setError (err); return; }
        setStatus ("Writing stems");
        auto s = std::make_shared<Session> (*old);
        s->stems.clear();
        s->sixStem = six;
        for (size_t i = 0; i < stems.names.size(); ++i)
            s->stems.push_back (makeClip (stems.names[i], std::move (stems.audio[i]), s->folder));
        for (int i = 0; i < kMaxStems; ++i) { stemMute[i] = false; stemSolo[i] = false; stemGain[i] = 1.f; }
        stemOffset.store (regionStart.load());   // stems cover the region only
        playStems.store (true);
        publish (s);
    });
}

void XtractProcessor::toMidi (int row)
{
    post ([this, row]
    {
        auto old = getSession();
        if (! old || ! old->source) { setError ("Load a sample first"); return; }
        std::shared_ptr<Clip> clip = row == 0 ? old->source : (row - 1 < (int) old->stems.size() ? old->stems[(size_t) row - 1] : nullptr);
        if (! clip) return;
        const bool drums = clip->name == "drums";
        setStatus (drums ? "Detecting drum hits" : "Transcribing notes");
        progress.store (0.3f);
        NoteList notes; juce::String err;
        juce::AudioBuffer<float> audio = row == 0 ? regionAudio (*clip) : clip->audio;
        const bool ok = drums ? Transcriber::drumsFromAudio (audio, Separator::kModelRate, notes, err)
                              : Transcriber::notesFromAudio (audio, Separator::kModelRate, notes, err);
        if (! ok) { setError (err); return; }
        const double tempo = manualBpm.load() > 0.0 ? manualBpm.load() : bpm.load();
        auto mid = old->folder.getChildFile (clip->name + ".mid");
        if (! Transcriber::writeMidiFile (notes, tempo, mid)) { setError ("Could not write MIDI file"); return; }
        auto nc = std::make_shared<Clip> (*clip);
        nc->midiFile = mid;
        nc->midiNotes = (int) notes.notes.size();
        nc->midiIsDrums = notes.isDrums;
        auto s = std::make_shared<Session> (*old);
        if (row == 0) s->source = nc; else s->stems[(size_t) row - 1] = nc;
        publish (s);
    });
}

void XtractProcessor::sendTo (int row, Target target)
{
    auto s = getSession();
    if (! s || ! s->source) return;
    std::shared_ptr<Clip> clip = row == 0 ? s->source : (row - 1 < (int) s->stems.size() ? s->stems[(size_t) row - 1] : nullptr);
    if (! clip) return;
    auto dir = inboxDir (target);
    auto base = juce::File::createLegalFileName (s->source->name + " - " + clip->name);
    if (row == 0)
        writeWav (regionAudio (*clip), Separator::kModelRate, dir.getChildFile (base + ".wav"));   // the selected region
    else if (clip->wavFile.existsAsFile())
        clip->wavFile.copyFileTo (dir.getChildFile (base + ".wav"));
    if (clip->midiFile.existsAsFile())
        clip->midiFile.copyFileTo (dir.getChildFile (base + ".mid"));
}

// ---------------------------------------------------------------------------
void XtractProcessor::downloadModel (bool six)
{
    if (isDownloading()) return;
    downloadSix = six;
    downloadTarget = Separator::modelFile (six);
    auto part = downloadTarget.getSiblingFile (downloadTarget.getFileName() + ".part");
    part.deleteFile();
    { juce::SpinLock::ScopedLockType l (statusLock); dlError = {}; }
    download = Separator::modelURL (six).downloadToFile (part, juce::URL::DownloadTaskOptions());
    if (! download) { juce::SpinLock::ScopedLockType l (statusLock); dlError = "Could not start download"; }
}

float XtractProcessor::downloadProgress() const
{
    if (! download) return 0.f;
    if (download->isFinished())
    {
        auto* self = const_cast<XtractProcessor*> (this);
        auto part = downloadTarget.getSiblingFile (downloadTarget.getFileName() + ".part");
        if (download->hadError() || part.getSize() < 1000000)
        {
            juce::SpinLock::ScopedLockType l (statusLock);
            self->dlError = "Download failed - check your internet connection";
            part.deleteFile();
        }
        else
        {
            downloadTarget.deleteFile();
            part.moveFileTo (downloadTarget);
        }
        self->download.reset();
        return 1.f;
    }
    const auto total = download->getTotalLength();
    const auto got = download->getLengthDownloaded();
    const auto expect = total > 0 ? total : Separator::modelSizeBytes (downloadSix);
    return expect > 0 ? juce::jlimit (0.f, 0.99f, (float) got / (float) expect) : 0.f;
}

// ---------------------------------------------------------------------------
void XtractProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    juce::XmlElement xml ("XTRACT");
    if (auto s = getSession(); s && s->source)
    {
        xml.setAttribute ("folder", s->folder.getFullPathName());
        xml.setAttribute ("name", s->source->name);
        xml.setAttribute ("six", s->sixStem);
        xml.setAttribute ("regionStart", regionStart.load());
        xml.setAttribute ("regionEnd", regionEnd.load());
        xml.setAttribute ("stemOffset", stemOffset.load());
        xml.setAttribute ("sourceMidi", s->source->midiFile.getFullPathName());
        for (auto& st : s->stems)
        {
            auto* e = xml.createNewChildElement ("stem");
            e->setAttribute ("name", st->name);
            e->setAttribute ("midi", st->midiFile.getFullPathName());
        }
    }
    xml.setAttribute ("manualBpm", manualBpm.load());
    copyXmlToBinary (xml, dest);
}

void XtractProcessor::setStateInformation (const void* data, int size)
{
    auto xml = getXmlFromBinary (data, size);
    if (! xml || ! xml->hasTagName ("XTRACT")) return;
    manualBpm.store (xml->getDoubleAttribute ("manualBpm", 0.0));
    juce::File folder (xml->getStringAttribute ("folder"));
    if (! folder.isDirectory()) return;
    const juce::String name = xml->getStringAttribute ("name");
    const bool six = xml->getBoolAttribute ("six");
    const double rs = xml->getDoubleAttribute ("regionStart"), re = xml->getDoubleAttribute ("regionEnd");
    const double so = xml->getDoubleAttribute ("stemOffset", rs);
    juce::File sourceMidi (xml->getStringAttribute ("sourceMidi"));
    std::vector<std::pair<juce::String, juce::File>> stemInfo;
    for (auto* e : xml->getChildWithTagNameIterator ("stem"))
        stemInfo.push_back ({ e->getStringAttribute ("name"), juce::File (e->getStringAttribute ("midi")) });

    post ([this, folder, name, six, rs, re, so, sourceMidi, stemInfo]
    {
        setStatus ("Restoring session");
        auto loadClip = [this, folder] (const juce::String& n) -> std::shared_ptr<Clip>
        {
            juce::AudioBuffer<float> raw; double rate = 44100.0; juce::String err;
            auto f = folder.getChildFile (n + ".wav");
            if (! decodeFile (f, raw, rate, err)) return nullptr;
            auto c = std::make_shared<Clip>();
            c->name = n; c->audio = Separator::resample (raw, rate, Separator::kModelRate, 2); c->wavFile = f; c->buildPeaks();
            return c;
        };
        auto s = std::make_shared<Session>();
        s->folder = folder; s->sixStem = six;
        s->source = loadClip ("source");
        if (! s->source) { setError ("Previous sample is gone: " + folder.getFullPathName()); return; }
        s->source->name = name;
        if (sourceMidi.existsAsFile()) s->source->midiFile = sourceMidi;
        for (auto& [n, mid] : stemInfo)
            if (auto c = loadClip (n)) { if (mid.existsAsFile()) c->midiFile = mid; s->stems.push_back (c); }
        publish (s);
        regionStart.store (rs); regionEnd.store (re > rs ? re : s->source->lengthSeconds());
        stemOffset.store (so);
        playStems.store (! s->stems.empty());
    });
}

juce::AudioProcessorEditor* XtractProcessor::createEditor() { return new XtractEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new XtractProcessor(); }
