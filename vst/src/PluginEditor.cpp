#include "PluginEditor.h"

namespace sdf
{
namespace
{
    constexpr int kTopBarHeight = 58;
    constexpr int kOverviewHeight = 72;
    constexpr int kGap = 6;
} // namespace

SpectrogramDrawFilterEditor::SpectrogramDrawFilterEditor (SpectrogramDrawFilterProcessor& p)
    : AudioProcessorEditor (&p), processor (p)
{
    setLookAndFeel (&lookAndFeel);
    setWantsKeyboardFocus (true);

    auto& params = processor.getParameters();
    auto& ui = processor.getUiState();

    viewState.onChange = [this] {
        spectrogramView.repaint();
        overview.viewChanged();
    };

    addAndMakeVisible (spectrogramView);
    addAndMakeVisible (overview);

    spectrogramView.setBrushRadius ((float) ui.getProperty ("brushRadius", 20.0f));
    spectrogramView.onBrushRadiusChanged = [this] (float r) {
        processor.getUiState().setProperty ("brushRadius", r, nullptr);
        updateBrushLabel();
    };

    captureButton.setClickingTogglesState (false);
    captureButton.setTooltip ("Record the audio passing through the plugin so its spectrogram can be shown. "
                              "Press, then play the audio (or click Apply in Audacity).");
    captureButton.onClick = [this] {
        if (session != nullptr)
            session->setArmed (! session->isArmed());
    };

    clearAudioButton.setTooltip ("Discard the captured audio (your painting is kept)");
    clearAudioButton.onClick = [this] {
        if (session != nullptr)
            session->clearCapture();
    };

    undoButton.onClick = [this] { undo(); };
    redoButton.onClick = [this] { redo(); };
    clearPaintButton.setTooltip ("Erase all painting (undoable)");
    clearPaintButton.onClick = [this] {
        if (session != nullptr)
            session->mask.clear();
    };

    invertButton.setClickingTogglesState (true);
    invertButton.setTooltip ("Play everything except what you painted");
    invertAttachment = std::make_unique<Attachment::ButtonAttachment> (params, ParamIDs::invert, invertButton);

    infoButton.setClickingTogglesState (true);
    infoButton.setTooltip ("Show host diagnostics");
    infoButton.onClick = [this] {
        showDiagnostics = infoButton.getToggleState();
        repaint();
    };

    for (auto* s : { &bgGainSlider, &hardnessSlider })
    {
        s->setSliderStyle (juce::Slider::LinearHorizontal);
        s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 22);
    }
    bgGainSlider.setTooltip ("How much of the unpainted audio leaks through");
    hardnessSlider.setTooltip ("0 = soft, feathered brush; 100 = hard edge");
    bgGainAttachment = std::make_unique<Attachment::SliderAttachment> (params, ParamIDs::bgGain, bgGainSlider);
    hardnessAttachment = std::make_unique<Attachment::SliderAttachment> (params, ParamIDs::hardness, hardnessSlider);
    bgGainSlider.textFromValueFunction = [] (double v) { return v <= -144.0 ? juce::String ("-inf dB") : juce::String (v, 1) + " dB"; };
    bgGainSlider.updateText();

    fftCombo.addItemList ({ "512", "1024", "2048", "4096", "8192", "16384" }, 1);
    fftCombo.setTooltip ("Larger = sharper frequency detail but blurrier timing; smaller = the opposite");
    fftAttachment = std::make_unique<Attachment::ComboBoxAttachment> (params, ParamIDs::fftSize, fftCombo);

    scaleCombo.addItemList ({ "Log", "Linear", "Mel" }, 1);
    scaleAttachment = std::make_unique<Attachment::ComboBoxAttachment> (params, ParamIDs::freqScale, scaleCombo);

    for (auto* c : std::initializer_list<juce::Component*> { &captureButton, &clearAudioButton, &undoButton, &redoButton,
                                                             &clearPaintButton, &invertButton, &infoButton, &bgGainSlider,
                                                             &hardnessSlider, &fftCombo, &scaleCombo, &brushLabel })
        addAndMakeVisible (c);

