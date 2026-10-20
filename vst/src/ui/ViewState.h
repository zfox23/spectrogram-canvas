#pragma once

#include <juce_core/juce_core.h>

#include <functional>

#include "model/FreqScale.h"
#include "model/MaskGrid.h"

namespace sdf
{
/** The visible window onto the spectrogram, shared by the main view and the overview. */
struct ViewState
{
    double startSeconds = 0.0;
    double endSeconds = 10.0;
    double normLow = 0.0, normHigh = 1.0; // visible part of the frequency axis
    double totalSeconds = 10.0;           // length of the whole document
    FreqScale scale;

    std::function<void()> onChange;

    static constexpr double kMinSpanSeconds = 0.05;
    static constexpr double kMinNormSpan = 0.02;

    double getSpan() const { return endSeconds - startSeconds; }

    void setTimeRange (double start, double end)
    {
        double span = juce::jlimit (kMinSpanSeconds, juce::jmax (kMinSpanSeconds, totalSeconds), end - start);
        start = juce::jlimit (0.0, juce::jmax (0.0, totalSeconds - span), start);
        if (start != startSeconds || start + span != endSeconds)
        {
            startSeconds = start;
            endSeconds = start + span;
            changed();
        }
    }

    void zoomTime (double factor, double anchorSeconds)
    {
        const double newSpan = getSpan() * factor;
        const double frac = (anchorSeconds - startSeconds) / getSpan();
        setTimeRange (anchorSeconds - frac * newSpan, anchorSeconds - frac * newSpan + newSpan);
    }

    void setFreqRange (double low, double high)
    {
        double span = juce::jlimit (kMinNormSpan, 1.0, high - low);
        low = juce::jlimit (0.0, 1.0 - span, low);
        if (low != normLow || low + span != normHigh)
        {
            normLow = low;
            normHigh = low + span;
            changed();
        }
    }

    void zoomFreq (double factor, double anchorNorm)
    {
        const double span = normHigh - normLow;
        const double newSpan = span * factor;
        const double frac = (anchorNorm - normLow) / span;
        setFreqRange (anchorNorm - frac * newSpan, anchorNorm - frac * newSpan + newSpan);
    }

    /** Grows the document length (e.g. as audio is captured), keeping the view where it is. */
    void setTotalSeconds (double total)
    {
        total = juce::jmax (1.0, total);
        if (total == totalSeconds)
            return;

        const bool wasShowingAll = startSeconds <= 0.0 && endSeconds >= totalSeconds - 1.0e-9;
        totalSeconds = total;
        if (wasShowingAll)
        {
            startSeconds = 0.0;
            endSeconds = total;
        }
        else if (endSeconds > total)
        {
            setTimeRange (startSeconds, total);
            return;
        }
        changed();
    }

    void setMaxHz (double maxHz)
    {
        maxHz = juce::jlimit (1000.0, MaskGrid::kMaxHz, maxHz);
        if (maxHz != scale.maxHz)
        {
            scale.maxHz = maxHz;
            changed();
        }
    }

    void setScaleType (FreqScaleType type)
    {
        if (type != scale.type)
        {
            scale.type = type;
            changed();
        }
    }

    void changed()
    {
        if (onChange)
            onChange();
    }
};
} // namespace sdf
