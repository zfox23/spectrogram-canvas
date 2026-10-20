#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <memory>

#include "analysis/SpectrogramStore.h"
#include "capture/CaptureBuffer.h"
#include "model/MaskGrid.h"

namespace sdf
{
/**
    Everything one "document" of the plugin works on: the painted mask, the captured audio
    and its spectrogram.

    Sessions are shared between every plugin instance in the process that carries the same
    session ID in its state. That matters in Audacity, which edits settings in one plugin
    instance and processes audio in another: both join the same session, so painting is
    heard immediately and captured audio appears in the editor straight away.

    Captured audio is cached on disk so that it survives every instance being closed.
*/
class Session : private juce::TimeSliceClient
{
public:
    explicit Session (const juce::String& id);
    ~Session() override;

    /** Returns the live session with this ID, creating it if needed (created is set to true). */
    static std::shared_ptr<Session> getOrCreate (const juce::String& id, bool& created);
    static juce::String createId();

    /** Creates a new, independent session holding a copy of another's mask and captured audio. */
    static std::shared_ptr<Session> forkFrom (Session& source);

    /**
        True when the host splits one effect across several plugin instances that must share a
        session (Audacity). Elsewhere, an instance whose state names a session that is already
        in use (e.g. a duplicated track in Reaper) forks its own copy instead.
    */
    static bool hostSharesSessions();

    /** Number of plugin instances currently attached (message thread). */
    int getNumUsers() const noexcept { return users.load(); }
    void addUser() noexcept { ++users; }
    void removeUser() noexcept { --users; }

    const juce::String id;
    MaskGrid mask;
    CaptureBuffer capture;

    /** The spectrogram for the current FFT size (may be rebuilt at any time). */
    std::shared_ptr<SpectrogramStore> getSpectrogram() const { return spectrogram.load(); }

    /** Any thread: the FFT size the spectrogram should be computed with. */
    void requestFftOrder (int order) noexcept { requestedOrder.store (order); }

    //==============================================================================
    bool isArmed() const noexcept { return armed.load(); }
    void setArmed (bool shouldBeArmed);

    /**
        Audio thread: records the mono mix of a block if capture is armed.
        Returns false if not armed (or another instance is writing).
    */
    bool captureBlock (juce::int64 timelineStart, const float* mono, int numSamples, bool offline);

    /** Message thread: discards the captured audio. */
    void clearCapture();

    /** Loads the cached capture for this session, if there is one, on the background thread. */
    void loadCachedCaptureAsync();
    bool isLoading() const noexcept { return loadPending.load(); }

    /** Milliseconds-since-startup of the last captured block. */
    juce::uint32 getLastCaptureTime() const noexcept { return lastCaptureMs.load(); }

    static juce::File getCacheDirectory();

private:
    int useTimeSlice() override;
    void saveCapture();
    juce::File getCacheFile() const;
    juce::File getCacheInfoFile() const;

    struct AnalysisThread : juce::TimeSliceThread
    {
        AnalysisThread() : juce::TimeSliceThread ("Spectrogram analysis") { startThread (juce::Thread::Priority::low); }
        ~AnalysisThread() override { stopThread (2000); }
    };

    juce::SharedResourcePointer<AnalysisThread> analysisThread;

    std::atomic<std::shared_ptr<SpectrogramStore>> spectrogram;
    std::atomic<int> requestedOrder { 12 };
    std::atomic<bool> armed { false };
    std::atomic<int> users { 0 };
    std::atomic<bool> savePending { false };
    std::atomic<bool> loadPending { false };
    std::atomic<juce::uint32> lastCaptureMs { 0 };

    juce::SpinLock writeLock;          // held by the audio thread while writing capture
    juce::CriticalSection captureLock; // held while reading capture in bulk, or replacing it

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Session)
};
} // namespace sdf
