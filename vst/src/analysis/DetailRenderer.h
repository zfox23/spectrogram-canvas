#pragma once

#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>
#include <juce_events/juce_events.h>
#include <juce_graphics/juce_graphics.h>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

#include "analysis/SpectrogramStore.h"

namespace sdf
{
/**
    Renders a full-detail spectrogram of the visible window straight from the captured audio:
    one FFT centred on every pixel column (so time is as smooth as the screen allows) and
    zero-padded FFTs with interpolation between bins (so frequency is smooth too).

    This is used when zoomed in past the resolution of the precomputed SpectrogramStore.
    Work is split across background threads; only the most recent request is kept.
*/
class DetailRenderer
{
public:
    /** Reads mono captured audio for a timeline range (must be thread-safe). */
    using AudioReader = std::function<void (juce::int64 start, float* dest, int numSamples)>;

    struct Request
    {
        AudioReader readAudio;
        SpectrogramStore::View view;
        int width = 0, height = 0;
        int fftOrder = 12;
        double sampleRate = 44100.0;
        juce::uint32 dataVersion = 0; // changes whenever the captured audio changes
    };

    /** Called on the message thread with the finished image and the request it answers. */
    using Callback = std::function<void (juce::Image, const Request&)>;

    DetailRenderer();
    ~DetailRenderer();

    /** Starts rendering, abandoning any render still in progress. */
    void request (Request r, Callback onDone);

    /** Abandons any render in progress. */
    void cancel();

    /**
        Computes 8-bit spectrogram values for columns [x0, x1) into out (column-major,
        out[x * height + y], top row = highest frequency). Stops early (returning false) if
        shouldStop() becomes true.
    */
    static bool renderColumns (const Request& r, std::vector<uint8_t>& out, int x0, int x1,
                               const std::function<bool()>& shouldStop);

    /**
        Order of the zero-padded FFT used for a given analysis FFT order: 4x padding (at least
        8192 points, at most 32768), so peaks are drawn within about a quarter of a bin of
        their true frequency. Padding interpolates; it doesn't add real resolution.
    */
    static int paddedOrderFor (int fftOrder) { return juce::jlimit (13, 15, fftOrder + 2); }

private:
    struct Job;

    juce::ThreadPool pool;
    std::atomic<juce::uint32> generation { 0 };
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DetailRenderer)
};
} // namespace sdf
