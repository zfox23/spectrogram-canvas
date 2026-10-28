#include <juce_core/juce_core.h>

#include "analysis/SpectrogramStore.h"
#include "capture/CaptureBuffer.h"

using namespace sc;

class CaptureTests : public juce::UnitTest
{
public:
    CaptureTests() : juce::UnitTest ("CaptureBuffer", "Model") {}

    void runTest() override
    {
        beginTest ("Writes across chunk boundaries read back exactly; gaps read as silence");
        {
            CaptureBuffer cap;
            std::vector<float> data (100000);
            for (size_t i = 0; i < data.size(); ++i)
                data[i] = (float) i * 1.0e-5f;

            const juce::int64 start = CaptureBuffer::kChunkSize - 123;
            for (size_t pos = 0; pos < data.size(); pos += 4096)
                cap.write (start + (juce::int64) pos, data.data() + pos, (int) juce::jmin<size_t> (4096, data.size() - pos), true);

            expectEquals (cap.getStartSample(), start);
            expectEquals (cap.getEndSample(), start + (juce::int64) data.size());

            std::vector<float> back (data.size() + 20);
            cap.read (start - 10, back.data(), (int) back.size());
            int errors = 0;
            for (size_t i = 0; i < back.size(); ++i)
            {
                const float expected = (i < 10 || i >= data.size() + 10) ? 0.0f : data[i - 10];
                errors += back[i] != expected ? 1 : 0;
            }
            expectEquals (errors, 0);
        }

        beginTest ("Realtime writes use the pool and do not drop while it lasts");
        {
            CaptureBuffer cap;
            std::vector<float> block (512, 0.25f);
            bool ok = true;
            for (int i = 0; i < 2000; ++i) // ~1M samples = 16 chunks, within the pool
                ok = cap.write ((juce::int64) i * 512, block.data(), 512, false) && ok;
            expect (ok);
            expect (! cap.hasOverflowed());
        }

        beginTest ("Dirty range accumulates and resets");
        {
            CaptureBuffer cap;
            std::vector<float> block (100, 0.1f);
            cap.write (1000, block.data(), 100, true);
            cap.write (5000, block.data(), 100, true);
            juce::int64 s = 0, e = 0;
            expect (cap.takeDirtyRange (s, e));
            expectEquals (s, (juce::int64) 1000);
            expectEquals (e, (juce::int64) 5100);
            expect (! cap.takeDirtyRange (s, e));
        }

        beginTest ("WAV cache round-trips with its timeline offset");
        {
            CaptureBuffer cap;
            cap.setSampleRate (48000.0);
            std::vector<float> data (30000);
            for (size_t i = 0; i < data.size(); ++i)
                data[i] = 0.5f * (float) std::sin ((double) i * 0.01);
            cap.write (777, data.data(), (int) data.size(), true);

            const auto file = juce::File::createTempFile ("wav");
            expect (cap.saveToWav (file));

            CaptureBuffer loaded;
            expect (loaded.loadFromWav (file, 777));
            file.deleteFile();

            expectEquals (loaded.getSampleRate(), 48000.0);
            expectEquals (loaded.getStartSample(), (juce::int64) 777);
            expectEquals (loaded.getEndSample(), (juce::int64) (777 + data.size()));

            std::vector<float> back (data.size());
            loaded.read (777, back.data(), (int) back.size());
            float maxErr = 0.0f;
            for (size_t i = 0; i < data.size(); ++i)
                maxErr = juce::jmax (maxErr, std::abs (back[i] - data[i]));
            expectLessThan (maxErr, 1.0e-4f); // 16-bit quantisation
        }

        beginTest ("Spectrogram shows a sine in the right bin, at full scale");
        {
            CaptureBuffer cap;
            const double sr = 44100.0;
            cap.setSampleRate (sr);
            std::vector<float> data (44100);
            const double freq = 1000.0;
            for (size_t i = 0; i < data.size(); ++i)
                data[i] = (float) std::sin (juce::MathConstants<double>::twoPi * freq * (double) i / sr);
            cap.write (0, data.data(), (int) data.size(), true);

            SpectrogramStore spec (12, sr);
            spec.analyse (cap, 0, (juce::int64) data.size());

            const juce::int64 frame = 10;
            int peakBin = 0;
            for (int b = 0; b < spec.getNumBins(); ++b)
                if (spec.getValue (frame, b) > spec.getValue (frame, peakBin))
                    peakBin = b;

            const int expectedBin = (int) std::round (freq * spec.getFftSize() / sr);
            expectEquals (peakBin, expectedBin);
            expectGreaterThan ((int) spec.getValue (frame, peakBin), 245); // ~0 dB
            expectLessThan ((int) spec.getValue (frame, expectedBin + 200), 40);
        }
    }
};

static CaptureTests captureTests;
