#include "session/Session.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <map>

namespace sc
{
namespace
{
    struct Registry
    {
        juce::CriticalSection lock;
        std::map<juce::String, std::weak_ptr<Session>> sessions;
    };

    Registry& getRegistry()
    {
        static Registry registry;
        return registry;
    }

    void deleteStaleCacheFiles()
    {
        static std::once_flag once;
        std::call_once (once, [] {
            const auto cutoff = juce::Time::getCurrentTime() - juce::RelativeTime::days (30);
            for (const auto& f : Session::getCacheDirectory().findChildFiles (juce::File::findFiles, false))
                if (f.getLastModificationTime() < cutoff)
                    f.deleteFile();
        });
    }

    // Upper bound of audio analysed per time slice, so FFT-size changes stay responsive.
    constexpr juce::int64 kMaxSamplesPerSlice = 1 << 18;
} // namespace

//==============================================================================
Session::Session (const juce::String& sessionId) : id (sessionId)
{
    analysisThread->addTimeSliceClient (this);
}

Session::~Session()
{
    analysisThread->removeTimeSliceClient (this);
    armed = false;
    if (savePending.load())
        saveCapture();
}

std::shared_ptr<Session> Session::getOrCreate (const juce::String& id, bool& created)
{
    deleteStaleCacheFiles();

    auto& reg = getRegistry();
    const juce::ScopedLock sl (reg.lock);

    for (auto it = reg.sessions.begin(); it != reg.sessions.end();)
        it = it->second.expired() ? reg.sessions.erase (it) : std::next (it);

    if (auto existing = reg.sessions[id].lock())
    {
        created = false;
        return existing;
    }

    auto session = std::make_shared<Session> (id);
    reg.sessions[id] = session;
    created = true;
    return session;
}

juce::String Session::createId()
{
    return juce::Uuid().toDashedString();
}

bool Session::hostSharesSessions()
{
    static const bool shares = juce::File (juce::PluginHostType::getHostPath()).getFileName().containsIgnoreCase ("audacity");
    return shares;
}

std::shared_ptr<Session> Session::forkFrom (Session& source)
{
    bool created = false;
    auto copy = getOrCreate (createId(), created);

    juce::MemoryOutputStream maskData;
    source.mask.writeTo (maskData);
    juce::MemoryInputStream in (maskData.getData(), maskData.getDataSize(), false);
    copy->mask.readFrom (in);

    // If the source is still loading its cached audio (e.g. a project just opened), the copy
    // loads its own copy of the cache file instead.
    if (source.loadPending.load())
    {
        if (source.getCacheFile().copyFileTo (copy->getCacheFile()) && source.getCacheInfoFile().copyFileTo (copy->getCacheInfoFile()))
            copy->loadCachedCaptureAsync();
        return copy;
    }

    const juce::ScopedLock cl (source.captureLock);
    const auto& src = source.capture;
    if (src.hasData())
    {
        copy->capture.setSampleRate (src.getSampleRate());
        std::vector<float> block (CaptureBuffer::kChunkSize);
        for (juce::int64 pos = src.getStartSample(); pos < src.getEndSample(); pos += (juce::int64) block.size())
        {
            const int n = (int) juce::jmin ((juce::int64) block.size(), src.getEndSample() - pos);
            src.read (pos, block.data(), n);
            copy->capture.write (pos, block.data(), n, true);
        }
        copy->savePending = true;
    }

    return copy;
}

juce::File Session::getCacheDirectory()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("SpectrogramCanvas")
        .getChildFile ("captures");
}

juce::File Session::getCacheFile() const { return getCacheDirectory().getChildFile (id + ".wav"); }
juce::File Session::getCacheInfoFile() const { return getCacheDirectory().getChildFile (id + ".start"); }

//==============================================================================
void Session::setArmed (bool shouldBeArmed)
{
    const bool was = armed.exchange (shouldBeArmed);
    if (was && ! shouldBeArmed && capture.hasData())
        savePending = true;
}

bool Session::captureBlock (juce::int64 timelineStart, const float* mono, int numSamples, bool offline)
{
    if (! armed.load())
        return false;

    const juce::SpinLock::ScopedTryLockType lock (writeLock);
    if (! lock.isLocked())
        return false;

    capture.write (timelineStart, mono, numSamples, offline);
    lastCaptureMs = juce::Time::getMillisecondCounter();
    savePending = true;
    return true;
}

void Session::clearCapture()
{
    armed = false;
    {
        const juce::SpinLock::ScopedLockType wl (writeLock);
        const juce::ScopedLock cl (captureLock);
        capture.clear();
        spectrogram.store (nullptr);
    }
    savePending = false;
    getCacheFile().deleteFile();
    getCacheInfoFile().deleteFile();
}

void Session::loadCachedCaptureAsync()
{
    if (getCacheFile().existsAsFile())
        loadPending = true;
}

void Session::saveCapture()
{
    savePending = false;
    const juce::ScopedLock cl (captureLock);
    if (capture.saveToWav (getCacheFile()))
        getCacheInfoFile().replaceWithText (juce::String (capture.getStartSample()));
}

//==============================================================================
int Session::useTimeSlice()
{
    capture.topUpPool();

    if (loadPending.load())
    {
        {
            armed = false;
            const juce::SpinLock::ScopedLockType wl (writeLock);
            const juce::ScopedLock cl (captureLock);
            const auto start = getCacheInfoFile().loadFileAsString().getLargeIntValue();
            if (capture.loadFromWav (getCacheFile(), start))
                capture.markAllDirty();
            spectrogram.store (nullptr);
        }
        loadPending = false;
    }

    if (! capture.hasData())
        return 50;

    // (Re)build the spectrogram when the FFT size or sample rate changes.
    auto spec = spectrogram.load();
    const int order = requestedOrder.load();
    if (spec == nullptr || spec->getFftOrder() != order || spec->getSampleRate() != capture.getSampleRate())
    {
        spec = std::make_shared<SpectrogramStore> (order, capture.getSampleRate());
        spectrogram.store (spec);
        capture.markAllDirty();
    }

    juce::int64 start = 0, end = 0;
    if (capture.takeDirtyRange (start, end))
    {
        const juce::int64 sliceEnd = juce::jmin (end, start + kMaxSamplesPerSlice);
        if (sliceEnd < end)
            capture.markDirty (sliceEnd, end);

        const juce::ScopedLock cl (captureLock);
        spec->analyse (capture, start, sliceEnd);
        return sliceEnd < end ? 1 : 10;
    }

    // Save the capture once it has been idle for a moment.
    if (savePending.load() && (! armed.load() || juce::Time::getMillisecondCounter() - lastCaptureMs.load() > 2000))
        saveCapture();

    return 30;
}
} // namespace sc
