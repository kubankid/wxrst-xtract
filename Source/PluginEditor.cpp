#include "PluginEditor.h"

using namespace IceColours;

static juce::String secsToStr (double s)
{
    const int m = (int) (s / 60.0);
    return juce::String (m) + ":" + juce::String (s - m * 60.0, 2).paddedLeft ('0', 5);
}

// ---------------------------------------------------------------------------
void Panel::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (panel);
    g.fillRoundedRectangle (r, 8.f);
    g.setColour (panelEdge);
    g.drawRoundedRectangle (r.reduced (0.5f), 8.f, 1.f);
    auto titleArea = r.removeFromTop (22.f);
    g.setColour (ice.withAlpha (0.08f));
    g.fillRoundedRectangle (titleArea.withTrimmedBottom (-6.f), 8.f);
    g.setColour (ice);
    g.setFont (juce::Font (juce::FontOptions (11.f, juce::Font::bold)));
    g.drawText (title, titleArea.toNearestInt().withTrimmedLeft (10), juce::Justification::centredLeft);
    g.setColour (ice.withAlpha (0.25f));
    g.drawLine (r.getX() + 8.f, titleArea.getBottom(), r.getRight() - 8.f, titleArea.getBottom(), 1.f);
}

// ---------------------------------------------------------------------------
void WaveView::setClip (std::shared_ptr<const Clip> c, juce::Colour col, double off)
{
    clip = std::move (c); colour = col; offset = off; repaint();
}

double WaveView::xToSec (float x) const
{
    if (! clip) return 0.0;
    return offset + juce::jlimit (0.0, 1.0, (double) x / (double) juce::jmax (1, getWidth())) * clip->lengthSeconds();
}

float WaveView::secToX (double s) const
{
    if (! clip || clip->lengthSeconds() <= 0.0) return 0.f;
    return (float) ((s - offset) / clip->lengthSeconds()) * (float) getWidth();
}

void WaveView::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (screen);
    g.fillRoundedRectangle (r, 6.f);

    if (! clip || clip->peaks.empty())
    {
        g.setColour (panelEdge);
        g.drawRoundedRectangle (r.reduced (0.5f), 6.f, 1.f);
        g.setColour (dimText);
        g.setFont (juce::Font (juce::FontOptions (12.f, juce::Font::bold)));
        g.drawText (editable ? "DROP A SAMPLE HERE" : "-", getLocalBounds(), juce::Justification::centred);
        return;
    }

    const int w = getWidth(), h = getHeight();
    const float mid = (float) h * 0.5f, amp = (float) h * 0.46f;
    const int bins = (int) clip->peaks.size() / 2;

    // region shading (source only)
    const double rs = proc.regionStart.load(), re = proc.regionEnd.load();
    if (editable)
    {
        g.setColour (ice.withAlpha (0.07f));
        g.fillRect (juce::Rectangle<float> (secToX (rs), 0.f, secToX (re) - secToX (rs), (float) h));
    }

    juce::Path p;
    for (int x = 0; x < w; ++x)
    {
        const int b = juce::jlimit (0, bins - 1, (int) ((juce::int64) x * bins / juce::jmax (1, w)));
        const float mn = clip->peaks[(size_t) b * 2], mx = clip->peaks[(size_t) b * 2 + 1];
        p.addRectangle ((float) x, mid - mx * amp, 1.f, juce::jmax (1.f, (mx - mn) * amp));
    }
    if (editable)
    {
        // dim outside the region
        g.setColour (colour.withAlpha (0.28f));
        g.fillPath (p);
        g.saveState();
        g.reduceClipRegion (juce::Rectangle<float> (secToX (rs), 0.f, secToX (re) - secToX (rs), (float) h).toNearestInt());
        g.setColour (colour.withAlpha (0.9f));
        g.fillPath (p);
        g.restoreState();
        g.setColour (ice);
        g.fillRect (juce::Rectangle<float> (secToX (rs) - 1.f, 0.f, 2.f, (float) h));
        g.fillRect (juce::Rectangle<float> (secToX (re) - 1.f, 0.f, 2.f, (float) h));
    }
    else
    {
        g.setColour (colour.withAlpha (0.85f));
        g.fillPath (p);
    }

    // playhead
    if (proc.playing.load())
    {
        const double pos = proc.playPos.load();
        const float x = secToX (pos);
        if (x >= 0.f && x <= (float) w)
        {
            g.setColour (iceGlow);
            g.fillRect (juce::Rectangle<float> (x - 0.5f, 0.f, 1.5f, (float) h));
        }
    }
    g.setColour (panelEdge);
    g.drawRoundedRectangle (r.reduced (0.5f), 6.f, 1.f);
}

