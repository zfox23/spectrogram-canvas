#include "ui/SpectrogramView.h"

#include "ui/Theme.h"

#include <cmath>

namespace sdf
{
void renderMaskOverlay (juce::Image& image, const MaskGrid& mask, const ViewState& view, bool invert, int x0, int x1)
{
    const int w = image.getWidth(), h = image.getHeight();
    x0 = juce::jmax (0, x0);
    x1 = juce::jmin (w - 1, x1);
    if (w <= 0 || h <= 0 || x1 < x0)
        return;

    std::vector<float> rows ((size_t) h), values ((size_t) h);
    for (int y = 0; y < h; ++y)
    {
        const double norm = view.normHigh - (y + 0.5) / h * (view.normHigh - view.normLow);
        rows[(size_t) y] = (float) MaskGrid::hzToRow (view.scale.normToHz (norm));
    }

    const auto paintColour = Theme::paint;
    const juce::Image::BitmapData bitmap (image, x0, 0, x1 - x0 + 1, h, juce::Image::BitmapData::writeOnly);

    for (int x = x0; x <= x1; ++x)
    {
        const double t = view.startSeconds + view.getSpan() * (x + 0.5) / w;
        mask.sampleColumn (t, rows.data(), values.data(), h);

        for (int y = 0; y < h; ++y)
        {
            const float m = values[(size_t) y];
            const float audible = invert ? 1.0f - m : m;
            // Dim what is filtered out, and tint what was painted.
            const float dim = 0.4f * (1.0f - audible);
            const float tint = 0.45f * m;
            const float alpha = dim + tint - dim * tint;
            juce::Colour c = alpha > 0.0f
                                 ? juce::Colours::black.interpolatedWith (paintColour, tint / juce::jmax (alpha, 1.0e-6f)).withAlpha (alpha)
                                 : juce::Colours::transparentBlack;
            bitmap.setPixelColour (x - x0, y, c);
        }
    }
}

//==============================================================================
SpectrogramView::SpectrogramView (ViewState& v) : view (v)
{
    setMouseCursor (juce::MouseCursor::CrosshairCursor);
    setWantsKeyboardFocus (false);
}

void SpectrogramView::setSession (std::shared_ptr<Session> s)
{
    session = std::move (s);
    specDirty = overlayDirty = true;
    renderedStore = nullptr;
    repaint();
}

void SpectrogramView::setBrushRadius (float r)
{
    brushRadius = juce::jlimit (kMinBrush, kMaxBrush, r);
    repaint();
}

void SpectrogramView::setInvert (bool shouldInvert)
{
    if (invert != shouldInvert)
    {
        invert = shouldInvert;
        overlayDirty = true;
        repaint();
    }
}

void SpectrogramView::setOverlayText (const juce::String& text)
{
    if (text != overlayText)
    {
        overlayText = text;
        repaint();
    }
}

juce::Rectangle<int> SpectrogramView::getPlotArea() const
{
    return getLocalBounds().withTrimmedLeft (Theme::rulerWidth).withTrimmedBottom (Theme::rulerHeight);
}

void SpectrogramView::resized()
{
    specDirty = overlayDirty = true;
}

//==============================================================================
double SpectrogramView::xToSeconds (float x) const
{
    const auto plot = getPlotArea();
    return view.startSeconds + (x - (float) plot.getX()) / (float) plot.getWidth() * view.getSpan();
}

float SpectrogramView::secondsToX (double s) const
{
    const auto plot = getPlotArea();
    return (float) (plot.getX() + (s - view.startSeconds) / view.getSpan() * plot.getWidth());
}

double SpectrogramView::yToNorm (float y) const
{
    const auto plot = getPlotArea();
    return view.normHigh - (y - (float) plot.getY()) / (float) plot.getHeight() * (view.normHigh - view.normLow);
}

float SpectrogramView::hzToY (double hz) const
{
    const auto plot = getPlotArea();
    return (float) (plot.getY() + (view.normHigh - view.scale.hzToNorm (hz)) / (view.normHigh - view.normLow) * plot.getHeight());
}

MaskGrid::ScreenMapping SpectrogramView::makeMapping() const
{
    MaskGrid::ScreenMapping m;
    m.secondsToX = [this] (double s) { return (double) secondsToX (s); };
    m.xToSeconds = [this] (double x) { return xToSeconds ((float) x); };
    m.hzToY = [this] (double hz) { return (double) hzToY (hz); };
    m.yToHz = [this] (double y) { return view.scale.normToHz (yToNorm ((float) y)); };
    return m;
}

//==============================================================================
void SpectrogramView::refresh (juce::int64 playheadSample, double sampleRate, bool showPlayhead)
{
    if (session == nullptr)
        return;

    // Spectrogram image: re-render when its data or the view changes (throttled while capturing).
    if (auto spec = session->getSpectrogram())
    {
        const auto now = juce::Time::getMillisecondCounter();
        if (spec.get() != renderedStore || (spec->getVersion() != renderedSpecVersion && now - lastSpecRenderMs > 150))
            specDirty = true;
    }
    else if (renderedStore != nullptr)
    {
        specDirty = true;
    }

    if (session->mask.getVersion() != renderedMaskVersion && overlayDirtyX1 < overlayDirtyX0)
        overlayDirty = true; // changed by something other than our own strokes (undo, another instance, ...)

    const float newPlayheadX = showPlayhead && sampleRate > 0 ? secondsToX ((double) playheadSample / sampleRate) : -1.0f;
    if (std::abs (newPlayheadX - playheadX) > 0.5f)
    {
        const auto plot = getPlotArea();
        repaint ((int) playheadX - 2, plot.getY(), 5, plot.getHeight());
        playheadX = newPlayheadX;
        repaint ((int) playheadX - 2, plot.getY(), 5, plot.getHeight());
    }

    if (specDirty || overlayDirty || overlayDirtyX1 >= overlayDirtyX0)
        repaint();
}

void SpectrogramView::paint (juce::Graphics& g)
{
    g.fillAll (Theme::background);
    const auto plot = getPlotArea();
    if (plot.isEmpty())
        return;

    const auto spec = session != nullptr ? session->getSpectrogram() : nullptr;
    SpectrogramStore::View sv { view.startSeconds, view.endSeconds, view.scale, view.normLow, view.normHigh };

    if (specImage.getWidth() != plot.getWidth() || specImage.getHeight() != plot.getHeight())
    {
        specImage = juce::Image (juce::Image::RGB, plot.getWidth(), plot.getHeight(), true);
        overlayImage = juce::Image (juce::Image::ARGB, plot.getWidth(), plot.getHeight(), true);
        specDirty = overlayDirty = true;
    }

    if (specDirty)
    {
        if (spec != nullptr)
        {
            spec->render (specImage, sv);
            renderedSpecVersion = spec->getVersion();
        }
        else
        {
            specImage.clear (specImage.getBounds(), SpectrogramStore::colourFor (0));
        }
        renderedStore = spec.get();
        lastSpecRenderMs = juce::Time::getMillisecondCounter();
        specDirty = false;
    }

    if (session != nullptr)
    {
        if (overlayDirty)
        {
            overlayDirtyX0 = 0;
            overlayDirtyX1 = plot.getWidth() - 1;
        }
        if (overlayDirtyX1 >= overlayDirtyX0)
        {
            overlayImage.clear ({ overlayDirtyX0, 0, overlayDirtyX1 - overlayDirtyX0 + 1, overlayImage.getHeight() });
            renderMaskOverlay (overlayImage, session->mask, view, invert, overlayDirtyX0, overlayDirtyX1);
            renderedMaskVersion = session->mask.getVersion();
        }
        overlayDirty = false;
        overlayDirtyX0 = 0;
        overlayDirtyX1 = -1;
    }

    g.drawImageAt (specImage, plot.getX(), plot.getY());
    g.drawImageAt (overlayImage, plot.getX(), plot.getY());

    {
        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (plot);

        // Shade time before 0 and after the end of the document.
        g.setColour (Theme::background.withAlpha (0.6f));
        const float endX = secondsToX (view.totalSeconds);
        if (endX < (float) plot.getRight())
            g.fillRect (juce::Rectangle<float> (endX, (float) plot.getY(), (float) plot.getRight() - endX, (float) plot.getHeight()));

        if (playheadX >= 0.0f)
        {
            g.setColour (Theme::playhead);
            g.drawVerticalLine ((int) playheadX, (float) plot.getY(), (float) plot.getBottom());
        }

        if (mouseInside || drag == Drag::paint || drag == Drag::erase)
        {
            const auto colour = drag == Drag::erase ? Theme::erase : juce::Colours::white;
            g.setColour (colour.withAlpha (0.9f));
            g.drawEllipse (juce::Rectangle<float> (brushRadius * 2, brushRadius * 2).withCentre (mousePos), 1.2f);
            if (hardness < 0.99f)
            {
                const float inner = juce::jmax (1.0f, brushRadius * hardness);
                g.setColour (colour.withAlpha (0.35f));
                g.drawEllipse (juce::Rectangle<float> (inner * 2, inner * 2).withCentre (mousePos), 1.0f);
            }
        }
    }

    drawRulers (g, plot);

    if (overlayText.isNotEmpty())
    {
        g.setColour (Theme::text.withAlpha (0.8f));
        g.setFont (juce::FontOptions (16.0f));
        g.drawFittedText (overlayText, plot.reduced (40), juce::Justification::centred, 4);
    }
}

void SpectrogramView::drawRulers (juce::Graphics& g, juce::Rectangle<int> plot)
{
    g.setFont (juce::FontOptions (11.0f));

    // Frequency ruler.
    {
        static const double candidates[] = { 20, 30, 50, 70, 100, 150, 200, 300, 500, 700, 1000, 1500, 2000, 3000, 4000,
                                             5000, 6000, 7000, 8000, 10000, 12000, 14000, 16000, 18000, 20000, 22000, 24000 };
        float lastY = 1.0e9f;
        for (double hz : candidates)
        {
            if (hz > view.scale.maxHz)
                break;
            const float y = hzToY (hz);
            if (y < (float) plot.getY() || y > (float) plot.getBottom() || lastY - y < 18.0f)
                continue;
            lastY = y;
            g.setColour (Theme::grid);
            g.drawHorizontalLine ((int) y, (float) plot.getX() - 4.0f, (float) plot.getX());
            g.setColour (Theme::textDim);
            const auto label = hz >= 1000 ? juce::String (hz / 1000.0, hz < 10000 && std::fmod (hz, 1000.0) != 0 ? 1 : 0) + "k"
                                          : juce::String ((int) hz);
            g.drawText (label, 0, (int) y - 7, Theme::rulerWidth - 6, 14, juce::Justification::centredRight);
        }
    }

    // Time ruler.
    {
        const double pxPerSec = plot.getWidth() / view.getSpan();
        static const double steps[] = { 0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600 };
        double step = steps[std::size (steps) - 1];
        for (double s : steps)
            if (s * pxPerSec >= 80.0)
            {
                step = s;
                break;
            }

        const int decimals = step < 0.01 ? 3 : step < 0.1 ? 2 : step < 1 ? 1 : 0;
        for (double t = std::ceil (view.startSeconds / step) * step; t <= view.endSeconds; t += step)
        {
            const float x = secondsToX (t);
            g.setColour (Theme::grid);
            g.drawVerticalLine ((int) x, (float) plot.getBottom(), (float) plot.getBottom() + 4.0f);
            g.setColour (Theme::textDim);
            const juce::String label = t >= 60.0 ? juce::String ((int) (t / 60)) + ":" + juce::String (std::fmod (t, 60.0), decimals).paddedLeft ('0', decimals > 0 ? 3 + decimals : 2)
                                                 : juce::String (t, decimals) + "s";
            const int labelX = juce::jmin ((int) x - 40, plot.getRight() - 80);
            g.drawText (label, labelX, plot.getBottom() + 4, 80, Theme::rulerHeight - 4,
                        labelX < (int) x - 40 ? juce::Justification::topRight : juce::Justification::centredTop);
        }
    }
}

//==============================================================================
void SpectrogramView::markOverlayDirty (float x0, float x1)
{
    const int plotX = getPlotArea().getX();
    const int a = (int) std::floor (x0) - plotX - 1, b = (int) std::ceil (x1) - plotX + 1;
    if (overlayDirtyX1 < overlayDirtyX0)
    {
        overlayDirtyX0 = a;
        overlayDirtyX1 = b;
    }
    else
    {
        overlayDirtyX0 = juce::jmin (overlayDirtyX0, a);
        overlayDirtyX1 = juce::jmax (overlayDirtyX1, b);
    }
    overlayDirtyX0 = juce::jmax (0, overlayDirtyX0);
    overlayDirtyX1 = juce::jmin (getPlotArea().getWidth() - 1, overlayDirtyX1);
}

void SpectrogramView::stampAt (juce::Point<float> p)
{
    if (session == nullptr)
        return;

    session->mask.stamp (makeMapping(), p.x, p.y, brushRadius, hardness, drag == Drag::erase);
    markOverlayDirty (p.x - brushRadius, p.x + brushRadius);
    renderedMaskVersion = session->mask.getVersion();
    repaint();
}

void SpectrogramView::repaintBrush()
{
    repaint (juce::Rectangle<float> (brushRadius * 2 + 6, brushRadius * 2 + 6).withCentre (mousePos).toNearestInt());
}

void SpectrogramView::mouseDown (const juce::MouseEvent& e)
{
    mousePos = e.position;

    if (e.mods.isMiddleButtonDown() || (e.mods.isLeftButtonDown() && e.mods.isAltDown()))
    {
        drag = Drag::pan;
        panStartSeconds = view.startSeconds;
        panStartNormLow = view.normLow;
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        return;
    }

    if (session == nullptr || ! getPlotArea().toFloat().contains (e.position))
        return;

    drag = e.mods.isRightButtonDown() ? Drag::erase : Drag::paint;
    session->mask.beginStroke();
    stampAt (e.position);
    lastStamp = e.position;
}

void SpectrogramView::mouseDrag (const juce::MouseEvent& e)
{
    repaintBrush();
    mousePos = e.position;

    if (drag == Drag::pan)
    {
        const auto plot = getPlotArea();
        const auto offset = e.getOffsetFromDragStart().toDouble();
        const double dt = -offset.x / plot.getWidth() * view.getSpan();
        const double dn = offset.y / plot.getHeight() * (view.normHigh - view.normLow);
        view.setTimeRange (panStartSeconds + dt, panStartSeconds + dt + view.getSpan());
        view.setFreqRange (panStartNormLow + dn, panStartNormLow + dn + (view.normHigh - view.normLow));
        return;
    }

    if (drag == Drag::paint || drag == Drag::erase)
    {
        // Interpolate stamps so fast strokes stay continuous.
        const float spacing = juce::jmax (1.0f, brushRadius * 0.25f);
        const float dist = lastStamp.getDistanceFrom (e.position);
        if (dist >= spacing)
        {
            const int steps = (int) (dist / spacing);
            for (int i = 1; i <= steps; ++i)
                stampAt (lastStamp + (e.position - lastStamp) * ((float) i / (float) steps));
            lastStamp = e.position;
        }
    }
    repaintBrush();
}

void SpectrogramView::mouseUp (const juce::MouseEvent&)
{
    if ((drag == Drag::paint || drag == Drag::erase) && session != nullptr)
        session->mask.endStroke();

    if (drag == Drag::pan)
        setMouseCursor (juce::MouseCursor::CrosshairCursor);

    drag = Drag::none;
    repaint();
}

void SpectrogramView::mouseMove (const juce::MouseEvent& e)
{
    repaintBrush();
    mousePos = e.position;
    mouseInside = getPlotArea().toFloat().contains (e.position);
    repaintBrush();
}

void SpectrogramView::mouseExit (const juce::MouseEvent&)
{
    mouseInside = false;
    repaint();
}

void SpectrogramView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const float delta = std::abs (wheel.deltaY) > std::abs (wheel.deltaX) ? wheel.deltaY : wheel.deltaX;
    if (delta == 0.0f)
        return;

    const auto mods = e.mods;
    if (mods.isCtrlDown() && mods.isShiftDown())
    {
        view.zoomFreq (std::pow (2.0, -delta * 2.0), yToNorm (e.position.y));
    }
    else if (mods.isCtrlDown())
    {
        view.zoomTime (std::pow (2.0, -delta * 2.0), xToSeconds (e.position.x));
    }
    else if (mods.isShiftDown())
    {
        const double dt = -delta * 0.5 * view.getSpan();
        view.setTimeRange (view.startSeconds + dt, view.endSeconds + dt);
    }
    else
    {
        repaintBrush();
        setBrushRadius (brushRadius * std::pow (2.0f, delta * 1.5f));
        repaintBrush();
        if (onBrushRadiusChanged)
            onBrushRadiusChanged (brushRadius);
    }
}
} // namespace sdf
