#include "ui/SpectrogramView.h"

#include "ui/Theme.h"

#include <array>
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

    // Overlay colour for each mask level: dim what is filtered out, tint what was painted.
    std::array<juce::PixelARGB, 256> lut {};
    for (int i = 0; i < 256; ++i)
    {
        const float m = (float) i / 255.0f;
        const float audible = invert ? 1.0f - m : m;
        const float dim = 0.4f * (1.0f - audible);
        const float tint = 0.45f * m;
        const float alpha = dim + tint - dim * tint;
        auto c = alpha > 0.0f ? juce::Colours::black.interpolatedWith (Theme::paint, tint / juce::jmax (alpha, 1.0e-6f)).withAlpha (alpha)
                              : juce::Colours::transparentBlack;
        lut[(size_t) i] = c.getPixelARGB(); // premultiplied
    }

    const juce::Image::BitmapData bitmap (image, x0, 0, x1 - x0 + 1, h, juce::Image::BitmapData::writeOnly);

    for (int x = x0; x <= x1; ++x)
    {
        const double t = view.startSeconds + view.getSpan() * (x + 0.5) / w;
        mask.sampleColumn (t, rows.data(), values.data(), h);

        for (int y = 0; y < h; ++y)
        {
            const int level = juce::jlimit (0, 255, (int) (values[(size_t) y] * 255.0f + 0.5f));
            reinterpret_cast<juce::PixelARGB*> (bitmap.getPixelPointer (x - x0, y))->set (lut[(size_t) level]);
        }
    }
}

