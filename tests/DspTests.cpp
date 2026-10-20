#include <juce_core/juce_core.h>

#include "dsp/SpectralMaskProcessor.h"

using namespace sdf;

namespace
{
constexpr double kSampleRate = 44100.0;

/** Runs a mono signal through the processor in irregular block sizes. */
std::vector<float> runProcessor (const MaskGrid& mask, const std::vector<float>& input, int order,
                                 SpectralMaskProcessor::Settings settings, int numChannels = 1)
{
    SpectralMaskProcessor proc (mask);
    proc.prepare (kSampleRate, numChannels);
    proc.setFftOrder (order);

    std::vector<float> output (input.size());
    juce::AudioBuffer<float> buffer (numChannels, 1024);
    const int blockSizes[] = { 512, 37, 1024, 300, 1 };
    size_t pos = 0;
    int b = 0;

    while (pos < input.size())
    {
        const int n = (int) std::min<size_t> ((size_t) blockSizes[b++ % 5], input.size() - pos);
        buffer.setSize (numChannels, n, false, false, true);
        for (int ch = 0; ch < numChannels; ++ch)
            buffer.copyFrom (ch, 0, input.data() + pos, n);
        proc.process (buffer, (juce::int64) pos, settings);
        std::copy (buffer.getReadPointer (0), buffer.getReadPointer (0) + n, output.data() + pos);
        pos += (size_t) n;
    }

    return output;
}

std::vector<float> noise (size_t n, int seed = 1)
{
    juce::Random r (seed);
    std::vector<float> v (n);
    for (auto& x : v)
        x = r.nextFloat() * 2.0f - 1.0f;
    return v;
}

std::vector<float> sines (size_t n, std::initializer_list<double> freqs)
{
    std::vector<float> v (n, 0.0f);
    for (double f : freqs)
        for (size_t i = 0; i < n; ++i)
            v[i] += 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * f * (double) i / kSampleRate);
    return v;
}

