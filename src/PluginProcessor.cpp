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

    bool created = false;
    setSession (Session::getOrCreate (Session::createId(), created));
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
std::shared_ptr<Session> SpectrogramDrawFilterProcessor::getSession() const
{
    const juce::SpinLock::ScopedLockType sl (sessionLock);
    return session;
}

void SpectrogramDrawFilterProcessor::setSession (std::shared_ptr<Session> newSession)
{
    newSession->requestFftOrder (fftOrderForChoice ((int) fftSizeParam->load()));
    {
        const juce::SpinLock::ScopedLockType sl (sessionLock);
        std::swap (session, newSession);
    }
    // The previous session (now in newSession) is released here, off the audio thread.
}

//==============================================================================
void SpectrogramDrawFilterProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    spectral.prepare (sampleRate, juce::jmax (getTotalNumInputChannels(), getTotalNumOutputChannels()));
    spectral.setFftOrder (fftOrderForChoice ((int) fftSizeParam->load()));
    setLatencySamples (spectral.getLatencySamples());
    monoScratch.setSize (1, juce::jmax (samplesPerBlock, 4096));

    sampleCounter = 0;
    expectedNextTimeline = -1;
    wasPlaying = false;
    diagnostics.sampleRate = sampleRate;
    ++diagnostics.prepareCount;
}

void SpectrogramDrawFilterProcessor::releaseResources()
{
    // Rendering has finished (e.g. Audacity's Apply), so stop capturing.
    if (auto s = getSession(); s != nullptr && s->isArmed() && s->capture.hasData())
        s->setArmed (false);
}

void SpectrogramDrawFilterProcessor::reset()
{
    spectral.reset();
    sampleCounter = 0;
    expectedNextTimeline = -1;
    ++diagnostics.resetCount;
}

juce::int64 SpectrogramDrawFilterProcessor::resolveTimeline (int numSamples, bool& playing, bool& hasTransport)
{
    bool hasPlayHead = false, hasTime = false;
    juce::int64 hostTime = 0;
    playing = false;

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

    hasTransport = hasTime;
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
    diagnostics.lastProcessMs = juce::Time::getMillisecondCounter();

    sampleCounter += numSamples;
    return timeline;
}

void SpectrogramDrawFilterProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();
    const int numIn = getTotalNumInputChannels();
    diagnostics.numInputChannels = numIn;

    for (int ch = numIn; ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);

    const juce::SpinLock::ScopedTryLockType sl (sessionLock);
    if (! sl.isLocked() || session == nullptr)
    {
        buffer.clear(); // state is being swapped right now; skip one block
        return;
    }

    bool playing = false, hasTransport = false;
    const juce::int64 timeline = resolveTimeline (numSamples, playing, hasTransport);

    // Stop capturing when the host transport stops.
    if (hasTransport && wasPlaying && ! playing && session->isArmed() && session->capture.hasData())
        session->setArmed (false);
    wasPlaying = playing;

    const int order = fftOrderForChoice ((int) fftSizeParam->load());
    session->requestFftOrder (order);
    if (order != spectral.getFftOrder())
    {
        spectral.setFftOrder (order);
        triggerAsyncUpdate(); // latency changes are reported from the message thread
    }

    SpectralMaskProcessor::Settings settings;
    const float bgDb = bgGainParam->load();
    settings.backgroundGain = bgDb <= -144.0f ? 0.0f : juce::Decibels::decibelsToGain (bgDb, -1000.0f);
    settings.invert = invertParam->load() >= 0.5f;

    // Hosts that report a transport may keep processing while stopped; only record real playback.
    if (session->isArmed() && numIn > 0 && (playing || ! hasTransport))
    {
        session->capture.setSampleRate (getSampleRate());

        // Record the mono mix, in pieces no larger than the scratch buffer.
        for (int offset = 0; offset < numSamples;)
        {
            const int n = juce::jmin (numSamples - offset, monoScratch.getNumSamples());
            monoScratch.copyFrom (0, 0, buffer, 0, offset, n);
            for (int ch = 1; ch < numIn; ++ch)
                monoScratch.addFrom (0, 0, buffer, ch, offset, n);
            if (numIn > 1)
                monoScratch.applyGain (0, 0, n, 1.0f / (float) numIn);
            session->captureBlock (timeline + offset, monoScratch.getReadPointer (0), n, isNonRealtime());
            offset += n;
        }

        // While capturing, pass the audio through unchanged (but with the same latency).
        settings.bypassMask = true;
    }

    spectral.setMask (&session->mask);
    spectral.process (buffer, timeline, settings);
}

void SpectrogramDrawFilterProcessor::handleAsyncUpdate()
{
    setLatencySamples (spectral.getLatencySamples());
}

//==============================================================================
void SpectrogramDrawFilterProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto s = getSession();

    juce::ValueTree state ("SpectrogramDrawFilter");
    state.setProperty ("version", 1, nullptr);
    state.setProperty ("session", s->id, nullptr);
    state.appendChild (parameters.copyState(), nullptr);
    state.appendChild (uiState.createCopy(), nullptr);

    juce::MemoryOutputStream maskData;
    s->mask.writeTo (maskData);
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

    const juce::String id = state.getProperty ("session").toString();
    if (id.isEmpty() || id == getSession()->id)
        return;

    bool created = false;
    auto newSession = Session::getOrCreate (id, created);

    // A session that is already live in this process is authoritative; only a newly
    // re-created one takes its mask from the saved state and its audio from the cache.
    if (created)
    {
        if (const auto* block = state.getProperty ("mask").getBinaryData())
        {
            juce::MemoryInputStream in (*block, false);
            newSession->mask.readFrom (in);
        }
        newSession->loadCachedCaptureAsync();
    }

    setSession (std::move (newSession));
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