    for (auto* l : { &bgGainLabel, &fftLabel, &hardnessLabel, &scaleLabel })
    {
        l->setFont (juce::FontOptions (11.0f));
        l->setColour (juce::Label::textColourId, Theme::textDim);
        addAndMakeVisible (l);
    }
    brushLabel.setColour (juce::Label::textColourId, Theme::textDim);
    brushLabel.setJustificationType (juce::Justification::centredRight);
    updateBrushLabel();

    syncSession();
    updateDocumentLength();

    if (ui.hasProperty ("viewStart"))
        viewState.setTimeRange (ui.getProperty ("viewStart"), ui.getProperty ("viewEnd"));
    if (ui.hasProperty ("normLow"))
        viewState.setFreqRange (ui.getProperty ("normLow"), ui.getProperty ("normHigh"));

    setResizable (true, true);
    setResizeLimits (720, 420, 4000, 3000);
    setSize ((int) ui.getProperty ("width", 1100), (int) ui.getProperty ("height", 700));
    startTimerHz (30);
}

SpectrogramDrawFilterEditor::~SpectrogramDrawFilterEditor()
{
    auto& ui = processor.getUiState();
    ui.setProperty ("viewStart", viewState.startSeconds, nullptr);
    ui.setProperty ("viewEnd", viewState.endSeconds, nullptr);
    ui.setProperty ("normLow", viewState.normLow, nullptr);
    ui.setProperty ("normHigh", viewState.normHigh, nullptr);
    setLookAndFeel (nullptr);
}

//==============================================================================
void SpectrogramDrawFilterEditor::paint (juce::Graphics& g)
{
    g.fillAll (Theme::background);
    g.setColour (Theme::panel);
    g.fillRect (getLocalBounds().removeFromTop (topBarHeight));
    g.setColour (Theme::panelEdge);
    g.drawHorizontalLine (topBarHeight - 1, 0.0f, (float) getWidth());
}

void SpectrogramDrawFilterEditor::paintOverChildren (juce::Graphics& g)
{
    if (! showDiagnostics)
        return;

    auto area = spectrogramView.getBounds().reduced (60, 16).removeFromTop (240).removeFromRight (520);
    g.setColour (juce::Colours::black.withAlpha (0.8f));
    g.fillRoundedRectangle (area.toFloat(), 6.0f);
    g.setColour (Theme::text);
    g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain));
    g.drawMultiLineText (diagnosticsText, area.getX() + 12, area.getY() + 20, area.getWidth() - 24);
}

void SpectrogramDrawFilterEditor::resized()
{
    auto& ui = processor.getUiState();
    ui.setProperty ("width", getWidth(), nullptr);
    ui.setProperty ("height", getHeight(), nullptr);

    // One row of controls when there is room, otherwise buttons on one row and parameters below.
    constexpr int kButtonsWidth = 84 + 86 + 52 + 52 + 82 + 60 + 6 * kGap + 3 * 10;
    constexpr int kParamsWidth = 180 + 80 + 130 + 76 + 5 * kGap + 28 + 70;
    const bool twoRows = getWidth() - 20 < kButtonsWidth + kParamsWidth;
    topBarHeight = twoRows ? 2 * kTopBarHeight - 20 : kTopBarHeight;

    auto bounds = getLocalBounds();
    auto bar = bounds.removeFromTop (topBarHeight).reduced (10, 6);

    juce::Rectangle<int> captions, controls;
    auto startRow = [&] (juce::Rectangle<int> row) {
        captions = row.removeFromTop (16);
        controls = row.withTrimmedTop (2);
    };

    auto place = [&] (juce::Component& c, int width, juce::Label* caption = nullptr) {
        auto r = controls.removeFromLeft (width);
        c.setBounds (r);
        if (caption != nullptr)
            caption->setBounds (r.getX(), captions.getY(), width, captions.getHeight());
        controls.removeFromLeft (kGap);
    };
    auto gap = [&] (int w) { controls.removeFromLeft (w); };

    if (twoRows)
    {
        // The buttons have no captions, so the first row is just the controls.
        controls = bar.removeFromTop (30);
        bar.removeFromTop (8);
    }
    else
    {
        startRow (bar);
    }

    place (captureButton, 84);
    place (clearAudioButton, 86);
    gap (10);
    place (undoButton, 52);
    place (redoButton, 52);
    place (clearPaintButton, 82);
    gap (10);
    place (invertButton, 60);
    gap (10);

    if (twoRows)
    {
        infoButton.setBounds (controls.removeFromRight (28));
        startRow (bar);
    }

    place (bgGainSlider, 180, &bgGainLabel);
    place (fftCombo, 80, &fftLabel);
    place (hardnessSlider, 130, &hardnessLabel);
    place (scaleCombo, 76, &scaleLabel);

    if (! twoRows)
    {
        infoButton.setBounds (controls.removeFromRight (28));
        controls.removeFromRight (kGap);
    }
    brushLabel.setBounds (controls);

    bounds.reduce (6, 6);
    overview.setBounds (bounds.removeFromBottom (kOverviewHeight));
    bounds.removeFromBottom (kGap);
    spectrogramView.setBounds (bounds);
}