void WaveView::timerCallback()
{
    const bool pl = proc.playing.load();
    const double pos = proc.playPos.load();
    if (pl || lastPlaying || (editable && (std::abs (pos - lastPlayPos) > 1e-6)))
        repaint();
    lastPlaying = pl; lastPlayPos = pos;
}

void WaveView::mouseMove (const juce::MouseEvent& e)
{
    if (! editable || ! clip) return;
    const float xs = secToX (proc.regionStart.load()), xe = secToX (proc.regionEnd.load());
    const bool nearEdge = std::abs (e.x - xs) < 6.f || std::abs (e.x - xe) < 6.f;
    setMouseCursor (nearEdge ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::IBeamCursor);
}

void WaveView::mouseDown (const juce::MouseEvent& e)
{
    if (! editable || ! clip) return;
    const float xs = secToX (proc.regionStart.load()), xe = secToX (proc.regionEnd.load());
    if (std::abs (e.x - xs) < 6.f) dragMode = 2;
    else if (std::abs (e.x - xe) < 6.f) dragMode = 3;
    else { dragMode = 1; dragAnchor = xToSec ((float) e.x); }
}

void WaveView::mouseDrag (const juce::MouseEvent& e)
{
    if (! editable || ! clip || dragMode == 0) return;
    const double s = xToSec ((float) e.x);
    if (dragMode == 1) proc.setRegion (juce::jmin (dragAnchor, s), juce::jmax (dragAnchor, s));
    else if (dragMode == 2) proc.setRegion (juce::jmin (s, proc.regionEnd.load() - 0.05), proc.regionEnd.load());
    else proc.setRegion (proc.regionStart.load(), juce::jmax (s, proc.regionStart.load() + 0.05));
    repaint();
}

void WaveView::mouseUp (const juce::MouseEvent&)
{
    if (dragMode == 1 && editable && clip && std::abs (proc.regionEnd.load() - proc.regionStart.load()) < 0.06)
        proc.resetRegion();  // a plain click clears the selection
    dragMode = 0;
    repaint();
}

// ---------------------------------------------------------------------------
void FileDragPad::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    const bool has = file.existsAsFile();
    g.setColour (screen);
    g.fillRoundedRectangle (r, 6.f);
    g.setColour (has ? ice.withAlpha (0.6f) : panelEdge);
    g.drawRoundedRectangle (r.reduced (0.5f), 6.f, 1.f);
    g.setColour (has ? ice : dimText);
    g.setFont (juce::Font (juce::FontOptions (10.f, juce::Font::bold)));
    g.drawText (has ? label : juce::String ("-"), getLocalBounds(), juce::Justification::centred);
}

void FileDragPad::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging || ! file.existsAsFile() || e.getDistanceFromDragStart() < 8) return;
    dragging = true;
    if (auto* c = juce::DragAndDropContainer::findParentDragContainerFor (this))
        c->performExternalDragDropOfFiles ({ file.getFullPathName() }, false, this, [this] { dragging = false; });
}

