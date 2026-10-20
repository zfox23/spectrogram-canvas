#include "analysis/SpectrogramStore.h"

#include <array>
#include <cmath>

namespace sdf
{
SpectrogramStore::SpectrogramStore (int fftOrder, double sr)
    : order (fftOrder),
      size (1 << fftOrder),
      hop (size / 2),
      numBins (size / 2 + 1),
      sampleRate (sr),
      maxFrames (CaptureBuffer::kMaxSamples / hop + 1),
      fft (fftOrder),
      chunkTable ((size_t) (maxFrames / kFramesPerChunk + 1))
{
    window.resize ((size_t) size);
    float windowSum = 0.0f;
    for (int i = 0; i < size; ++i)
    {
        window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) size);
        windowSum += window[(size_t) i];
    }

    // A full-scale sine then reads as 0 dB.
    magnitudeScale = 2.0f / windowSum;
    fftBuffer.assign ((size_t) size * 2, 0.0f);
    frameSamples.assign ((size_t) size, 0.0f);

    for (auto& c : chunkTable)
        c.store (nullptr, std::memory_order_relaxed);
}

std::atomic<uint8_t>* SpectrogramStore::getChunk (juce::int64 chunkIndex) const noexcept
{
    if (chunkIndex < 0 || chunkIndex >= (juce::int64) chunkTable.size())
        return nullptr;
    return chunkTable[(size_t) chunkIndex].load (std::memory_order_acquire);
}

uint8_t SpectrogramStore::getValue (juce::int64 frame, int bin) const noexcept
{
    if (frame < 0)
        return 0;
    if (auto* chunk = getChunk (frame / kFramesPerChunk))
        return chunk[(size_t) ((frame % kFramesPerChunk) * numBins + bin)].load (std::memory_order_relaxed);
    return 0;
}

void SpectrogramStore::analyse (const CaptureBuffer& capture, juce::int64 start, juce::int64 end)
{
    const juce::int64 firstFrame = juce::jmax ((juce::int64) 0, (start - size / 2) / hop);
    const juce::int64 lastFrame = juce::jmin (maxFrames - 1, (end + size / 2) / hop + 1);
    const float dbRange = kCeilDb - kFloorDb;

    for (juce::int64 f = firstFrame; f <= lastFrame; ++f)
    {
        const juce::int64 chunkIndex = f / kFramesPerChunk;
        auto* chunk = getChunk (chunkIndex);
        if (chunk == nullptr)
        {
            auto fresh = std::make_unique<std::atomic<uint8_t>[]> ((size_t) (kFramesPerChunk * numBins));
            for (int i = 0; i < kFramesPerChunk * numBins; ++i)
                fresh[(size_t) i].store (0, std::memory_order_relaxed);
            chunk = fresh.get();
            ownedChunks.push_back (std::move (fresh));
            chunkTable[(size_t) chunkIndex].store (chunk, std::memory_order_release);
        }

        capture.read (f * hop - size / 2, frameSamples.data(), size);
        for (int i = 0; i < size; ++i)
            fftBuffer[(size_t) i] = frameSamples[(size_t) i] * window[(size_t) i];
        std::fill (fftBuffer.begin() + size, fftBuffer.end(), 0.0f);

        fft.performFrequencyOnlyForwardTransform (fftBuffer.data(), true);

        auto* row = chunk + (f % kFramesPerChunk) * numBins;
        for (int k = 0; k < numBins; ++k)
        {
            const float db = juce::Decibels::gainToDecibels (fftBuffer[(size_t) k] * magnitudeScale, kFloorDb);
            const float v = juce::jlimit (0.0f, 255.0f, (db - kFloorDb) / dbRange * 255.0f);
            row[k].store ((uint8_t) v, std::memory_order_relaxed);
        }
    }

    version.fetch_add (1, std::memory_order_acq_rel);
}

//==============================================================================
juce::Colour SpectrogramStore::colourFor (uint8_t value)
{
    const auto& p = getColourTable()[value];
    return juce::Colour (p.getRed(), p.getGreen(), p.getBlue());
}

const std::array<juce::PixelARGB, 256>& SpectrogramStore::getColourTable()
{
    // Magma-like ramp: black -> indigo -> magenta -> orange -> pale yellow.
    static const std::array<juce::PixelARGB, 256> lut = [] {
        const juce::Colour stops[] = { juce::Colour (0xff000004), juce::Colour (0xff1c1044), juce::Colour (0xff4f127b),
                                       juce::Colour (0xff812581), juce::Colour (0xffb5367a), juce::Colour (0xffe55064),
                                       juce::Colour (0xfffb8761), juce::Colour (0xfffec287), juce::Colour (0xfffcfdbf) };
        constexpr int numStops = (int) std::size (stops);
        std::array<juce::PixelARGB, 256> table {};
        for (int i = 0; i < 256; ++i)
        {
            const float pos = (float) i / 255.0f * (float) (numStops - 1);
            const int s = juce::jmin ((int) pos, numStops - 2);
            table[(size_t) i] = stops[s].interpolatedWith (stops[s + 1], pos - (float) s).getPixelARGB();
        }
        return table;
    }();

    return lut;
}

