#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <optional>

namespace sc
{
/** The host's loop range, in timeline seconds. */
struct LoopRange
{
    bool valid = false;
    double startSeconds = 0.0, endSeconds = 0.0;
    bool repeatEnabled = false;

    bool operator== (const LoopRange& o) const
    {
        return valid == o.valid && startSeconds == o.startSeconds && endSeconds == o.endSeconds && repeatEnabled == o.repeatEnabled;
    }
    bool operator!= (const LoopRange& o) const { return ! (*this == o); }
};

/**
    Finds the host's loop points.

    In REAPER it asks REAPER directly (via the VST3 IReaperHostApplication extension), which gives
    the exact loop range in seconds at any time, even while stopped. In other hosts it uses the
    loop points from the play head, which are in quarter notes and only arrive while processing.
*/
class HostTransport : public juce::VST3ClientExtensions
{
public:
    /** VST3 entry point: called by the plugin wrapper with the host's IHostApplication. */
    void setIHostApplication (Steinberg::FUnknown* host) override;

    /** Audio thread: records the play head's loop points (fallback for non-REAPER hosts). */
    void updateFromPlayHead (const juce::AudioPlayHead::PositionInfo& pos);

    /** Message thread: the current loop range. */
    LoopRange get() const;

    bool isConnectedToReaper() const noexcept { return getLoopTimeRange != nullptr; }

    /**
        Message thread: the position the listener is hearing right now, in timeline seconds, if
        the host can tell us (REAPER). Hosts process audio ahead of playback, so this differs
        from the position of the audio being processed. Empty when stopped or unknown.
    */
    std::optional<double> getAudiblePlayPosition() const;

private:
    using GetSetLoopTimeRange2 = void (*) (void* project, bool isSet, bool isLoop, double* start, double* end, bool allowAutoSeek);
    using GetSetRepeat = int (*) (int value);
    using GetPlayStateEx = int (*) (void* project);
    using GetPlayPosition2Ex = double (*) (void* project);

    GetSetLoopTimeRange2 getLoopTimeRange = nullptr;
    GetSetRepeat getSetRepeat = nullptr;
    GetPlayStateEx getPlayState = nullptr;
    GetPlayPosition2Ex getPlayPosition = nullptr;
    void* reaperProject = nullptr;

    std::atomic<bool> playHeadValid { false }, playHeadLooping { false };
    std::atomic<double> playHeadStart { 0.0 }, playHeadEnd { 0.0 };
};
} // namespace sc
