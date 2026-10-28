#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <memory>

#include "dsp/SpectralMaskProcessor.h"
#include "host/HostTransport.h"
#include "session/Session.h"

namespace sc
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
    std::atomic<juce::uint32> lastProcessMs { 0 };
};

class SpectrogramCanvasProcessor : public juce::AudioProcessor,
                                       private juce::AsyncUpdater
{
public:
    SpectrogramCanvasProcessor();
    ~SpectrogramCanvasProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void reset() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    juce::VST3ClientExtensions* getVST3ClientExtensions() override { return &hostTransport; }
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
    const HostDiagnostics& getDiagnostics() const { return diagnostics; }

    /** Message thread: the session currently in use (it can change when state is restored). */
    std::shared_ptr<Session> getSession() const;

    /** Host-timeline position (in samples) of the audio most recently processed. */
    juce::int64 getTimelinePosition() const { return diagnostics.timelinePosition.load(); }
    double getCurrentSampleRate() const { return diagnostics.sampleRate.load(); }

    /** Message thread: the host's loop range (REAPER: exact and live; others: from the play head). */
    LoopRange getLoopRange() const { return hostTransport.get(); }

    /**
        Message thread: where the playhead should be drawn, in timeline seconds, or empty to hide it.
        In REAPER this is the audible position (REAPER processes FX ahead of playback); elsewhere it
        is the position of the audio most recently processed.
    */
    std::optional<double> getDisplayPlayPosition() const;

    /** UI-only state that is saved with the plugin (brush size, view, ...). */
    juce::ValueTree& getUiState() { return uiState; }

    static int fftOrderForChoice (int choiceIndex) { return SpectralMaskProcessor::kMinOrder + choiceIndex; }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    juce::int64 resolveTimeline (int numSamples, bool& isPlaying, bool& hasTransport);
    void setSession (std::shared_ptr<Session> newSession);
    void handleAsyncUpdate() override;

    juce::AudioProcessorValueTreeState parameters;
    std::atomic<float>* bgGainParam = nullptr;
    std::atomic<float>* fftSizeParam = nullptr;
    std::atomic<float>* invertParam = nullptr;

    mutable juce::SpinLock sessionLock; // guards 'session' against swaps during processBlock
    std::shared_ptr<Session> session;

    SpectralMaskProcessor spectral;
    HostDiagnostics diagnostics;
    HostTransport hostTransport;
    juce::ValueTree uiState { "UI" };

    juce::AudioBuffer<float> monoScratch;
    juce::int64 sampleCounter = 0;
    juce::int64 expectedNextTimeline = -1;
    bool wasPlaying = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrogramCanvasProcessor)
};
} // namespace sc