void SpectrogramStore::render (juce::Image& image, const View& view) const
{
    const int w = image.getWidth(), h = image.getHeight();
    if (w <= 0 || h <= 0)
        return;

    // Bin range covered by each pixel row (top row = highest frequency).
    // Rows that span several bins take the loudest; rows narrower than a bin interpolate
    // between the two nearest bins, so low frequencies don't look like stair steps.
    std::vector<int> binLo ((size_t) h), binHi ((size_t) h), binStep ((size_t) h);
    std::vector<float> binCentre ((size_t) h);
    const double binPerHz = size / sampleRate;
    for (int y = 0; y < h; ++y)
    {
        auto normAt = [&] (double py) { return view.normHigh - py / h * (view.normHigh - view.normLow); };
        const double hzTop = view.scale.normToHz (normAt (y));
        const double hzBottom = view.scale.normToHz (normAt (y + 1));
        binLo[(size_t) y] = juce::jlimit (0, numBins - 1, (int) std::ceil (hzBottom * binPerHz));
        binHi[(size_t) y] = juce::jlimit (0, numBins - 1, (int) std::floor (hzTop * binPerHz));
        binStep[(size_t) y] = juce::jmax (1, (binHi[(size_t) y] - binLo[(size_t) y] + 1) / kMaxBinsPerPixel);
        binCentre[(size_t) y] = (float) juce::jlimit (0.0, (double) (numBins - 1), view.scale.normToHz (normAt (y + 0.5)) * binPerHz);
    }

    const double framesPerSecond = sampleRate / hop;
    const juce::Image::BitmapData bitmap (image, juce::Image::BitmapData::writeOnly);
    const auto& lut = getColourTable();

    // Each pixel shows the loudest of a few sampled frames x bins it covers (max-pooling),
    // so narrow features stay visible when zoomed out.
    std::array<const std::atomic<uint8_t>*, kMaxFramesPerPixel> rows {};

    for (int x = 0; x < w; ++x)
    {
        const double t0 = view.startSeconds + (view.endSeconds - view.startSeconds) * x / w;
        const double t1 = view.startSeconds + (view.endSeconds - view.startSeconds) * (x + 1) / w;
        const juce::int64 f0 = (juce::int64) std::llround (t0 * framesPerSecond);
        const juce::int64 f1 = juce::jmax (f0, (juce::int64) std::llround (t1 * framesPerSecond) - 1);
        const juce::int64 frameStep = juce::jmax ((juce::int64) 1, (f1 - f0 + 1) / kMaxFramesPerPixel);

        int numRows = 0;
        for (juce::int64 f = f0; f <= f1 && numRows < kMaxFramesPerPixel; f += frameStep)
            if (f >= 0)
                if (const auto* chunk = getChunk (f / kFramesPerChunk))
                    rows[(size_t) numRows++] = chunk + (f % kFramesPerChunk) * numBins;

        for (int y = 0; y < h; ++y)
        {
            uint8_t v = 0;
            const int b0 = binLo[(size_t) y], b1 = binHi[(size_t) y], step = binStep[(size_t) y];
            if (b1 > b0)
            {
                for (int r = 0; r < numRows; ++r)
                    for (int b = b0; b <= b1; b += step)
                        v = juce::jmax (v, rows[(size_t) r][b].load (std::memory_order_relaxed));
            }
            else
            {
                const float c = binCentre[(size_t) y];
                const int k0 = juce::jmin ((int) c, numBins - 2);
                uint8_t a = 0, b = 0;
                for (int r = 0; r < numRows; ++r)
                {
                    a = juce::jmax (a, rows[(size_t) r][k0].load (std::memory_order_relaxed));
                    b = juce::jmax (b, rows[(size_t) r][k0 + 1].load (std::memory_order_relaxed));
                }
                v = (uint8_t) juce::jlimit (0, 255, (int) ((float) a + (c - (float) k0) * (float) (b - a) + 0.5f));
            }

            auto* pixel = bitmap.getPixelPointer (x, y);
            const auto& c = lut[v];
            if (bitmap.pixelFormat == juce::Image::RGB)
                reinterpret_cast<juce::PixelRGB*> (pixel)->set (c);
            else
                reinterpret_cast<juce::PixelARGB*> (pixel)->set (c);
        }
    }
}
} // namespace sdf
