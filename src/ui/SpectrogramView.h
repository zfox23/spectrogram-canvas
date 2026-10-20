#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

#include "session/Session.h"
#include "ui/ViewState.h"

namespace sdf
{
/** Renders the mask overlay for a view into an ARGB image (shared by both views). */
void renderMaskOverlay (juce::Image& image, const MaskGrid& mask, const ViewState& view, bool invert, int x0, int x1);

/** The main, zoomable spectrogram that the user paints on. */
class SpectrogramView : public juce::Component
{
public:
    explicit SpectrogramView (ViewState& view);

    void setSession (std::shared_ptr<Session> s);

    /** Called ~30 times a second by the editor. */
    void refresh (juce::int64 playheadSample, double playheadSampleRate, bool showPlayhead);

    void setBrushRadius (float r);
    float getBrushRadius() const { return brushRadius; }
    void setHardness (float h) { hardness = h; }
    void setInvert (bool shouldInvert);
    void setOverlayText (const juce::String& text);

    std::function<void (float)> onBrushRadiusChanged;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    static constexpr float kMinBrush = 1.5f, kMaxBrush = 400.0f;

private:
    juce::Rectangle<int> getPlotArea() const;
    MaskGrid::ScreenMapping makeMapping() const;
    double xToSeconds (float x) const;
    float secondsToX (double s) const;
    double yToNorm (float y) const;
    float hzToY (double hz) const;

    void stampAt (juce::Point<float> p);
    void markOverlayDirty (float x0, float x1);
    void drawRulers (juce::Graphics&, juce::Rectangle<int> plot);
    void repaintBrush();

    ViewState& view;
    std::shared_ptr<Session> session;

    juce::Image specImage, overlayImage;
    const SpectrogramStore* renderedStore = nullptr;
    uint32_t renderedSpecVersion = 0, renderedMaskVersion = 0;
    juce::uint32 lastSpecRenderMs = 0;
    bool specDirty = true, overlayDirty = true;
    int overlayDirtyX0 = 0, overlayDirtyX1 = -1;

    float brushRadius = 20.0f, hardness = 0.5f;
    bool invert = false;
    juce::Point<float> mousePos;
    bool mouseInside = false;

    enum class Drag { none, paint, erase, pan };
    Drag drag = Drag::none;
    juce::Point<float> lastStamp;
    double panStartSeconds = 0.0, panStartNormLow = 0.0;

    float playheadX = -1.0f;
    juce::String overlayText;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrogramView)
};
} // namespace sdf
