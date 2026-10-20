#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace sdf
{
SpectrogramDrawFilterProcessor::SpectrogramDrawFilterProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "PARAMS", createParameterLayout())
{
    bgGainParam = parameters.getRawParameterValue (ParamIDs::bgGain);
    fftSizeParam = parameters.getRawParameterValue (ParamIDs::fftSize);
    invertParam = parameters.getRawParameterValue (ParamIDs::invert);
}

SpectrogramDrawFilterProcessor::~SpectrogramDrawFilterProcessor()
{
    cancelPendingUpdate();
}

juce::AudioProcessorValueTreeState::ParameterLayout SpectrogramDrawFilterProcessor::createParameterLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamIDs::bgGain, 1 }, "Background Gain",
        NormalisableRange<float> (-144.0f, 12.0f, 0.1f, 2.5f), -144.0f,
        AudioParameterFloatAttributes().withLabel ("dB").withStringFromValueFunction ([] (float v, int) {
            return v <= -144.0f ? String ("-inf") : String (v, 1);
        })));

    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { ParamIDs::fftSize, 1 }, "FFT Size",
        StringArray { "512", "1024", "2048", "4096", "8192", "16384" },
        SpectralMaskProcessor::kDefaultOrder - SpectralMaskProcessor::kMinOrder));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamIDs::hardness, 1 }, "Brush Hardness",
        NormalisableRange<float> (0.0f, 100.0f, 1.0f), 50.0f));

    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamIDs::invert, 1 }, "Invert", false));

    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { ParamIDs::freqScale, 1 }, "Frequency Scale", StringArray { "Log", "Linear", "Mel" }, 0));

    return layout;
}

bool SpectrogramDrawFilterProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

//==============================================================================
void SpectrogramDrawFilterProcessor::prepareToPlay (double sampleRate, int)
{
    spectral.prepare (sampleRate, juce::jmax (getTotalNumInputChannels(), getTotalNumOutputChannels()));
    spectral.setFftOrder (fftOrderForChoice ((int) fftSizeParam->load()));
    setLatencySamples (spectral.getLatencySamples());

    sampleCounter = 0;
    expectedNextTimeline = -1;
    diagnostics.sampleRate = sampleRate;
    ++diagnostics.prepareCount;
}

void SpectrogramDrawFilterProcessor::reset()
{
    spectral.reset();
    sampleCounter = 0;
    expectedNextTimeline = -1;
    ++diagnostics.resetCount;
}

juce::int64 SpectrogramDrawFilterProcessor::resolveTimeline (int numSamples)
{
    bool hasPlayHead = false, hasTime = false, playing = false;
    juce::int64 hostTime = 0;

    if (auto* ph = getPlayHead())
    {
        hasPlayHead = true;
        if (const auto pos = ph->getPosition())
        {
            playing = pos->getIsPlaying();
            if (const auto t = pos->getTimeInSamples())
            {
                hasTime = true;
                hostTime = *t;
            }
        }
    }

    const juce::int64 timeline = hasTime ? hostTime : sampleCounter;

    if (expectedNextTimeline >= 0 && timeline != expectedNextTimeline)
        ++diagnostics.timelineJumps;
    expectedNextTimeline = timeline + numSamples;

    diagnostics.hasPlayHead = hasPlayHead;
    diagnostics.hasTimeInSamples = hasTime;
    diagnostics.isPlaying = playing;
    diagnostics.isNonRealtime = isNonRealtime();
    diagnostics.hostTimeInSamples = hostTime;
    diagnostics.sampleCounter = sampleCounter;
    diagnostics.timelinePosition = timeline;
    diagnostics.lastBlockSize = numSamples;

    sampleCounter += numSamples;
    return timeline;
}

void SpectrogramDrawFilterProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();
    diagnostics.numInputChannels = getTotalNumInputChannels();

    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);

    const juce::int64 timeline = resolveTimeline (numSamples);

    const int order = fftOrderForChoice ((int) fftSizeParam->load());
    if (order != spectral.getFftOrder())
    {
        spectral.setFftOrder (order);
        triggerAsyncUpdate(); // latency changes are reported from the message thread
    }

    const float bgDb = bgGainParam->load();
    SpectralMaskProcessor::Settings settings;
    settings.backgroundGain = bgDb <= -144.0f ? 0.0f : juce::Decibels::decibelsToGain (bgDb, -1000.0f);
    settings.invert = invertParam->load() >= 0.5f;

    spectral.process (buffer, timeline, settings);
}

void SpectrogramDrawFilterProcessor::handleAsyncUpdate()
{
    setLatencySamples (spectral.getLatencySamples());
}

//==============================================================================
void SpectrogramDrawFilterProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::ValueTree state ("SpectrogramDrawFilter");
    state.setProperty ("version", 1, nullptr);
    state.appendChild (parameters.copyState(), nullptr);
    state.appendChild (uiState.createCopy(), nullptr);

    juce::MemoryOutputStream maskData;
    mask.writeTo (maskData);
    state.setProperty ("mask", maskData.getMemoryBlock(), nullptr);

    juce::MemoryOutputStream out (destData, false);
    state.writeToStream (out);
}

void SpectrogramDrawFilterProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto state = juce::ValueTree::readFromData (data, (size_t) sizeInBytes);
    if (! state.hasType ("SpectrogramDrawFilter"))
        return;

    if (auto p = state.getChildWithName (parameters.state.getType()); p.isValid())
        parameters.replaceState (p);

    if (auto ui = state.getChildWithName ("UI"); ui.isValid())
        uiState.copyPropertiesAndChildrenFrom (ui, nullptr);

    if (const auto* block = state.getProperty ("mask").getBinaryData())
    {
        juce::MemoryInputStream in (*block, false);
        mask.readFrom (in);
    }
}

juce::AudioProcessorEditor* SpectrogramDrawFilterProcessor::createEditor()
{
    return new SpectrogramDrawFilterEditor (*this);
}
} // namespace sdf

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new sdf::SpectrogramDrawFilterProcessor();
}
