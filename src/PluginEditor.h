#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"

namespace sdf
{
class SpectrogramDrawFilterEditor : public juce::AudioProcessorEditor,
                                    private juce::Timer
{
public:
    explicit SpectrogramDrawFilterEditor (SpectrogramDrawFilterProcessor&);
    ~SpectrogramDrawFilterEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    SpectrogramDrawFilterProcessor& processor;
    juce::String diagnosticsText;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrogramDrawFilterEditor)
};
} // namespace sdf