// ---------------------------------------------------------------------------
StemRow::StemRow (XtractProcessor& p, int idx) : index (idx), proc (p), wave (p, false)
{
    addAndMakeVisible (wave);
    for (auto* b : { &mute, &solo, &toMidi, &sendChords, &sendMelody, &sendDrums }) addAndMakeVisible (*b);
    addAndMakeVisible (audioPad); addAndMakeVisible (midiPad);
    addAndMakeVisible (level);
    level.setSliderStyle (juce::Slider::LinearHorizontal);
    level.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    level.setRange (0.0, 1.5, 0.01);
    level.setValue (1.0, juce::dontSendNotification);
    level.setColour (juce::Slider::trackColourId, iceDeep);
    level.setColour (juce::Slider::thumbColourId, iceGlow);
    level.setColour (juce::Slider::backgroundColourId, knobRing);
    level.onValueChange = [this] { proc.stemGain[index].store ((float) level.getValue()); };
    mute.setClickingTogglesState (true);
    solo.setClickingTogglesState (true);
    mute.onClick = [this] { proc.stemMute[index].store (mute.getToggleState()); };
    solo.onClick = [this] { proc.stemSolo[index].store (solo.getToggleState()); };
    toMidi.onClick = [this] { proc.toMidi (index + 1); };
    sendChords.onClick = [this] { proc.sendTo (index + 1, Target::Chords); };
    sendMelody.onClick = [this] { proc.sendTo (index + 1, Target::Melody); };
    sendDrums.onClick  = [this] { proc.sendTo (index + 1, Target::Drums); };
    setEnabled (false);
}

void StemRow::update (std::shared_ptr<const Clip> c, double offset)
{
    clip = std::move (c);
    const bool has = clip != nullptr;
    setEnabled (has);
    colour = has ? Separator::stemColour (clip->name) : dimText;
    wave.setClip (clip, colour, offset);
    audioPad.setFile (has ? clip->wavFile : juce::File());
    midiPad.setFile (has ? clip->midiFile : juce::File());
    mute.setToggleState (proc.stemMute[index].load(), juce::dontSendNotification);
    solo.setToggleState (proc.stemSolo[index].load(), juce::dontSendNotification);
    level.setValue (proc.stemGain[index].load(), juce::dontSendNotification);
    repaint();
}

void StemRow::resized()
{
    auto r = getLocalBounds().reduced (4, 3);
    r.removeFromLeft (78);                                  // name column (painted)
    wave.setBounds (r.removeFromLeft (292));
    r.removeFromLeft (6);
    mute.setBounds (r.removeFromLeft (26).reduced (0, 10));
    r.removeFromLeft (3);
    solo.setBounds (r.removeFromLeft (26).reduced (0, 10));
    r.removeFromLeft (4);
    level.setBounds (r.removeFromLeft (60).reduced (0, 12));
    r.removeFromLeft (8);
    toMidi.setBounds (r.removeFromLeft (64).reduced (0, 10));
    r.removeFromLeft (6);
    audioPad.setBounds (r.removeFromLeft (60).reduced (0, 8));
    r.removeFromLeft (4);
    midiPad.setBounds (r.removeFromLeft (60).reduced (0, 8));
    r.removeFromLeft (10);
    r.removeFromLeft (46);                                  // "-> WXRST" label (painted)
    sendChords.setBounds (r.removeFromLeft (60).reduced (0, 10));
    r.removeFromLeft (3);
    sendMelody.setBounds (r.removeFromLeft (60).reduced (0, 10));
    r.removeFromLeft (3);
    sendDrums.setBounds (r.removeFromLeft (60).reduced (0, 10));
}

void StemRow::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().reduced (4, 3);
    auto name = r.removeFromLeft (78);
    g.setColour (colour.withAlpha (clip ? 1.f : 0.3f));
    g.fillRoundedRectangle (name.removeFromLeft (4).toFloat().reduced (0.f, 8.f), 2.f);
    name.removeFromLeft (6);
    g.setColour (clip ? text : dimText);
    g.setFont (juce::Font (juce::FontOptions (12.f, juce::Font::bold)));
    g.drawText (clip ? clip->name.toUpperCase() : juce::String ("-"), name.removeFromTop (name.getHeight() / 2), juce::Justification::bottomLeft);
    g.setColour (dimText);
    g.setFont (juce::Font (juce::FontOptions (10.f)));
    juce::String sub;
    if (clip && clip->midiNotes > 0) sub = juce::String (clip->midiNotes) + (clip->midiIsDrums ? " hits" : " notes");
    else if (clip) sub = clip->name == "drums" ? "kick/snare/hat" : "polyphonic";
    g.drawText (sub, name, juce::Justification::topLeft);

    // "-> WXRST" label before the send buttons
    auto lab = juce::Rectangle<int> (sendChords.getX() - 48, r.getY(), 46, r.getHeight());
    g.setColour (clip ? ice : dimText);
    g.setFont (juce::Font (juce::FontOptions (9.f, juce::Font::bold)));
    g.drawText ("TO WXRST", lab, juce::Justification::centredRight);
}