//==============================================================================
SpectrogramView::SpectrogramView (ViewState& v) : view (v)
{
    setMouseCursor (juce::MouseCursor::CrosshairCursor);
    setWantsKeyboardFocus (false);
    setOpaque (true);
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

void SpectrogramView::setLoopRange (const LoopRange& range)
{
    if (range != loopRange)
    {
        loopRange = range;
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

    // The visible window changed since the images were rendered (zoom, pan, scrub, scale).
    if (! sameWindow (currentWindow(), renderedView))
        repaint();

    if (specDirty || overlayDirty || overlayDirtyX1 >= overlayDirtyX0)
        repaint();
}

SpectrogramStore::View SpectrogramView::currentWindow() const
{
    return { view.startSeconds, view.endSeconds, view.scale, view.normLow, view.normHigh };
}

bool SpectrogramView::sameScale (const SpectrogramStore::View& a, const SpectrogramStore::View& b)
{
    return a.scale.type == b.scale.type && a.scale.minHz == b.scale.minHz && a.scale.maxHz == b.scale.maxHz;
}

bool SpectrogramView::sameWindow (const SpectrogramStore::View& a, const SpectrogramStore::View& b)
{
    return a.startSeconds == b.startSeconds && a.endSeconds == b.endSeconds && a.normLow == b.normLow
           && a.normHigh == b.normHigh && sameScale (a, b);
}

void SpectrogramView::updateCursor()
{
    if (drag == Drag::pan || drag == Drag::scrub)
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    else
        setMouseCursor (juce::MouseCursor::CrosshairCursor);
}

void SpectrogramView::paint (juce::Graphics& g)
{
    g.fillAll (Theme::background);
    const auto plot = getPlotArea();
    if (plot.isEmpty())
        return;

    const auto now = juce::Time::getMillisecondCounter();
    const auto spec = session != nullptr ? session->getSpectrogram() : nullptr;
    const auto sv = currentWindow();

    if (! sameWindow (sv, lastSeenView))
    {
        lastSeenView = sv;
        lastViewChangeMs = now;
    }

    bool sizeChanged = false;
    if (specImage.getWidth() != plot.getWidth() || specImage.getHeight() != plot.getHeight())
    {
        specImage = SpectrogramStore::makeImage (juce::Image::RGB, plot.getWidth(), plot.getHeight());
        overlayImage = SpectrogramStore::makeImage (juce::Image::ARGB, plot.getWidth(), plot.getHeight());
        specDirty = overlayDirty = sizeChanged = true;
        hasRendered = false;
    }

    // While the window is moving, stretch the last render to the new window (cheap and exact
    // for the parts it covers), and do a full render at most every kRenderIntervalMs and once
    // the motion settles. refresh() keeps repainting until the render matches the window.
    const bool windowMoved = ! sameWindow (sv, renderedView);
    const bool interacting = now - lastViewChangeMs < kSettleMs && now - lastFullRenderMs < kRenderIntervalMs;
    if (windowMoved && hasRendered && ! sizeChanged && sameScale (sv, renderedView) && interacting
        && drag != Drag::paint && drag != Drag::erase)
    {
        drawStretchedPreview (g, plot, sv);
    }
    else
    {
        if (windowMoved)
        {
            specDirty = overlayDirty = true;
            renderedView = sv;
        }
        renderImages (spec, plot);
        g.drawImageAt (specImage, plot.getX(), plot.getY());
        g.drawImageAt (overlayImage, plot.getX(), plot.getY());
    }

    paintDecorations (g, plot);
}

void SpectrogramView::drawStretchedPreview (juce::Graphics& g, juce::Rectangle<int> plot, const SpectrogramStore::View& sv)
{
    const auto& r = renderedView;
    const double w = plot.getWidth(), h = plot.getHeight();
    const double sx = (r.endSeconds - r.startSeconds) / (sv.endSeconds - sv.startSeconds);
    const double tx = (r.startSeconds - sv.startSeconds) / (sv.endSeconds - sv.startSeconds) * w;
    const double sy = (r.normHigh - r.normLow) / (sv.normHigh - sv.normLow);
    const double ty = (sv.normHigh - r.normHigh) / (sv.normHigh - sv.normLow) * h;
    const auto transform = juce::AffineTransform::scale ((float) sx, (float) sy)
                               .translated ((float) (tx + plot.getX()), (float) (ty + plot.getY()));

    juce::Graphics::ScopedSaveState ss (g);
    g.reduceClipRegion (plot);
    g.setColour (SpectrogramStore::colourFor (0));
    g.fillRect (plot);
    g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
    g.drawImageTransformed (specImage, transform);
    g.drawImageTransformed (overlayImage, transform);
}

void SpectrogramView::renderImages (const std::shared_ptr<SpectrogramStore>& spec, juce::Rectangle<int> plot)
{
    bool rendered = false;

    if (specDirty)
    {
        if (spec != nullptr)
        {
            spec->render (specImage, renderedView);
            renderedSpecVersion = spec->getVersion();
        }
        else
        {
            specImage.clear (specImage.getBounds(), SpectrogramStore::colourFor (0));
        }
        renderedStore = spec.get();
        lastSpecRenderMs = juce::Time::getMillisecondCounter();
        specDirty = false;
        rendered = true;
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
            rendered = true;
        }
        overlayDirty = false;
        overlayDirtyX0 = 0;
        overlayDirtyX1 = -1;
    }

    if (rendered)
        lastFullRenderMs = juce::Time::getMillisecondCounter();
    hasRendered = true;
}

void SpectrogramView::paintDecorations (juce::Graphics& g, juce::Rectangle<int> plot)
{
    {
        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (plot);

        // Shade time before 0 and after the end of the document.
        g.setColour (Theme::background.withAlpha (0.6f));
        const float endX = secondsToX (view.totalSeconds);
        if (endX < (float) plot.getRight())
            g.fillRect (juce::Rectangle<float> (endX, (float) plot.getY(), (float) plot.getRight() - endX, (float) plot.getHeight()));

        if (loopRange.valid)
            Theme::drawLoopRange (g, plot, secondsToX (loopRange.startSeconds), secondsToX (loopRange.endSeconds),
                                  loopRange.repeatEnabled, 5.0f);

        if (playheadX >= 0.0f)
        {
            g.setColour (Theme::playhead);
            g.drawVerticalLine ((int) playheadX, (float) plot.getY(), (float) plot.getBottom());
        }

        if ((mouseInside && drag == Drag::none) || drag == Drag::paint || drag == Drag::erase)
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
        // Try progressively finer steps (1-2-5 per decade, then 1..9, then halves) and keep the
        // finest set whose labels don't collide, so zooming in reveals more labels.
        static const std::vector<std::vector<double>> mantissaSets = {
            { 1, 2, 5 }, { 1, 1.5, 2, 3, 5, 7 }, { 1, 1.5, 2, 2.5, 3, 4, 5, 6, 7, 8, 9 },
            { 1, 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.7, 1.8, 1.9, 2, 2.2, 2.4, 2.6, 2.8, 3, 3.5, 4, 4.5, 5, 5.5, 6, 6.5, 7, 7.5, 8, 8.5, 9, 9.5 }
        };

        auto ticksFor = [&] (const std::vector<double>& mantissas) {
            std::vector<double> ticks;
            for (double decade = 10.0; decade <= 10000.0; decade *= 10.0)
                for (double m : mantissas)
                {
                    const double hz = m * decade;
                    if (hz >= view.scale.minHz && hz <= view.scale.maxHz)
                    {
                        const float y = hzToY (hz);
                        if (y >= (float) plot.getY() && y <= (float) plot.getBottom())
                            ticks.push_back (hz);
                    }
                }
            return ticks;
        };

        auto fits = [&] (const std::vector<double>& ticks) {
            for (size_t i = 1; i < ticks.size(); ++i)
                if (hzToY (ticks[i - 1]) - hzToY (ticks[i]) < 16.0f)
                    return false;
            return true;
        };

        std::vector<double> candidates = ticksFor (mantissaSets[0]);
        for (size_t i = 1; i < mantissaSets.size(); ++i)
        {
            auto finer = ticksFor (mantissaSets[i]);
            if (! fits (finer))
                break;
            candidates = std::move (finer);
        }

        float lastY = 1.0e9f;
        for (double hz : candidates)
        {
            const float y = hzToY (hz);
            if (lastY - y < 16.0f)
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

    // Middle-drag scrubs through time; Alt + left-drag pans both time and frequency.
    const bool scrub = e.mods.isMiddleButtonDown();
    const bool pan = e.mods.isLeftButtonDown() && e.mods.isAltDown();

    if (scrub || pan)
    {
        drag = scrub ? Drag::scrub : Drag::pan;
        panStartSeconds = view.startSeconds;
        panStartNormLow = view.normLow;
        updateCursor();
        repaint();
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

    if (drag == Drag::pan || drag == Drag::scrub)
    {
        // The content follows the mouse, like grabbing the page.
        const auto plot = getPlotArea();
        const auto offset = e.getOffsetFromDragStart().toDouble();
        const double dt = -offset.x / plot.getWidth() * view.getSpan();
        view.setTimeRange (panStartSeconds + dt, panStartSeconds + dt + view.getSpan());
        if (drag == Drag::pan)
        {
            const double dn = offset.y / plot.getHeight() * (view.normHigh - view.normLow);
            view.setFreqRange (panStartNormLow + dn, panStartNormLow + dn + (view.normHigh - view.normLow));
        }
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

    drag = Drag::none;
    updateCursor();
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
