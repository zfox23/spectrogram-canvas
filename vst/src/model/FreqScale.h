#pragma once

#include <algorithm>
#include <cmath>

namespace sc
{
enum class FreqScaleType
{
    Log = 0,
    Linear,
    Mel
};

/** Maps frequencies in Hz to a normalised display position in [0, 1] (0 = bottom, 1 = top). */
struct FreqScale
{
    FreqScaleType type = FreqScaleType::Log;
    double minHz = 20.0;
    double maxHz = 22050.0;

    static double hzToMel (double hz) { return 2595.0 * std::log10 (1.0 + hz / 700.0); }
    static double melToHz (double mel) { return 700.0 * (std::pow (10.0, mel / 2595.0) - 1.0); }

    double hzToNorm (double hz) const
    {
        switch (type)
        {
            case FreqScaleType::Linear:
                return (hz - minHz) / (maxHz - minHz);
            case FreqScaleType::Mel:
                return (hzToMel (hz) - hzToMel (minHz)) / (hzToMel (maxHz) - hzToMel (minHz));
            case FreqScaleType::Log:
            default:
                return std::log (std::max (hz, 1.0e-3) / minHz) / std::log (maxHz / minHz);
        }
    }

    double normToHz (double n) const
    {
        switch (type)
        {
            case FreqScaleType::Linear:
                return minHz + n * (maxHz - minHz);
            case FreqScaleType::Mel:
                return melToHz (hzToMel (minHz) + n * (hzToMel (maxHz) - hzToMel (minHz)));
            case FreqScaleType::Log:
            default:
                return minHz * std::pow (maxHz / minHz, n);
        }
    }
};
} // namespace sc
