#include "ui/OverviewView.h"

#include "ui/SpectrogramView.h"
#include "ui/Theme.h"

namespace sdf
{
OverviewView::OverviewView (ViewState& v) : view (v) {}

void OverviewView::setSession (std::shared_ptr<Session> s)
{
    session = std::move (s);
    dirty = true;
    repaint();
}

void OverviewView::setInvert (bool shouldInvert)
{
    if (invert != shouldInvert)
    {
        invert = shouldInvert;
        dirty = true;
        repaint();
    }
}

juce::Rectangle<int> OverviewView::getPlotArea() const
{
    return getLocalBounds().withTrimmedLeft (Theme::rulerWidth).reduced (0, 2);
}

void OverviewView::resized() { dirty = true; }

double OverviewView::xToSeconds (float x) const
{
    const auto plot = getPlotArea();
    return (x - (float) plot.getX()) / (float) plot.getWidth() * view.totalSeconds;
}

float OverviewView::secondsToX (double s) const
{
    const auto plot = getPlotArea();
    return (float) (plot.getX() + s / view.totalSeconds * plot.getWidth());
}

juce::Rectangle<float> OverviewView::getWindowRect() const
{
    const auto plot = getPlotArea().toFloat();
    const float x0 = secondsToX (view.startSeconds), x1 = secondsToX (view.endSeconds);
    return { x0, plot.getY(), juce::jmax (2.0f, x1 - x0), plot.getHeight() };
}

void OverviewView::refresh (juce::int64 playheadSample, double sampleRate, bool showPlayhead)
{
    if (session == nullptr)
        return;

    const auto now = juce::Time::getMillisecondCounter();
    const auto spec = session->getSpectrogram();
    const bool specChanged = spec.get() != renderedStore || (spec != nullptr && spec->getVersion() != renderedSpecVersion);
    const bool maskChanged = session->mask.getVersion() != renderedMaskVersion;
    const bool viewChanged = renderedTotal != view.totalSeconds || renderedScale.type != view.scale.type
                             || renderedScale.maxHz != view.scale.maxHz;

    if (viewChanged || ((specChanged || maskChanged) && now - lastRenderMs > 250))
    {
        dirty = true;
        repaint();
    }

    const float newX = showPlayhead && sampleRate > 0 ? secondsToX ((double) playheadSample / sampleRate) : -1.0f;
    if (std::abs (newX - playheadX) > 0.5f)
    {
        playheadX = newX;
        repaint();
    }
}

void OverviewView::paint (juce::Graphics& g)
{
    g.fillAll (Theme::background);
    const auto plot = getPlotArea();
    if (plot.isEmpty())
        return;

    if (image.getWidth() != plot.getWidth() || image.getHeight() != plot.getHeight())
    {
        image = juce::Image (juce::Image::ARGB, plot.getWidth(), plot.getHeight(), true);
        dirty = true;
    }

    if (dirty && session != nullptr)
    {
        ViewState whole;
        whole.totalSeconds = view.totalSeconds;
        whole.startSeconds = 0.0;
        whole.endSeconds = view.totalSeconds;
        whole.scale = view.scale;

        const auto spec = session->getSpectrogram();
        if (spec != nullptr)
            spec->render (image, { 0.0, view.totalSeconds, view.scale, 0.0, 1.0 });
        else
            image.clear (image.getBounds(), SpectrogramStore::colourFor (0));

        juce::Image overlay (juce::Image::ARGB, image.getWidth(), image.getHeight(), true);
        renderMaskOverlay (overlay, session->mask, whole, invert, 0, overlay.getWidth() - 1);
        {
            juce::Graphics ig (image);
            ig.drawImageAt (overlay, 0, 0);
        }

        renderedStore = spec.get();
        renderedSpecVersion = spec != nullptr ? spec->getVersion() : 0;
        renderedMaskVersion = session->mask.getVersion();
        renderedTotal = view.totalSeconds;
        renderedScale = view.scale;
        lastRenderMs = juce::Time::getMillisecondCounter();
        dirty = false;
    }

    g.drawImageAt (image, plot.getX(), plot.getY());

    // Dim everything outside the main view's window.
    const auto win = getWindowRect();
    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.fillRect (juce::Rectangle<float> ((float) plot.getX(), win.getY(), win.getX() - (float) plot.getX(), win.getHeight()));
    g.fillRect (juce::Rectangle<float> (win.getRight(), win.getY(), (float) plot.getRight() - win.getRight(), win.getHeight()));
    g.setColour (Theme::text);
    g.drawRect (win, 1.5f);

    if (playheadX >= 0.0f)
    {
        g.setColour (Theme::playhead.withAlpha (0.8f));
        g.drawVerticalLine ((int) playheadX, (float) plot.getY(), (float) plot.getBottom());
    }

    g.setColour (Theme::textDim);
    g.setFont (juce::FontOptions (11.0f));
    g.drawFittedText ("Overview", 0, plot.getY(), Theme::rulerWidth - 6, plot.getHeight(), juce::Justification::centredRight, 2);
}

//==============================================================================
void OverviewView::mouseDown (const juce::MouseEvent& e)
{
    const auto win = getWindowRect();
    dragStartSeconds = view.startSeconds;
    dragStartEnd = view.endSeconds;

    if (std::abs (e.position.x - win.getX()) < 5.0f)
        drag = Drag::left;
    else if (std::abs (e.position.x - win.getRight()) < 5.0f)
        drag = Drag::right;
    else if (win.contains (e.position))
        drag = Drag::move;
    else
    {
        // Jump: centre the window on the click, then keep dragging it.
        const double span = view.getSpan();
        const double centre = xToSeconds (e.position.x);
        view.setTimeRange (centre - span / 2, centre + span / 2);
        dragStartSeconds = view.startSeconds;
        dragStartEnd = view.endSeconds;
        drag = Drag::move;
    }
}

void OverviewView::mouseDrag (const juce::MouseEvent& e)
{
    const double dt = xToSeconds (e.position.x) - xToSeconds (e.mouseDownPosition.x);
    switch (drag)
    {
        case Drag::move:
            view.setTimeRange (dragStartSeconds + dt, dragStartEnd + dt);
            break;
        case Drag::left:
            view.setTimeRange (juce::jmin (dragStartSeconds + dt, dragStartEnd - ViewState::kMinSpanSeconds), dragStartEnd);
            break;
        case Drag::right:
            view.setTimeRange (dragStartSeconds, juce::jmax (dragStartEnd + dt, dragStartSeconds + ViewState::kMinSpanSeconds));
            break;
        case Drag::none:
            break;
    }
}

void OverviewView::mouseUp (const juce::MouseEvent&) { drag = Drag::none; }

void OverviewView::mouseMove (const juce::MouseEvent& e)
{
    const auto win = getWindowRect();
    const bool onEdge = std::abs (e.position.x - win.getX()) < 5.0f || std::abs (e.position.x - win.getRight()) < 5.0f;
    setMouseCursor (onEdge ? juce::MouseCursor::LeftRightResizeCursor
                           : win.contains (e.position) ? juce::MouseCursor::DraggingHandCursor
                                                       : juce::MouseCursor::PointingHandCursor);
}

void OverviewView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const float delta = std::abs (wheel.deltaY) > std::abs (wheel.deltaX) ? wheel.deltaY : wheel.deltaX;
    if (delta != 0.0f)
        view.zoomTime (std::pow (2.0, -delta * 2.0), xToSeconds (e.position.x));
}
} // namespace sdf