// ---------------------------------------------------------------------------
XtractEditor::XtractEditor (XtractProcessor& p) : AudioProcessorEditor (&p), proc (p)
{
    setLookAndFeel (&lnf);
    addAndMakeVisible (sourceP);
    addAndMakeVisible (stemsP);

    for (auto* b : { &loadBtn, &recBtn, &playBtn, &loopBtn, &origBtn, &separateBtn, &srcMidiBtn, &srcChords, &srcMelody, &srcDrums, &snapBtn, &monoBtn })
        sourceP.addAndMakeVisible (*b);
    sourceP.addAndMakeVisible (sourceWave);
    sourceP.addAndMakeVisible (srcAudioPad);
    sourceP.addAndMakeVisible (srcMidiPad);
    sourceP.addAndMakeVisible (modelBox);
    sourceP.addAndMakeVisible (regionLabel);
    sourceP.addAndMakeVisible (sendLabel);
    sourceP.addAndMakeVisible (bpmLabel);
    sourceP.addAndMakeVisible (bpmCaption);
    bpmCaption.setText ("MIDI BPM", juce::dontSendNotification);
    bpmCaption.setFont (juce::Font (juce::FontOptions (9.f, juce::Font::bold)));
    bpmCaption.setColour (juce::Label::textColourId, dimText);
    bpmCaption.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (statusLabel);

    modelBox.addItem ("4 STEMS: drums / bass / vocals / other", 1);
    modelBox.addItem ("6 STEMS: + guitar / piano (slower, piano is hit or miss)", 2);
    modelBox.setSelectedId (1, juce::dontSendNotification);
    modelBox.onChange = [this] { updateSeparateButton(); };

    loopBtn.setClickingTogglesState (true);
    loopBtn.setToggleState (true, juce::dontSendNotification);
    loopBtn.onClick = [this] { proc.looping.store (loopBtn.getToggleState()); };
    recBtn.setClickingTogglesState (true);
    recBtn.onClick = [this]
    {
        proc.setCapturing (recBtn.getToggleState());
        recBtn.setButtonText (recBtn.getToggleState() ? "STOP" : "REC");
    };
    playBtn.onClick = [this] { proc.togglePlay(); };
    origBtn.onClick = [this]
    {
        proc.playStems.store (! proc.playStems.load());
        refreshSession();
    };
    loadBtn.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Load a sample", juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                       "*.wav;*.aif;*.aiff;*.mp3;*.flac;*.ogg;*.m4a;*.caf");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc) { if (fc.getResult().existsAsFile()) proc.loadSourceFile (fc.getResult()); });
    };
    separateBtn.onClick = [this]
    {
        const bool six = modelBox.getSelectedId() == 2;
        if (! proc.modelReady (six)) proc.downloadModel (six);
        else proc.separate (six);
        updateSeparateButton();
    };
    srcMidiBtn.onClick = [this] { proc.toMidi (0); };
    srcChords.onClick = [this] { proc.sendTo (0, Target::Chords); };
    srcMelody.onClick = [this] { proc.sendTo (0, Target::Melody); };
    srcDrums.onClick  = [this] { proc.sendTo (0, Target::Drums); };

    snapBtn.setClickingTogglesState (true);
    snapBtn.setToggleState (proc.midiSnap.load(), juce::dontSendNotification);
    snapBtn.setTooltip ("Snap every note to the nearest 1/16 at the MIDI BPM. Off = timing exactly as played.");
    snapBtn.onClick = [this] { proc.midiSnap.store (snapBtn.getToggleState()); };
    monoBtn.setClickingTogglesState (true);
    monoBtn.setToggleState (proc.midiMono.load(), juce::dontSendNotification);
    monoBtn.setTooltip ("One note at a time (leads, vocals, bass lines). The bass stem is always mono.");
    monoBtn.onClick = [this] { proc.midiMono.store (monoBtn.getToggleState()); };
    bpmLabel.setEditable (true);
    bpmLabel.setJustificationType (juce::Justification::centred);
    bpmLabel.setColour (juce::Label::backgroundColourId, knobBody);
    bpmLabel.setColour (juce::Label::outlineColourId, panelEdge);
    bpmLabel.setFont (juce::Font (juce::FontOptions (12.f, juce::Font::bold)));
    bpmLabel.setTooltip ("Tempo written into the MIDI files. Blank = follow the host.");
    bpmLabel.onTextChange = [this]
    {
        const double v = bpmLabel.getText().retainCharacters ("0123456789.").getDoubleValue();
        proc.setManualBpm (v >= 20.0 && v <= 300.0 ? v : 0.0);
    };
    regionLabel.setFont (juce::Font (juce::FontOptions (11.f)));
    regionLabel.setColour (juce::Label::textColourId, dimText);
    sendLabel.setText ("TO WXRST", juce::dontSendNotification);
    sendLabel.setFont (juce::Font (juce::FontOptions (9.f, juce::Font::bold)));
    sendLabel.setColour (juce::Label::textColourId, ice);
    sendLabel.setJustificationType (juce::Justification::centredRight);
    statusLabel.setFont (juce::Font (juce::FontOptions (11.f, juce::Font::bold)));
    statusLabel.setColour (juce::Label::textColourId, iceGlow);
    statusLabel.setJustificationType (juce::Justification::centredRight);

    for (int i = 0; i < kMaxStems; ++i)
    {
        auto* row = rows.add (new StemRow (proc, i));
        stemsP.addAndMakeVisible (row);
    }

    styleButton (separateBtn, true);
    styleButton (recBtn, false);

    startTimerHz (10);
    refreshSession();
    updateSeparateButton();
    setSize (980, 640);
}

