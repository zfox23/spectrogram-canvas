#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"
#include "ui/OverviewView.h"
#include "ui/SpectrogramView.h"
#include "ui/Theme.h"
#include "ui/ViewState.h"

namespace sdf
{
class SpectrogramDrawFilterEditor : public juce::AudioProcessorEditor,
                                    private juce::Timer
{
public:
    explicit SpectrogramDrawFilterEditor (SpectrogramDrawFilterProcessor&);
    ~SpectrogramDrawFilterEditor() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    void timerCallback() override;
    void syncSession();
    void updateDocumentLength();
    void updateBrushLabel();
    juce::String buildDiagnosticsText() const;
    void undo();
    void redo();

    using Attachment = juce::AudioProcessorValueTreeState;

    SpectrogramDrawFilterProcessor& processor;
    Theme::LookAndFeel lookAndFeel;
    std::shared_ptr<Session> session;

    ViewState viewState;
    SpectrogramView spectrogramView { viewState };
    OverviewView overview { viewState };

    juce::TextButton captureButton { "Capture" }, clearAudioButton { "Clear Audio" };
    juce::TextButton undoButton { "Undo" }, redoButton { "Redo" }, clearPaintButton { "Clear Paint" };
    juce::TextButton invertButton { "Invert" }, infoButton { "i" };
    juce::Slider bgGainSlider, hardnessSlider;
    juce::ComboBox fftCombo, scaleCombo;
    juce::Label bgGainLabel { {}, "Background Gain" }, fftLabel { {}, "FFT Size" }, hardnessLabel { {}, "Hardness" },
        scaleLabel { {}, "Freq Scale" }, brushLabel;

    std::unique_ptr<Attachment::SliderAttachment> bgGainAttachment, hardnessAttachment;
    std::unique_ptr<Attachment::ComboBoxAttachment> fftAttachment, scaleAttachment;
    std::unique_ptr<Attachment::ButtonAttachment> invertAttachment;

    int topBarHeight = 58;
    bool showDiagnostics = false;
    juce::String diagnosticsText;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrogramDrawFilterEditor)
};
} // namespace sdf
