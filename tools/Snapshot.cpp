// Renders the plugin editor to a PNG after feeding it synthetic audio, so the UI can be
// checked without a host.  Usage: SdfSnapshot <output.png> [width height]

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginEditor.h"
#include "PluginProcessor.h"

using namespace sdf;

namespace
{
/** A few seconds of test material: a rising chirp, a harmonic tone and noise bursts. */
float testSignal (juce::int64 i, double sr, juce::Random& rng)
{
    const double t = (double) i / sr;
    const double twoPi = juce::MathConstants<double>::twoPi;

    const double f0 = 100.0, f1 = 8000.0, dur = 8.0;
    const double k = std::log (f1 / f0) / dur;
    const double tc = std::fmod (t, dur); // repeat the chirp for long captures
    double x = 0.25 * std::sin (twoPi * f0 * (std::exp (k * tc) - 1.0) / k);

    for (int h = 1; h <= 6; ++h)
        x += 0.12 / h * std::sin (twoPi * 220.0 * h * t);

    if (std::fmod (t, 2.0) < 0.15)
        x += 0.2 * (rng.nextDouble() * 2.0 - 1.0);

    return (float) x;
}
} // namespace

int main (int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI gui;

    const juce::File out = juce::File::getCurrentWorkingDirectory().getChildFile (argc > 1 ? argv[1] : "snapshot.png");
    const int width = argc > 3 ? juce::String (argv[2]).getIntValue() : 1100;
    const int height = argc > 3 ? juce::String (argv[3]).getIntValue() : 700;

    // Optional 4th argument: comma-separated options, e.g. "invert,linear,mel,zoom,long".
    const auto options = juce::StringArray::fromTokens (argc > 4 ? argv[4] : "", ",", "");
    const double seconds = options.contains ("long") ? 180.0 : 8.0;

    SpectrogramDrawFilterProcessor proc;
    const double sr = 44100.0;
    const int block = 512;
    proc.setPlayConfigDetails (2, 2, sr, block);
    proc.prepareToPlay (sr, block);

    auto session = proc.getSession();
    session->setArmed (true);

    juce::AudioBuffer<float> buffer (2, block);
    juce::MidiBuffer midi;
    juce::Random rng (42);
    const juce::int64 total = (juce::int64) (sr * seconds);
    for (juce::int64 pos = 0; pos < total; pos += block)
    {
        for (int i = 0; i < block; ++i)
        {
            const float v = testSignal (pos + i, sr, rng);
            buffer.setSample (0, i, v);
            buffer.setSample (1, i, v);
        }
        proc.processBlock (buffer, midi);
    }
    session->setArmed (false);

    // "loop": process one block with a play head that reports loop points of 2-5 s
    // (in quarter notes at 120 BPM), exercising the non-REAPER loop path.
    struct LoopingPlayHead : juce::AudioPlayHead
    {
        juce::int64 time = 0;
        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo info;
            info.setTimeInSamples (time);
            info.setTimeInSeconds ((double) time / 44100.0);
            info.setBpm (120.0);
            info.setPpqPosition ((double) time / 44100.0 * 2.0);
            info.setLoopPoints (LoopPoints { 4.0, 10.0 });
            info.setIsLooping (true);
            return info;
        }
    } loopingPlayHead;

    if (options.contains ("loop"))
    {
        loopingPlayHead.time = total;
        proc.setPlayHead (&loopingPlayHead);
        buffer.clear();
        proc.processBlock (buffer, midi);
    }

    // Paint a stroke along the 3rd harmonic (660 Hz) from 1 s to 6 s.
    MaskGrid::ScreenMapping m;
    m.secondsToX = [] (double s) { return s * 100.0; };
    m.xToSeconds = [] (double x) { return x / 100.0; };
    m.hzToY = [] (double hz) { return -MaskGrid::hzToRow (hz); };
    m.yToHz = [] (double y) { return MaskGrid::rowToHz (-y); };
    session->mask.beginStroke();
    for (float x = 100.0f; x <= 600.0f; x += 2.0f)
        session->mask.stamp (m, x, (float) -MaskGrid::hzToRow (660.0), 12.0f, 0.4f, false);
    session->mask.endStroke();

    // Let the analysis thread finish.
    uint32_t lastVersion = 0;
    int stableFor = 0;
    for (int i = 0; i < 500 && stableFor < 10; ++i)
    {
        juce::Thread::sleep (20);
        const auto spec = session->getSpectrogram();
        const auto v = spec != nullptr ? spec->getVersion() : 0;
        stableFor = (spec != nullptr && v == lastVersion) ? stableFor + 1 : 0;
        lastVersion = v;
    }

    auto& params = proc.getParameters();
    if (options.contains ("invert"))
        params.getParameter (ParamIDs::invert)->setValueNotifyingHost (1.0f);
    if (options.contains ("linear"))
        params.getParameter (ParamIDs::freqScale)->setValueNotifyingHost (params.getParameter (ParamIDs::freqScale)->convertTo0to1 (1.0f));
    if (options.contains ("mel"))
        params.getParameter (ParamIDs::freqScale)->setValueNotifyingHost (1.0f);

    std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
    editor->setSize (width, height);
    editor->setVisible (true);

    // Pump the message loop so the editor's timer runs and the views render.
    juce::MessageManager::getInstance()->runDispatchLoopUntil (800);

    // "zoom": after the first render, zoom to 3-4 s and 400-1200 Hz (log), as a user would.
    if (options.contains ("zoom"))
    {
        auto& vs = dynamic_cast<SpectrogramDrawFilterEditor&> (*editor).getViewState();
        vs.setTimeRange (3.0, 4.0);
        vs.setFreqRange (vs.scale.hzToNorm (400.0), vs.scale.hzToNorm (1200.0));
        juce::MessageManager::getInstance()->runDispatchLoopUntil (400);
    }

    const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
    out.deleteFile();
    juce::FileOutputStream stream (out);
    juce::PNGImageFormat png;
    const bool ok = stream.openedOk() && png.writeImageToStream (image, stream);
    std::cout << (ok ? "Wrote " : "Failed to write ") << out.getFullPathName() << std::endl;

    editor.reset();
    proc.releaseResources();
    return ok ? 0 : 1;
}
