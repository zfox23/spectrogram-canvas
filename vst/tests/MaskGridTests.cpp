#include <juce_core/juce_core.h>

#include "model/MaskGrid.h"

using namespace sc;

namespace
{
/** A simple linear view: 100 px per second, 0.1 px per Hz, y grows downwards from 2400 px at 0 Hz. */
MaskGrid::ScreenMapping linearMapping()
{
    MaskGrid::ScreenMapping m;
    m.secondsToX = [] (double s) { return s * 100.0; };
    m.xToSeconds = [] (double x) { return x / 100.0; };
    m.hzToY = [] (double hz) { return 2400.0 - hz * 0.1; };
    m.yToHz = [] (double y) { return (2400.0 - y) * 10.0; };
    return m;
}
} // namespace

class MaskGridTests : public juce::UnitTest
{
public:
    MaskGridTests() : juce::UnitTest ("MaskGrid", "Model") {}

    void runTest() override
    {
        beginTest ("Row/Hz mapping round-trips and spans the range");
        {
            expectWithinAbsoluteError (MaskGrid::hzToRow (MaskGrid::kMinHz), 0.0, 1.0e-9);
            expectWithinAbsoluteError (MaskGrid::hzToRow (MaskGrid::kMaxHz), (double) MaskGrid::kRows - 1, 1.0e-9);
            expectWithinAbsoluteError (MaskGrid::rowToHz (MaskGrid::hzToRow (1234.5)), 1234.5, 1.0e-6);
        }

        beginTest ("Brush profile respects hardness");
        {
            expectEquals (MaskGrid::brushProfile (0.0f, 0.0f), 1.0f);
            expectEquals (MaskGrid::brushProfile (0.99f, 1.0f), 1.0f);
            expectEquals (MaskGrid::brushProfile (1.0f, 1.0f), 0.0f);
            expectWithinAbsoluteError (MaskGrid::brushProfile (0.5f, 0.0f), 0.5f, 1.0e-6f);
            expect (MaskGrid::brushProfile (0.75f, 0.5f) > 0.0f && MaskGrid::brushProfile (0.75f, 0.5f) < 1.0f);
        }

        beginTest ("Stamp paints a round region and erase removes it");
        {
            MaskGrid grid;
            const auto m = linearMapping();
            grid.beginStroke();
            grid.stamp (m, 100.0f, 2300.0f, 20.0f, 1.0f, false); // centre: 1 s, 1000 Hz
            grid.endStroke();

            expectGreaterThan (grid.sample (1.0, MaskGrid::hzToRow (1000.0)), 0.99f);
            expectLessThan (grid.sample (1.5, MaskGrid::hzToRow (1000.0)), 0.01f);
            expectLessThan (grid.sample (1.0, MaskGrid::hzToRow (5000.0)), 0.01f);

            grid.beginStroke();
            grid.stamp (m, 100.0f, 2300.0f, 30.0f, 1.0f, true);
            grid.endStroke();
            expectLessThan (grid.sample (1.0, MaskGrid::hzToRow (1000.0)), 0.01f);
        }

        beginTest ("Undo and redo restore stroke state");
        {
            MaskGrid grid;
            const auto m = linearMapping();
            const double row = MaskGrid::hzToRow (1000.0);

            grid.beginStroke();
            grid.stamp (m, 100.0f, 2300.0f, 20.0f, 1.0f, false);
            grid.endStroke();
            expect (grid.canUndo());

            grid.undo();
            expectLessThan (grid.sample (1.0, row), 0.01f);
            expect (grid.canRedo());

            grid.redo();
            expectGreaterThan (grid.sample (1.0, row), 0.99f);

            grid.clear();
            expectLessThan (grid.sample (1.0, row), 0.01f);
            grid.undo();
            expectGreaterThan (grid.sample (1.0, row), 0.99f);
        }

        beginTest ("Serialisation round-trips exactly");
        {
            MaskGrid a;
            const auto m = linearMapping();
            a.beginStroke();
            a.stamp (m, 100.0f, 2300.0f, 25.0f, 0.3f, false);
            a.stamp (m, 5000.0f, 1500.0f, 40.0f, 0.8f, false);
            a.endStroke();

            juce::MemoryOutputStream out;
            a.writeTo (out);

            MaskGrid b;
            juce::MemoryInputStream in (out.getData(), out.getDataSize(), false);
            expect (b.readFrom (in));

            int mismatches = 0;
            for (int c = 0; c < 12000; ++c)
                for (int r = 0; r < MaskGrid::kRows; r += 3)
                    mismatches += a.getCell (c, r) != b.getCell (c, r) ? 1 : 0;
            expectEquals (mismatches, 0);
            logMessage ("  serialised size: " + juce::String ((int) out.getDataSize()) + " bytes");
        }
    }
};

static MaskGridTests maskGridTests;