bool SpectrogramDrawFilterEditor::keyPressed (const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();
    if (mods.isCommandDown() && key.getKeyCode() == 'Z')
    {
        mods.isShiftDown() ? redo() : undo();
        return true;
    }
    if (mods.isCommandDown() && key.getKeyCode() == 'Y')
    {
        redo();
        return true;
    }
    return false;
}

void SpectrogramDrawFilterEditor::undo()
{
    if (session != nullptr)
        session->mask.undo();
}

void SpectrogramDrawFilterEditor::redo()
{
    if (session != nullptr)
        session->mask.redo();
}

void SpectrogramDrawFilterEditor::updateBrushLabel()
{
    brushLabel.setText ("Brush " + juce::String (juce::roundToInt (spectrogramView.getBrushRadius() * 2)) + " px",
                        juce::dontSendNotification);
}

//==============================================================================
void SpectrogramDrawFilterEditor::syncSession()
{
    auto current = processor.getSession();
    if (current != session)
    {
        session = current;
        spectrogramView.setSession (session);
        overview.setSession (session);
    }
}

void SpectrogramDrawFilterEditor::updateDocumentLength()
{
    if (session == nullptr)
        return;

    const auto& capture = session->capture;
    const double captureSr = capture.getSampleRate();
    double total = capture.hasData() ? (double) capture.getEndSample() / captureSr : 0.0;

    const auto painted = session->mask.getPaintedColumnRange();
    if (! painted.isEmpty())
        total = juce::jmax (total, MaskGrid::colToSeconds (painted.getEnd()));

    viewState.setTotalSeconds (total > 0.0 ? total : 10.0);

    const double sr = capture.hasData() ? captureSr : processor.getCurrentSampleRate() > 0 ? processor.getCurrentSampleRate() : 44100.0;
    viewState.setMaxHz (sr / 2.0);
}