/** Amplitude of a sinusoid at freq in x[start, end), via Goertzel. */
double toneAmplitude (const std::vector<float>& x, size_t start, size_t end, double freq)
{
    const double w = juce::MathConstants<double>::twoPi * freq / kSampleRate;
    const double coeff = 2.0 * std::cos (w);
    double s1 = 0, s2 = 0;
    for (size_t i = start; i < end; ++i)
    {
        const double s0 = x[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return 2.0 * std::sqrt (std::max (power, 0.0)) / (double) (end - start);
}

double rms (const std::vector<float>& x, size_t start, size_t end)
{
    double sum = 0;
    for (size_t i = start; i < end; ++i)
        sum += (double) x[i] * x[i];
    return std::sqrt (sum / (double) (end - start));
}

double toDb (double ratio) { return 20.0 * std::log10 (std::max (ratio, 1.0e-12)); }

void paintAllRows (MaskGrid& mask, double fromSec, double toSec)
{
    mask.fillRect ((int) MaskGrid::secondsToCol (fromSec), (int) MaskGrid::secondsToCol (toSec), 0, MaskGrid::kRows - 1, 255);
}

void paintBand (MaskGrid& mask, double loHz, double hiHz, double seconds)
{
    mask.fillRect (0, (int) MaskGrid::secondsToCol (seconds) + 1,
                   (int) std::floor (MaskGrid::hzToRow (loHz)), (int) std::ceil (MaskGrid::hzToRow (hiHz)), 255);
}
} // namespace

class SpectralMaskProcessorTests : public juce::UnitTest
{
public:
    SpectralMaskProcessorTests() : juce::UnitTest ("SpectralMaskProcessor", "DSP") {}

    void runTest() override
    {
        const size_t len = (size_t) (kSampleRate * 2.0);

        beginTest ("Fully painted mask reconstructs the input, delayed by the FFT size");
        {
            MaskGrid mask;
            paintAllRows (mask, 0.0, 3.0);
            const auto in = noise (len);

            for (int order = SpectralMaskProcessor::kMinOrder; order <= SpectralMaskProcessor::kMaxOrder; ++order)
            {
                const auto out = runProcessor (mask, in, order, { 0.0f, false });
                const size_t latency = (size_t) 1 << order;

                double errSq = 0, sigSq = 0;
                for (size_t i = latency * 2; i < len; ++i)
                {
                    const double d = (double) out[i] - in[i - latency];
                    errSq += d * d;
                    sigSq += (double) in[i - latency] * in[i - latency];
                }
                const double errDb = 10.0 * std::log10 (std::max (errSq, 1.0e-30) / sigSq);
                logMessage ("  FFT " + juce::String (1 << order) + ": reconstruction error " + juce::String (errDb, 1) + " dB");
                expectLessThan (errDb, -100.0);
            }
        }

        beginTest ("Unpainted mask outputs silence at -144 dB background");
        {
            MaskGrid mask;
            const auto out = runProcessor (mask, noise (len), SpectralMaskProcessor::kDefaultOrder, { 0.0f, false });
            expectLessThan (rms (out, 0, len), 1.0e-7);
        }

        beginTest ("Unpainted mask passes the background gain");
        {
            MaskGrid mask;
            const auto in = noise (len);
            const auto out = runProcessor (mask, in, SpectralMaskProcessor::kDefaultOrder, { 0.5f, false });
            const size_t lat = 4096;
            double maxErr = 0;
            for (size_t i = lat * 2; i < len; ++i)
                maxErr = std::max (maxErr, std::abs ((double) out[i] - 0.5 * in[i - lat]));
            expectLessThan (maxErr, 1.0e-4);
        }

        beginTest ("Painting the 440 Hz band isolates it from a 3 kHz tone");
        {
            MaskGrid mask;
            paintBand (mask, 380.0, 500.0, 3.0);
            const auto in = sines (len, { 440.0, 3000.0 });
            const auto out = runProcessor (mask, in, SpectralMaskProcessor::kDefaultOrder, { 0.0f, false });

            const size_t s = 16384, e = len;
            const double keep = toDb (toneAmplitude (out, s, e, 440.0) / toneAmplitude (in, s, e, 440.0));
            const double cut = toDb (toneAmplitude (out, s, e, 3000.0) / toneAmplitude (in, s, e, 3000.0));
            logMessage ("  440 Hz: " + juce::String (keep, 2) + " dB, 3 kHz: " + juce::String (cut, 1) + " dB");
            expectWithinAbsoluteError (keep, 0.0, 0.5);
            expectLessThan (cut, -60.0);
        }

        beginTest ("Invert removes the painted band and keeps the rest");
        {
            MaskGrid mask;
            paintBand (mask, 380.0, 500.0, 3.0);
            const auto in = sines (len, { 440.0, 3000.0 });
            const auto out = runProcessor (mask, in, SpectralMaskProcessor::kDefaultOrder, { 0.0f, true });

            const size_t s = 16384, e = len;
            const double cut = toDb (toneAmplitude (out, s, e, 440.0) / toneAmplitude (in, s, e, 440.0));
            const double keep = toDb (toneAmplitude (out, s, e, 3000.0) / toneAmplitude (in, s, e, 3000.0));
            logMessage ("  440 Hz: " + juce::String (cut, 1) + " dB, 3 kHz: " + juce::String (keep, 2) + " dB");
            expectLessThan (cut, -60.0);
            expectWithinAbsoluteError (keep, 0.0, 0.5);
        }

        beginTest ("Mask is applied at the right point in the timeline");
        {
            MaskGrid mask;
            paintAllRows (mask, 1.0, 2.0);
            const size_t n = (size_t) (kSampleRate * 3.0);
            const auto in = noise (n);
            const auto out = runProcessor (mask, in, SpectralMaskProcessor::kDefaultOrder, { 0.0f, false });
            const size_t lat = 4096;
            auto at = [] (double sec) { return (size_t) (sec * kSampleRate); };

            // Output is delayed by the latency, so input time t appears at output t + latency.
            const double inside = rms (out, at (1.2) + lat, at (1.8) + lat) / rms (in, at (1.2), at (1.8));
            const double before = rms (out, at (0.2) + lat, at (0.8) + lat) / rms (in, at (0.2), at (0.8));
            const double after = rms (out, at (2.2) + lat, at (2.8)) / rms (in, at (2.2), at (2.8) - lat);
            logMessage ("  inside " + juce::String (toDb (inside), 2) + " dB, before " + juce::String (toDb (before), 1)
                        + " dB, after " + juce::String (toDb (after), 1) + " dB");
            expectWithinAbsoluteError (toDb (inside), 0.0, 0.1);
            expectLessThan (toDb (before), -100.0);
            expectLessThan (toDb (after), -100.0);
        }

        beginTest ("Stereo channels are processed identically");
        {
            MaskGrid mask;
            paintBand (mask, 380.0, 500.0, 3.0);
            const auto in = sines (len, { 440.0, 3000.0 });
            const auto mono = runProcessor (mask, in, 11, { 0.0f, false }, 1);
            const auto stereoL = runProcessor (mask, in, 11, { 0.0f, false }, 2);
            double maxDiff = 0;
            for (size_t i = 0; i < len; ++i)
                maxDiff = std::max (maxDiff, std::abs ((double) mono[i] - stereoL[i]));
            expectLessThan (maxDiff, 1.0e-6);
        }
    }
};

static SpectralMaskProcessorTests spectralMaskProcessorTests;