XtractEditor::~XtractEditor()
{
    setLookAndFeel (nullptr);
}

void XtractEditor::styleButton (juce::TextButton& b, bool accent)
{
    if (accent)
    {
        b.setColour (juce::TextButton::buttonColourId, iceDeep.withAlpha (0.55f));
        b.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    }
}

void XtractEditor::resized()
{
    auto r = getLocalBounds().reduced (10);
    auto header = r.removeFromTop (58);
    statusLabel.setBounds (header.removeFromRight (520).withTrimmedTop (18));

    sourceP.setBounds (r.removeFromTop (196));
    r.removeFromTop (8);
    stemsP.setBounds (r);

    // --- source panel ---
    auto s = sourceP.getLocalBounds().reduced (8).withTrimmedTop (24);
    auto row1 = s.removeFromTop (26);
    loadBtn.setBounds (row1.removeFromLeft (64)); row1.removeFromLeft (4);
    recBtn.setBounds (row1.removeFromLeft (64)); row1.removeFromLeft (10);
    playBtn.setBounds (row1.removeFromLeft (64)); row1.removeFromLeft (4);
    loopBtn.setBounds (row1.removeFromLeft (54)); row1.removeFromLeft (4);
    origBtn.setBounds (row1.removeFromLeft (118)); row1.removeFromLeft (10);
    regionLabel.setBounds (row1.removeFromLeft (150));
    separateBtn.setBounds (row1.removeFromRight (150)); row1.removeFromRight (6);
    modelBox.setBounds (row1.removeFromRight (280));
    s.removeFromTop (6);
    sourceWave.setBounds (s.removeFromTop (96));
    s.removeFromTop (6);
    auto row3 = s.removeFromTop (26);
    srcMidiBtn.setBounds (row3.removeFromLeft (74)); row3.removeFromLeft (6);
    srcAudioPad.setBounds (row3.removeFromLeft (80)); row3.removeFromLeft (4);
    srcMidiPad.setBounds (row3.removeFromLeft (80)); row3.removeFromLeft (10);
    sendLabel.setBounds (row3.removeFromLeft (56)); row3.removeFromLeft (4);
    srcChords.setBounds (row3.removeFromLeft (64)); row3.removeFromLeft (3);
    srcMelody.setBounds (row3.removeFromLeft (64)); row3.removeFromLeft (3);
    srcDrums.setBounds (row3.removeFromLeft (64));
    bpmLabel.setBounds (row3.removeFromRight (110));
    bpmCaption.setBounds (row3.removeFromRight (64)); row3.removeFromRight (10);
    monoBtn.setBounds (row3.removeFromRight (56)); row3.removeFromRight (4);
    snapBtn.setBounds (row3.removeFromRight (78));

    // --- stem rows ---
    auto st = stemsP.getLocalBounds().reduced (6).withTrimmedTop (24);
    const int rh = juce::jmax (40, st.getHeight() / kMaxStems);
    for (auto* row : rows) row->setBounds (st.removeFromTop (rh));
}

