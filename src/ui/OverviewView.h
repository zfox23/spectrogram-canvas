#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

#include "session/Session.h"
#include "ui/ViewState.h"

namespace sdf
{
/**
    The whole document at a glance, with the main view's window drawn on it. Drag the window
    to scrub through time, drag its edges to zoom, or click elsewhere to jump.
*/
class OverviewView : public juce::Component
{
public:
    explicit OverviewView (ViewState& view);

    void setSession (std::shared_ptr<Session> s);
    void setInvert (bool shouldInvert);
    void refresh (juce::int64 playheadSample, double sampleRate, bool showPlayhead);
    void viewChanged() { repaint(); }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    juce::Rectangle<int> getPlotArea() const;
    double xToSeconds (float x) const;
    float secondsToX (double s) const;
    juce::Rectangle<float> getWindowRect() const;

    ViewState& view;
    std::shared_ptr<Session> session;

    juce::Image image;
    bool dirty = true;
    const SpectrogramStore* renderedStore = nullptr;
    uint32_t renderedSpecVersion = 0, renderedMaskVersion = 0;
    double renderedTotal = 0.0;
    FreqScale renderedScale;
    juce::uint32 lastRenderMs = 0;
    bool invert = false;

    enum class Drag { none, move, left, right };
    Drag drag = Drag::none;
    double dragStartSeconds = 0.0, dragStartEnd = 0.0;
    float playheadX = -1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OverviewView)
};
} // namespace sdf
