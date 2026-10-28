#include <juce_core/juce_core.h>

#include "analysis/DetailRenderer.h"

using namespace sc;

namespace
{
constexpr double kSampleRate = 44100.0;

DetailRenderer::Request makeRequest (std::shared_ptr<std::vector<float>> audio, double t0, double t1, FreqScaleType scale, int order)
{
    DetailRenderer::Request r;
    r.readAudio = [audio] (juce::int64 start, float* dest, int n) {
        for (int i = 0; i < n; ++i)
        {
            const auto pos = start + i;
            dest[i] = (pos >= 0 && pos < (juce::int64) audio->size()) ? (*audio)[(size_t) pos] : 0.0f;
        }
    };
    r.view.startSeconds = t0;
    r.view.endSeconds = t1;
    r.view.scale.type = scale;
    r.view.scale.maxHz = kSampleRate / 2;
    r.width = 400;
    r.height = 300;
    r.fftOrder = order;
    r.sampleRate = kSampleRate;
    return r;
}

int rowForHz (const DetailRenderer::Request& r, double hz)
{
    const double norm = r.view.scale.hzToNorm (hz);
    return (int) std::floor ((r.view.normHigh - norm) / (r.view.normHigh - r.view.normLow) * r.height);
}
} // namespace

class DetailRendererTests : public juce::UnitTest
{
public:
    DetailRendererTests() : juce::UnitTest ("DetailRenderer", "Model") {}

    void runTest() override
    {
        beginTest ("A click shows up in the right pixel column when zoomed in");
        {
            auto audio = std::make_shared<std::vector<float>> ((size_t) kSampleRate * 2, 0.0f);
            (*audio)[(size_t) kSampleRate] = 1.0f; // click at exactly 1.0 s

            // 0.95-1.05 s across 400 px: 4 samples-ish... 11 samples per pixel; FFT 512.
            auto r = makeRequest (audio, 0.95, 1.05, FreqScaleType::Log, 9);
            std::vector<uint8_t> out ((size_t) r.width * (size_t) r.height);
            expect (DetailRenderer::renderColumns (r, out, 0, r.width, [] { return false; }));

            // The window has a flat top a few pixels wide, so take the middle of the brightest plateau.
            const int row = rowForHz (r, 5000.0);
            auto at = [&] (int x) { return (int) out[(size_t) x * (size_t) r.height + (size_t) row]; };
            int best = 0;
            for (int x = 0; x < r.width; ++x)
                best = juce::jmax (best, at (x));
            int first = -1, last = -1;
            for (int x = 0; x < r.width; ++x)
                if (at (x) >= best)
                {
                    if (first < 0)
                        first = x;
                    last = x;
                }
            const double centre = (first + last) / 2.0 + 0.5;

            const double expectedX = r.width / 2.0; // 1.0 s is the middle of the window
            logMessage ("  click centred at column " + juce::String (centre, 1) + " (expected " + juce::String (expectedX, 1) + ")");
            expectWithinAbsoluteError (centre, expectedX, 1.5);
        }

        beginTest ("A tone shows up in the right pixel row, with smooth rows around it");
        {
            auto audio = std::make_shared<std::vector<float>> ((size_t) kSampleRate * 2);
            for (size_t i = 0; i < audio->size(); ++i)
                (*audio)[i] = 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 110.0 * (double) i / kSampleRate);

            // Zoom into 80-150 Hz on a log scale: 300 rows for a few FFT bins.
            auto r = makeRequest (audio, 0.9, 1.1, FreqScaleType::Log, 12);
            r.view.normLow = r.view.scale.hzToNorm (80.0);
            r.view.normHigh = r.view.scale.hzToNorm (150.0);
            std::vector<uint8_t> out ((size_t) r.width * (size_t) r.height);
            expect (DetailRenderer::renderColumns (r, out, 0, r.width, [] { return false; }));

            const auto* column = out.data() + (size_t) (r.width / 2) * (size_t) r.height;
            int peak = 0, distinct = 0;
            for (int y = 0; y < r.height; ++y)
            {
                if (column[y] > column[peak])
                    peak = y;
                if (y > 0 && column[y] != column[y - 1])
                    ++distinct;
            }

            // 8-bit levels make the top of the peak a short plateau; use its centre.
            int first = -1, last = -1;
            for (int y = 0; y < r.height; ++y)
                if (column[y] >= column[peak])
                {
                    if (first < 0)
                        first = y;
                    last = y;
                }
            const double centreRow = (first + last) / 2.0 + 0.5;
            const double peakHz = r.view.scale.normToHz (r.view.normHigh - centreRow / r.height * (r.view.normHigh - r.view.normLow));
            const double paddedBinHz = kSampleRate / (1 << DetailRenderer::paddedOrderFor (r.fftOrder));
            logMessage ("  110 Hz tone peaks at " + juce::String (peakHz, 2) + " Hz (padded bin width "
                        + juce::String (paddedBinHz, 2) + " Hz), value changes between rows: " + juce::String (distinct));
            expectWithinAbsoluteError (peakHz, 110.0, paddedBinHz / 2.0);
            expectGreaterThan (distinct, r.height / 2); // smooth, not a handful of flat steps
        }

        beginTest ("Rendering stops early when asked");
        {
            auto audio = std::make_shared<std::vector<float>> ((size_t) kSampleRate, 0.1f);
            auto r = makeRequest (audio, 0.0, 1.0, FreqScaleType::Linear, 12);
            std::vector<uint8_t> out ((size_t) r.width * (size_t) r.height);
            expect (! DetailRenderer::renderColumns (r, out, 0, r.width, [] { return true; }));
        }
    }
};

static DetailRendererTests detailRendererTests;