void XtractEditor::paint (juce::Graphics& g)
{
    g.fillAll (bg);
    // soft aurora haze behind everything
    juce::ColourGradient haze (iceDeep.withAlpha (0.16f), 120.f, 0.f, juce::Colours::transparentBlack, 700.f, 420.f, true);
    g.setGradientFill (haze);
    g.fillRect (getLocalBounds());

    auto header = getLocalBounds().reduced (10).removeFromTop (58);
    g.setColour (iceGlow);
    g.setFont (juce::Font (juce::FontOptions (30.f, juce::Font::bold)));
    g.drawText ("WXRST XTRACT", header.withTrimmedLeft (4).removeFromTop (36), juce::Justification::centredLeft);
    g.setColour (dimText);
    g.setFont (juce::Font (juce::FontOptions (10.f, juce::Font::bold)));
    g.drawText ("SAMPLE EXTRACTOR  /  STEM SPLITTER  /  AUDIO TO MIDI  -  SEND IT INTO WXRST", header.withTrimmedLeft (6).withTrimmedTop (38), juce::Justification::centredLeft);

    // progress bar under the status text
    const bool busy = proc.isBusy() || proc.isDownloading();
    if (busy)
    {
        const float p = proc.isDownloading() ? proc.downloadProgress() : proc.jobProgress();
        auto bar = statusLabel.getBounds().withTrimmedTop (statusLabel.getHeight() - 5).withHeight (3).toFloat();
        g.setColour (knobRing);
        g.fillRoundedRectangle (bar, 1.5f);
        g.setColour (ice);
        g.fillRoundedRectangle (bar.withWidth (juce::jmax (4.f, bar.getWidth() * juce::jlimit (0.02f, 1.f, p))), 1.5f);
    }

    if (dropHover)
    {
        g.setColour (ice.withAlpha (0.12f));
        g.fillRoundedRectangle (sourceP.getBounds().toFloat(), 8.f);
        g.setColour (ice);
        g.drawRoundedRectangle (sourceP.getBounds().toFloat().reduced (1.f), 8.f, 2.f);
    }
}

bool XtractEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files) if (juce::File (f).hasFileExtension ("wav;aif;aiff;mp3;flac;ogg;m4a;caf")) return true;
    return false;
}

void XtractEditor::filesDropped (const juce::StringArray& files, int, int)
{
    dropHover = false;
    for (auto& f : files) if (proc.loadSourceFile (juce::File (f))) break;
    repaint();
}

void XtractEditor::updateSeparateButton()
{
    const bool six = modelBox.getSelectedId() == 2;
    if (proc.isDownloading())
        separateBtn.setButtonText ("DOWNLOADING " + juce::String ((int) (proc.downloadProgress() * 100)) + "%");
    else if (! proc.modelReady (six))
        separateBtn.setButtonText (six ? "GET 6-STEM MODEL (55 MB)" : "GET 4-STEM MODEL (84 MB)");
    else
        separateBtn.setButtonText ("SEPARATE");
    separateBtn.setEnabled (! proc.isBusy() && ! proc.isDownloading() && (proc.hasSource() || ! proc.modelReady (six)));
}

void XtractEditor::refreshSession()
{
    auto s = proc.getSession();
    const bool has = s && s->source;
    sourceWave.setClip (has ? s->source : nullptr, ice, 0.0);
    srcAudioPad.setFile (has ? s->source->wavFile : juce::File());
    srcMidiPad.setFile (has ? s->source->midiFile : juce::File());
    const double so = proc.stemOffset.load();
    for (int i = 0; i < kMaxStems; ++i)
        rows[i]->update (has && i < (int) s->stems.size() ? s->stems[(size_t) i] : nullptr, so);
    const bool stems = has && ! s->stems.empty();
    origBtn.setEnabled (stems);
    origBtn.setButtonText (proc.playStems.load() && stems ? "HEAR: STEMS" : "HEAR: ORIGINAL");
    for (auto* b : { &playBtn, &srcMidiBtn, &srcChords, &srcMelody, &srcDrums }) b->setEnabled (has);
    updateSeparateButton();
    repaint();
}

