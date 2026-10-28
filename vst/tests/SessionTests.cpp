#include <juce_core/juce_core.h>

#include "session/Session.h"

using namespace sc;

class SessionTests : public juce::UnitTest
{
public:
    SessionTests() : juce::UnitTest ("Session", "Model") {}

    void runTest() override
    {
        beginTest ("Sessions with the same ID are shared");
        {
            bool created = false;
            const auto id = Session::createId();
            auto a = Session::getOrCreate (id, created);
            expect (created);
            auto b = Session::getOrCreate (id, created);
            expect (! created);
            expect (a == b);
        }

        beginTest ("Forking copies mask and audio into an independent session");
        {
            bool created = false;
            auto source = Session::getOrCreate (Session::createId(), created);
            source->mask.fillRect (100, 200, 300, 400, 255);
            source->capture.setSampleRate (48000.0);
            std::vector<float> audio (100000, 0.3f);
            source->capture.write (5000, audio.data(), (int) audio.size(), true);

            auto copy = Session::forkFrom (*source);
            expect (copy->id != source->id);
            expectEquals ((int) copy->mask.getCell (150, 350), 255);
            expectEquals (copy->capture.getStartSample(), (juce::int64) 5000);
            expectEquals (copy->capture.getEndSample(), (juce::int64) 105000);
            expectEquals (copy->capture.getSampleRate(), 48000.0);

            float v = 0.0f;
            copy->capture.read (60000, &v, 1);
            expectEquals (v, 0.3f);

            // Painting the copy must not affect the source.
            copy->mask.fillRect (1000, 1001, 10, 11, 255);
            expectEquals ((int) source->mask.getCell (1000, 10), 0);

            copy->clearCapture(); // leaves no cache file behind
            source->clearCapture();
        }
    }
};

static SessionTests sessionTests;
