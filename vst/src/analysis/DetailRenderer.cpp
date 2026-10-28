#include "analysis/DetailRenderer.h"

#include <cmath>

namespace sc
{
namespace
{
    int numWorkerThreads()
    {
        return juce::jlimit (1, 8, juce::SystemStats::getNumCpus() - 1);
    }
} // namespace

DetailRenderer::DetailRenderer()
    : pool (juce::ThreadPoolOptions{}.withThreadName ("Spectrogram detail").withNumberOfThreads (numWorkerThreads()))
{
}

DetailRenderer::~DetailRenderer()
{
    *alive = false;
    cancel();
    pool.removeAllJobs (true, 5000);
}

void DetailRenderer::cancel()
{
    ++generation;
}

//==============================================================================
bool DetailRenderer::renderColumns (const Request& r, std::vector<uint8_t>& out, int x0, int x1,
                                    const std::function<bool()>& shouldStop)
{
    const int n = 1 << r.fftOrder;
    const int paddedOrder = paddedOrderFor (r.fftOrder);
    const int p = 1 << paddedOrder;
    const int numBins = p / 2 + 1;
    const int h = r.height;
    const auto& v = r.view;

    juce::dsp::FFT fft (paddedOrder);
    std::vector<float> window ((size_t) n), samples ((size_t) n), buffer ((size_t) p * 2);
    float windowSum = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) n);
        windowSum += window[(size_t) i];
    }
    const float magnitudeScale = 2.0f / windowSum;
    const float dbRange = SpectrogramStore::kCeilDb - SpectrogramStore::kFloorDb;

    // For each pixel row: the padded-bin range it covers, and its centre as a fractional bin.
    std::vector<int> binLo ((size_t) h), binHi ((size_t) h);
    std::vector<float> binCentre ((size_t) h);
    const double binPerHz = p / r.sampleRate;
    auto normAt = [&] (double py) { return v.normHigh - py / h * (v.normHigh - v.normLow); };
    for (int y = 0; y < h; ++y)
    {
        const double hzTop = v.scale.normToHz (normAt (y)), hzBottom = v.scale.normToHz (normAt (y + 1));
        binLo[(size_t) y] = juce::jlimit (0, numBins - 1, (int) std::ceil (hzBottom * binPerHz));
        binHi[(size_t) y] = juce::jlimit (0, numBins - 1, (int) std::floor (hzTop * binPerHz));
        binCentre[(size_t) y] = (float) juce::jlimit (0.0, (double) (numBins - 1), v.scale.normToHz (normAt (y + 0.5)) * binPerHz);
    }

    std::vector<float> level ((size_t) numBins);

    for (int x = x0; x < x1; ++x)
    {
        if (shouldStop())
            return false;

        // One analysis frame centred on the middle of this pixel column.
        const double t = v.startSeconds + (v.endSeconds - v.startSeconds) * (x + 0.5) / r.width;
        const auto centre = (juce::int64) std::llround (t * r.sampleRate);
        r.readAudio (centre - n / 2, samples.data(), n);

        for (int i = 0; i < n; ++i)
            buffer[(size_t) i] = samples[(size_t) i] * window[(size_t) i];
        std::fill (buffer.begin() + n, buffer.end(), 0.0f);
        fft.performFrequencyOnlyForwardTransform (buffer.data(), true);

        for (int k = 0; k < numBins; ++k)
        {
            const float db = juce::Decibels::gainToDecibels (buffer[(size_t) k] * magnitudeScale, SpectrogramStore::kFloorDb);
            level[(size_t) k] = juce::jlimit (0.0f, 255.0f, (db - SpectrogramStore::kFloorDb) / dbRange * 255.0f);
        }

        auto* column = out.data() + (size_t) x * (size_t) h;
        for (int y = 0; y < h; ++y)
        {
            float value;
            if (binHi[(size_t) y] > binLo[(size_t) y])
            {
                // The pixel spans several bins: show the loudest.
                value = 0.0f;
                for (int k = binLo[(size_t) y]; k <= binHi[(size_t) y]; ++k)
                    value = juce::jmax (value, level[(size_t) k]);
            }
            else
            {
                // The pixel is narrower than a bin: interpolate between neighbouring bins.
                const float c = binCentre[(size_t) y];
                const int k0 = juce::jmin ((int) c, numBins - 2);
                value = level[(size_t) k0] + (c - (float) k0) * (level[(size_t) k0 + 1] - level[(size_t) k0]);
            }
            column[y] = (uint8_t) juce::jlimit (0, 255, (int) (value + 0.5f));
        }
    }

    return true;
}

//==============================================================================
/** Shared state of one render, split into column stripes across the pool. */
struct DetailRenderer::Job
{
    Request request;
    Callback onDone;
    std::vector<uint8_t> values;
    std::atomic<int> stripesLeft { 0 };
    std::atomic<bool> failed { false };
    juce::uint32 generation = 0;
};

void DetailRenderer::request (Request r, Callback onDone)
{
    if (r.width <= 0 || r.height <= 0)
        return;

    const auto gen = ++generation;
    auto job = std::make_shared<Job>();
    job->request = std::move (r);
    job->onDone = std::move (onDone);
    job->values.assign ((size_t) job->request.width * (size_t) job->request.height, 0);
    job->generation = gen;

    const int numStripes = juce::jmin (numWorkerThreads() * 2, job->request.width);
    job->stripesLeft = numStripes;

    auto aliveFlag = alive;
    auto* gen_ = &generation;

    for (int s = 0; s < numStripes; ++s)
    {
        const int x0 = job->request.width * s / numStripes;
        const int x1 = job->request.width * (s + 1) / numStripes;

        pool.addJob ([job, x0, x1, gen, gen_, aliveFlag] {
            auto shouldStop = [&] { return ! aliveFlag->load() || gen_->load() != gen; };

            if (! renderColumns (job->request, job->values, x0, x1, shouldStop))
                job->failed = true;

            if (--job->stripesLeft > 0 || job->failed.load() || shouldStop())
                return;

            // Last stripe done: build the image and hand it over on the message thread.
            const int w = job->request.width, h = job->request.height;
            auto image = SpectrogramStore::makeImage (juce::Image::RGB, w, h);
            {
                const juce::Image::BitmapData bitmap (image, juce::Image::BitmapData::writeOnly);
                const auto& lut = SpectrogramStore::getColourTable();
                for (int x = 0; x < w; ++x)
                    for (int y = 0; y < h; ++y)
                        reinterpret_cast<juce::PixelRGB*> (bitmap.getPixelPointer (x, y))->set (lut[job->values[(size_t) x * (size_t) h + (size_t) y]]);
            }

            juce::MessageManager::callAsync ([job, image, aliveFlag] {
                if (aliveFlag->load() && job->onDone)
                    job->onDone (image, job->request);
            });
        });
    }
}
} // namespace sc
