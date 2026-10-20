#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <array>
#include <memory>
#include <vector>

#include "model/MaskGrid.h"

namespace sdf
{
/**
    Streaming STFT filter: each frame's spectrum is multiplied by gains read from the
    painted MaskGrid at that frame's timeline position.

    Uses a sqrt-Hann analysis/synthesis window pair with 75% overlap, which reconstructs the
    input perfectly (delayed by exactly fftSize samples) when every gain is 1.

    All FFT sizes are allocated in prepare(), so switching sizes on the audio thread never
    allocates.
*/
class SpectralMaskProcessor
{
public:
    static constexpr int kMinOrder = 9;  // 512
    static constexpr int kMaxOrder = 14; // 16384
    static constexpr int kNumSizes = kMaxOrder - kMinOrder + 1;
    static constexpr int kDefaultOrder = 12; // 4096

    struct Settings
    {
        float backgroundGain = 0.0f; // linear
        bool invert = false;
    };

    explicit SpectralMaskProcessor (const MaskGrid& mask);

    void prepare (double sampleRate, int numChannels);
    void reset();

    /** Selects the FFT size (2^order). Realtime-safe; resets the STFT state if it changes. */
    void setFftOrder (int order);
    int getFftOrder() const noexcept { return currentOrder; }
    int getFftSize() const noexcept { return 1 << currentOrder; }
    int getLatencySamples() const noexcept { return getFftSize(); }

    /**
        Processes a block in place. timelineStartSample is the host-timeline position of the
        first input sample in the block; the mask is looked up in that same timeline.
    */
    void process (juce::AudioBuffer<float>& buffer, juce::int64 timelineStartSample, const Settings& settings);

private:
    struct Engine
    {
        int order = 0, size = 0, hop = 0;
        std::unique_ptr<juce::dsp::FFT> fft;
        std::vector<float> window;    // sqrt-Hann (periodic)
        std::vector<float> binRows;   // MaskGrid row position for each bin (0..size/2)
        std::vector<float> gains;     // per-bin gain for the current frame
        std::vector<float> fftBuffer; // 2 * size
        std::vector<std::vector<float>> input, output; // per-channel circular buffers of length size
        int pos = 0, hopCounter = 0;

        void allocate (int fftOrder, int numChannels, double sampleRate);
        void clear();
    };

    void processFrame (Engine& e, int channel);
    void computeGains (Engine& e, juce::int64 frameCentreTimeline, const Settings& settings);

    const MaskGrid& mask;
    double sampleRate = 44100.0;
    int numChannels = 0;
    int currentOrder = kDefaultOrder;
    std::array<Engine, kNumSizes> engines;
};
} // namespace sdf
