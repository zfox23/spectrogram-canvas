#include "PluginEditor.h"

namespace sdf
{
SpectrogramDrawFilterEditor::SpectrogramDrawFilterEditor (SpectrogramDrawFilterProcessor& p)
    : AudioProcessorEditor (&p), processor (p)
{
    setResizable (true, true);
    setResizeLimits (640, 400, 4000, 3000);
    setSize (1100, 700);
    startTimerHz (30);
}

SpectrogramDrawFilterEditor::~SpectrogramDrawFilterEditor() = default;

void SpectrogramDrawFilterEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff15171c));
    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (15.0f));
    g.drawMultiLineText (diagnosticsText, 20, 30, getWidth() - 40);
}

void SpectrogramDrawFilterEditor::resized() {}

void SpectrogramDrawFilterEditor::timerCallback()
{
    const auto& d = processor.getDiagnostics();
    const double sr = d.sampleRate.load();
    auto secs = [sr] (juce::int64 s) { return sr > 0 ? juce::String ((double) s / sr, 3) + " s" : juce::String ("?"); };

    juce::String t;
    t << "Host diagnostics\n"
      << "Host: " << juce::PluginHostType().getHostDescription() << "\n"
      << "Sample rate: " << sr << "   block size: " << d.lastBlockSize.load()
      << "   channels: " << d.numInputChannels.load() << "\n"
      << "Non-realtime (offline render): " << (d.isNonRealtime.load() ? "yes" : "no") << "\n"
      << "PlayHead available: " << (d.hasPlayHead.load() ? "yes" : "no")
      << "   timeInSamples available: " << (d.hasTimeInSamples.load() ? "yes" : "no")
      << "   playing: " << (d.isPlaying.load() ? "yes" : "no") << "\n"
      << "Host timeInSamples: " << d.hostTimeInSamples.load() << " (" << secs (d.hostTimeInSamples.load()) << ")\n"
      << "Sample counter: " << d.sampleCounter.load() << " (" << secs (d.sampleCounter.load()) << ")\n"
      << "Timeline used: " << d.timelinePosition.load() << " (" << secs (d.timelinePosition.load()) << ")\n"
      << "prepareToPlay calls: " << d.prepareCount.load() << "   reset calls: " << d.resetCount.load()
      << "   timeline jumps: " << d.timelineJumps.load() << "\n"
      << "Reported latency: " << processor.getLatencySamples() << " samples\n";

    if (t != diagnosticsText)
    {
        diagnosticsText = t;
        repaint();
    }
}
} // namespace sdf
