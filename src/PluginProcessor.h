#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>

#include "dsp/SpectralMaskProcessor.h"
#include "model/MaskGrid.h"

namespace sdf
{
namespace ParamIDs
{
    inline constexpr const char* bgGain = "bgGain";
    inline constexpr const char* fftSize = "fftSize";
    inline constexpr const char* hardness = "hardness";
    inline constexpr const char* invert = "invert";
    inline constexpr const char* freqScale = "freqScale";
} // namespace ParamIDs

/** Host/transport facts gathered on the audio thread, for the diagnostics overlay. */
struct HostDiagnostics
{
    std::atomic<bool> hasPlayHead { false };
    std::atomic<bool> hasTimeInSamples { false };
    std::atomic<bool> isPlaying { false };
    std::atomic<bool> isNonRealtime { false };
    std::atomic<juce::int64> hostTimeInSamples { 0 };
    std::atomic<juce::int64> sampleCounter { 0 };
    std::atomic<juce::int64> timelinePosition { 0 };
    std::atomic<int> lastBlockSize { 0 };
    std::atomic<int> numInputChannels { 0 };
    std::atomic<int> prepareCount { 0 };
    std::atomic<int> resetCount { 0 };
    std::atomic<int> timelineJumps { 0 };
    std::atomic<double> sampleRate { 0.0 };
};

class SpectrogramDrawFilterProcessor : public juce::AudioProcessor,
                                       private juce::AsyncUpdater
{
public:
    SpectrogramDrawFilterProcessor();
    ~SpectrogramDrawFilterProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void reset() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    juce::AudioProcessorValueTreeState& getParameters() { return parameters; }
    MaskGrid& getMask() { return mask; }
    const HostDiagnostics& getDiagnostics() const { return diagnostics; }

    /** Host-timeline position (in samples) of the audio currently being processed. */
    juce::int64 getTimelinePosition() const { return diagnostics.timelinePosition.load(); }

    /** UI-only state that is saved with the plugin. */
    juce::ValueTree& getUiState() { return uiState; }

    static int fftOrderForChoice (int choiceIndex) { return SpectralMaskProcessor::kMinOrder + choiceIndex; }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    juce::int64 resolveTimeline (int numSamples);
    void handleAsyncUpdate() override;

    juce::AudioProcessorValueTreeState parameters;
    std::atomic<float>* bgGainParam = nullptr;
    std::atomic<float>* fftSizeParam = nullptr;
    std::atomic<float>* invertParam = nullptr;

    MaskGrid mask;
    SpectralMaskProcessor spectral { mask };
    HostDiagnostics diagnostics;
    juce::ValueTree uiState { "UI" };

    juce::int64 sampleCounter = 0;
    juce::int64 expectedNextTimeline = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrogramDrawFilterProcessor)
};
} // namespace sdf
