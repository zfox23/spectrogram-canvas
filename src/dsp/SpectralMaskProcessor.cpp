#include "dsp/SpectralMaskProcessor.h"

#include <cmath>

namespace sdf
{
void SpectralMaskProcessor::Engine::allocate (int fftOrder, int channels, double sr)
{
    order = fftOrder;
    size = 1 << fftOrder;
    hop = size / 4;
    fft = std::make_unique<juce::dsp::FFT> (fftOrder);

    window.resize ((size_t) size);
    for (int i = 0; i < size; ++i)
        window[(size_t) i] = std::sqrt (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) size));

    const int numBins = size / 2 + 1;
    binRows.resize ((size_t) numBins);
    for (int k = 0; k < numBins; ++k)
        binRows[(size_t) k] = (float) MaskGrid::hzToRow ((double) k * sr / size);

    gains.assign ((size_t) numBins, 1.0f);
    fftBuffer.assign ((size_t) size * 2, 0.0f);
    input.assign ((size_t) channels, std::vector<float> ((size_t) size, 0.0f));
    output.assign ((size_t) channels, std::vector<float> ((size_t) size, 0.0f));
    clear();
}

void SpectralMaskProcessor::Engine::clear()
{
    for (auto& ch : input)
        std::fill (ch.begin(), ch.end(), 0.0f);
    for (auto& ch : output)
        std::fill (ch.begin(), ch.end(), 0.0f);
    pos = 0;
    hopCounter = 0;
}

//==============================================================================
SpectralMaskProcessor::SpectralMaskProcessor (const MaskGrid* m) : mask (m) {}

void SpectralMaskProcessor::prepare (double sr, int channels)
{
    sampleRate = sr;
    numChannels = channels;
    for (int i = 0; i < kNumSizes; ++i)
        engines[(size_t) i].allocate (kMinOrder + i, channels, sr);
}

void SpectralMaskProcessor::reset()
{
    for (auto& e : engines)
        e.clear();
}

void SpectralMaskProcessor::setFftOrder (int order)
{
    order = juce::jlimit (kMinOrder, kMaxOrder, order);
    if (order == currentOrder)
        return;

    currentOrder = order;
    engines[(size_t) (order - kMinOrder)].clear();
}

//==============================================================================
void SpectralMaskProcessor::computeGains (Engine& e, juce::int64 frameCentreTimeline, const Settings& s)
{
    const int numBins = e.size / 2 + 1;

    if (s.bypassMask)
    {
        std::fill (e.gains.begin(), e.gains.end(), 1.0f);
        return;
    }

    if (mask != nullptr)
        mask->sampleColumn ((double) frameCentreTimeline / sampleRate, e.binRows.data(), e.gains.data(), numBins);
    else
        std::fill (e.gains.begin(), e.gains.end(), 0.0f);

    const float bg = s.backgroundGain;
    for (int k = 0; k < numBins; ++k)
    {
        const float m = s.invert ? 1.0f - e.gains[(size_t) k] : e.gains[(size_t) k];
        e.gains[(size_t) k] = bg + (1.0f - bg) * m;
    }
}

void SpectralMaskProcessor::processFrame (Engine& e, int ch)
{
    const int n = e.size;
    auto& in = e.input[(size_t) ch];
    auto& out = e.output[(size_t) ch];
    float* buf = e.fftBuffer.data();

    // Unroll the circular input (oldest sample first) and apply the analysis window.
    for (int i = 0; i < n; ++i)
        buf[i] = in[(size_t) ((e.pos + i) % n)] * e.window[(size_t) i];
    std::fill (buf + n, buf + 2 * n, 0.0f);

    e.fft->performRealOnlyForwardTransform (buf, true);

    // Bins 0..n/2 are interleaved re/im pairs; scale them, then rebuild the conjugate half.
    for (int k = 0; k <= n / 2; ++k)
    {
        buf[2 * k] *= e.gains[(size_t) k];
        buf[2 * k + 1] *= e.gains[(size_t) k];
    }
    for (int k = n / 2 + 1; k < n; ++k)
    {
        buf[2 * k] = buf[2 * (n - k)];
        buf[2 * k + 1] = -buf[2 * (n - k) + 1];
    }

    e.fft->performRealOnlyInverseTransform (buf);

    // sqrt-Hann squared at 75% overlap sums to 2, so scale by 1/2.
    for (int i = 0; i < n; ++i)
        out[(size_t) ((e.pos + i) % n)] += buf[i] * e.window[(size_t) i] * 0.5f;
}

void SpectralMaskProcessor::process (juce::AudioBuffer<float>& buffer, juce::int64 timelineStart, const Settings& s)
{
    auto& e = engines[(size_t) (currentOrder - kMinOrder)];
    const int channels = juce::jmin (buffer.getNumChannels(), numChannels);
    const int numSamples = buffer.getNumSamples();

    for (int i = 0; i < numSamples; ++i)
    {
        for (int ch = 0; ch < channels; ++ch)
        {
            float* data = buffer.getWritePointer (ch);
            auto& out = e.output[(size_t) ch];
            const float x = data[i];
            data[i] = out[(size_t) e.pos];
            out[(size_t) e.pos] = 0.0f;
            e.input[(size_t) ch][(size_t) e.pos] = x;
        }

        e.pos = (e.pos + 1) % e.size;

        if (++e.hopCounter >= e.hop)
        {
            e.hopCounter = 0;
            // The frame holds input samples [t - size + 1, t]; its centre is at t - size/2 + 1.
            computeGains (e, timelineStart + i - e.size / 2 + 1, s);
            for (int ch = 0; ch < channels; ++ch)
                processFrame (e, ch);
        }
    }

    for (int ch = channels; ch < buffer.getNumChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);
}
} // namespace sdf
