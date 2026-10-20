#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace sdf::Theme
{
inline const juce::Colour background { 0xff121419 };
inline const juce::Colour panel { 0xff1b1e25 };
inline const juce::Colour panelEdge { 0xff2a2e38 };
inline const juce::Colour text { 0xffe6e8ee };
inline const juce::Colour textDim { 0xff9aa0ad };
inline const juce::Colour grid { 0xff5a6070 };
inline const juce::Colour accent { 0xff4cc9f0 };
inline const juce::Colour paint { 0xff4cc9f0 };
inline const juce::Colour erase { 0xffff6b6b };
inline const juce::Colour record { 0xffe5383b };
inline const juce::Colour playhead { 0xffffffff };
inline const juce::Colour loop { 0xffffc857 };

inline constexpr int rulerWidth = 46;
inline constexpr int rulerHeight = 20;

/**
    Draws the host's loop range over a plot: a marker bar along the top, edge lines, and a faint
    tint between them. Drawn dimmer when the host's repeat is off.
*/
inline void drawLoopRange (juce::Graphics& g, juce::Rectangle<int> plot, float x0, float x1, bool repeatEnabled, float barHeight)
{
    if (x1 < (float) plot.getX() || x0 > (float) plot.getRight())
        return;

    juce::Graphics::ScopedSaveState ss (g);
    g.reduceClipRegion (plot);

    const float alpha = repeatEnabled ? 1.0f : 0.5f;
    const float top = (float) plot.getY(), bottom = (float) plot.getBottom();

    g.setColour (loop.withAlpha (0.07f * alpha));
    g.fillRect (juce::Rectangle<float> (x0, top, x1 - x0, bottom - top));

    g.setColour (loop.withAlpha (0.9f * alpha));
    g.fillRect (juce::Rectangle<float> (x0, top, x1 - x0, barHeight));

    g.setColour (loop.withAlpha (0.75f * alpha));
    g.drawVerticalLine (juce::roundToInt (x0), top, bottom);
    g.drawVerticalLine (juce::roundToInt (x1) - 1, top, bottom);
}

class LookAndFeel : public juce::LookAndFeel_V4
{
public:
    LookAndFeel()
    {
        setColourScheme ({ panel, background, panel, panelEdge, text, accent, juce::Colours::black, accent, text });
        setColour (juce::TextButton::buttonColourId, juce::Colour (0xff262a33));
        setColour (juce::TextButton::buttonOnColourId, accent.darker (0.3f));
        setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff262a33));
        setColour (juce::ComboBox::outlineColourId, panelEdge);
        setColour (juce::PopupMenu::backgroundColourId, panel);
        setColour (juce::Slider::trackColourId, accent.withAlpha (0.8f));
        setColour (juce::Slider::backgroundColourId, juce::Colour (0xff2c303a));
        setColour (juce::Slider::thumbColourId, text);
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0xff262a33));
        setColour (juce::Label::textColourId, text);
    }
};
} // namespace sdf::Theme
