#pragma once

#include <juce_dsp/juce_dsp.h>
#include <juce_graphics/juce_graphics.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

#include "capture/CaptureBuffer.h"
#include "model/FreqScale.h"

namespace sdf
{
/**
    Magnitude spectrogram (8-bit dB) of a CaptureBuffer, at one FFT size.

    Frame f is centred on timeline sample f * hop. Frames are stored in lazily-allocated
    chunks; the analysis thread writes them while the UI reads.
*/
class SpectrogramStore
{
public:
    static constexpr float kFloorDb = -110.0f;
    static constexpr float kCeilDb = 0.0f;

    SpectrogramStore (int fftOrder, double sampleRate);

    int getFftOrder() const noexcept { return order; }
    int getFftSize() const noexcept { return size; }
    int getHop() const noexcept { return hop; }
    int getNumBins() const noexcept { return numBins; }
    double getSampleRate() const noexcept { return sampleRate; }

    /** Analysis thread: recomputes every frame whose window overlaps [start, end). */
    void analyse (const CaptureBuffer& capture, juce::int64 start, juce::int64 end);

    /** 0..255, where 0 is kFloorDb or below. */
    uint8_t getValue (juce::int64 frame, int bin) const noexcept;

    uint32_t getVersion() const noexcept { return version.load (std::memory_order_acquire); }

    /** View description shared by the renderers. */
    struct View
    {
        double startSeconds = 0.0, endSeconds = 1.0;
        FreqScale scale;
        double normLow = 0.0, normHigh = 1.0; // visible part of the frequency axis, 0..1
    };

    /** Renders the spectrogram into the whole of an RGB image. */
    void render (juce::Image& image, const View& view) const;

    static juce::Colour colourFor (uint8_t value);
    static const std::array<juce::PixelARGB, 256>& getColourTable();

    /** Images that are written pixel-by-pixel must be software images (not Direct2D). */
    static juce::Image makeImage (juce::Image::PixelFormat format, int w, int h)
    {
        return juce::Image (format, juce::jmax (1, w), juce::jmax (1, h), true, juce::SoftwareImageType());
    }

private:
    static constexpr int kFramesPerChunk = 256;
    static constexpr int kMaxFramesPerPixel = 4;
    static constexpr int kMaxBinsPerPixel = 6;
    using Chunk = std::unique_ptr<std::atomic<uint8_t>[]>;

    std::atomic<uint8_t>* getChunk (juce::int64 chunkIndex) const noexcept;

    const int order, size, hop, numBins;
    const double sampleRate;
    const juce::int64 maxFrames;

    juce::dsp::FFT fft;
    std::vector<float> window, fftBuffer, frameSamples;
    float magnitudeScale = 1.0f;

    std::vector<std::atomic<std::atomic<uint8_t>*>> chunkTable;
    std::vector<Chunk> ownedChunks;
    std::atomic<uint32_t> version { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrogramStore)
};
} // namespace sdf