void XtractEditor::timerCallback()
{
    // XTRACT_AUTO=1: separate the loaded file, then TO MIDI every row (headless smoke test)
    if (juce::SystemStats::getEnvironmentVariable ("XTRACT_AUTO", {}).isNotEmpty() && ! proc.isBusy())
    {
        auto s = proc.getSession();
        if (s && s->source && autoStage == 0) { autoStage = 1; proc.separate (false); }
        else if (s && ! s->stems.empty() && autoStage == 1)
        {
            autoStage = 2;
            proc.toMidi (0);
            for (int i = 0; i < (int) s->stems.size(); ++i) proc.toMidi (i + 1);
        }
        else if (autoStage == 2 && s && s->source->midiFile.existsAsFile()) autoStage = 3;
    }
    // XTRACT_SNAPSHOT=<png path>: render the editor to a file (for checking the UI without a screen)
    const bool autoMode = juce::SystemStats::getEnvironmentVariable ("XTRACT_AUTO", {}).isNotEmpty();
    if (auto snap = juce::SystemStats::getEnvironmentVariable ("XTRACT_SNAPSHOT", {});
        snap.isNotEmpty() && ((! autoMode && ++snapshotTicks == 25) || (autoMode && autoStage == 3 && ++snapshotTicks == 5)))
    {
        auto img = createComponentSnapshot (getLocalBounds(), false, 2.f);
        juce::File f (snap);
        f.deleteFile();
        juce::FileOutputStream os (f);
        juce::PNGImageFormat().writeImageToStream (img, os);
    }

    const int v = proc.sessionVersion.load();
    if (v != seenVersion) { seenVersion = v; refreshSession(); }

    const bool busy = proc.isBusy();
    if (busy != lastBusy) { lastBusy = busy; updateSeparateButton(); for (auto* r : rows) r->setEnabled (! busy && r->isEnabled()); }
    if (proc.isDownloading()) updateSeparateButton();
    else if (separateBtn.getButtonText().startsWith ("DOWNLOADING")) updateSeparateButton();

    juce::String msg;
    if (proc.isDownloading()) msg = "Downloading model " + juce::String ((int) (proc.downloadProgress() * 100)) + "%";
    else if (busy) msg = proc.jobStatus();
    else if (proc.lastError().isNotEmpty()) msg = "! " + proc.lastError();
    else if (proc.downloadError().isNotEmpty()) msg = "! " + proc.downloadError();
    else if (proc.isCapturing()) msg = "RECORDING " + secsToStr (proc.capturedSeconds());
    if (msg != statusLabel.getText()) statusLabel.setText (msg, juce::dontSendNotification);
    if (proc.isCapturing() && recBtn.getButtonText() != "STOP") recBtn.setButtonText ("STOP");
    if (! proc.isCapturing() && recBtn.getToggleState()) { recBtn.setToggleState (false, juce::dontSendNotification); recBtn.setButtonText ("REC"); }

    playBtn.setButtonText (proc.playing.load() ? "STOP" : "PLAY");
    playBtn.setToggleState (proc.playing.load(), juce::dontSendNotification);

    const double rs = proc.regionStart.load(), re = proc.regionEnd.load();
    auto s = proc.getSession();
    juce::String reg;
    if (s && s->source)
    {
        const bool whole = rs <= 0.0 && re >= s->source->lengthSeconds() - 0.01;
        reg = (whole ? "whole clip  " : "region  ") + secsToStr (rs) + " - " + secsToStr (re) + "  (" + juce::String (re - rs, 1) + " s)";
    }
    if (reg != regionLabel.getText()) regionLabel.setText (reg, juce::dontSendNotification);

    if (! bpmLabel.isBeingEdited())
    {
        const double mb = proc.manualBpmValue();
        auto t = mb > 0.0 ? juce::String (mb, 1) : juce::String (proc.hostBpm(), 1) + " (host)";
        if (t != bpmLabel.getText()) bpmLabel.setText (t, juce::dontSendNotification);
    }
    if (busy || proc.isDownloading()) repaint (statusLabel.getBounds());
}