void SpectrogramDrawFilterEditor::timerCallback()
{
    syncSession();
    if (session == nullptr)
        return;

    auto& params = processor.getParameters();
    viewState.setScaleType ((FreqScaleType) (int) params.getRawParameterValue (ParamIDs::freqScale)->load());
    const bool invert = params.getRawParameterValue (ParamIDs::invert)->load() >= 0.5f;
    spectrogramView.setInvert (invert);
    overview.setInvert (invert);
    spectrogramView.setHardness (params.getRawParameterValue (ParamIDs::hardness)->load() / 100.0f);

    // Stop capturing once audio has stopped arriving for a while (e.g. playback stopped).
    const auto now = juce::Time::getMillisecondCounter();
    if (session->isArmed() && session->capture.hasData() && now - session->getLastCaptureTime() > 1500)
        session->setArmed (false);

    const bool armed = session->isArmed();
    captureButton.setButtonText (armed ? "Capturing" : "Capture");
    captureButton.setColour (juce::TextButton::buttonColourId, armed ? Theme::record : juce::Colour (0xff262a33));
    undoButton.setEnabled (session->mask.canUndo());
    redoButton.setEnabled (session->mask.canRedo());
    clearAudioButton.setEnabled (session->capture.hasData());

    updateDocumentLength();

    const auto loop = processor.getLoopRange();
    spectrogramView.setLoopRange (loop);
    overview.setLoopRange (loop);

    juce::String overlay;
    if (session->isLoading())
        overlay = "Loading captured audio...";
    else if (! session->capture.hasData())
        overlay = armed ? "Capturing - play the audio now (or click Apply in Audacity)."
                        : "No audio captured yet.\nPress Capture, then play the audio (or click Apply in Audacity) to see its spectrogram.";
    spectrogramView.setOverlayText (overlay);

    const auto playhead = processor.getDisplayPlayPosition();
    spectrogramView.refresh (playhead);
    overview.refresh (playhead);

    if (showDiagnostics)
    {
        const auto text = buildDiagnosticsText();
        if (text != diagnosticsText)
        {
            diagnosticsText = text;
            repaint (spectrogramView.getBounds());
        }
    }
}

juce::String SpectrogramDrawFilterEditor::buildDiagnosticsText() const
{
    const auto& d = processor.getDiagnostics();
    const double sr = d.sampleRate.load();
    auto secs = [sr] (juce::int64 s) { return sr > 0 ? juce::String ((double) s / sr, 3) + " s" : juce::String ("?"); };

    juce::String t;
    t << "Host: " << juce::PluginHostType().getHostDescription() << "\n"
      << "Sample rate: " << sr << "   block: " << d.lastBlockSize.load() << "   channels: " << d.numInputChannels.load() << "\n"
      << "Offline render: " << (d.isNonRealtime.load() ? "yes" : "no") << "\n"
      << "PlayHead: " << (d.hasPlayHead.load() ? "yes" : "no")
      << "   timeInSamples: " << (d.hasTimeInSamples.load() ? "yes" : "no")
      << "   playing: " << (d.isPlaying.load() ? "yes" : "no") << "\n"
      << "Host time: " << d.hostTimeInSamples.load() << " (" << secs (d.hostTimeInSamples.load()) << ")\n"
      << "Sample counter: " << d.sampleCounter.load() << " (" << secs (d.sampleCounter.load()) << ")\n"
      << "Timeline used: " << d.timelinePosition.load() << " (" << secs (d.timelinePosition.load()) << ")\n"
      << "prepare: " << d.prepareCount.load() << "   reset: " << d.resetCount.load()
      << "   timeline jumps: " << d.timelineJumps.load() << "\n"
      << "Latency: " << processor.getLatencySamples() << " samples\n";

    const auto shown = processor.getDisplayPlayPosition();
    t << "Playhead shown at: " << (shown ? juce::String (*shown, 3) + " s" : juce::String ("hidden"))
      << "   processing at: " << secs (d.timelinePosition.load()) << "\n";

    const auto loop = processor.getLoopRange();
    t << "Loop: " << (loop.valid ? juce::String (loop.startSeconds, 3) + " - " + juce::String (loop.endSeconds, 3) + " s"
                                 : juce::String ("none"))
      << (loop.valid ? (loop.repeatEnabled ? "  (repeat on)" : "  (repeat off)") : "") << "\n";

    if (session != nullptr)
    {
        const auto& c = session->capture;
        t << "Session: " << session->id.substring (0, 8) << "   captured: "
          << (c.hasData() ? juce::String ((double) c.getStartSample() / c.getSampleRate(), 2) + " - "
                                + juce::String ((double) c.getEndSample() / c.getSampleRate(), 2) + " s"
                          : juce::String ("none"))
          << (c.hasOverflowed() ? "  (dropped audio!)" : "") << "\n";
    }
    return t;
}
} // namespace sdf
