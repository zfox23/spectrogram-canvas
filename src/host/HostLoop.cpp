#include "host/HostLoop.h"

JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wnon-virtual-dtor", "-Wpragma-pack")
JUCE_BEGIN_IGNORE_WARNINGS_MSVC (4100)
#include <pluginterfaces/base/funknown.h>

namespace reaper
{
using namespace Steinberg;
using uint32 = Steinberg::uint32;

// From the reaper-sdk (zlib-style licence), as shipped in JUCE's examples/Plugins/extern.
class IReaperHostApplication : public FUnknown // available from IHostApplication in REAPER v5.02+
{
public:
    virtual void* PLUGIN_API getReaperApi (CStringA funcname) = 0;
    virtual void* PLUGIN_API getReaperParent (uint32 w) = 0; // track(=1), take(=2), project(=3), fxdsp(=4), trackchan(=5)
    virtual void* PLUGIN_API reaperExtended (uint32 call, void* parm1, void* parm2, void* parm3) = 0;

    static const FUID iid;
};

DECLARE_CLASS_IID (IReaperHostApplication, 0x79655E36, 0x77EE4267, 0xA573FEF7, 0x4912C27C)
DEF_CLASS_IID (IReaperHostApplication)
} // namespace reaper

JUCE_END_IGNORE_WARNINGS_MSVC
JUCE_END_IGNORE_WARNINGS_GCC_LIKE

namespace sdf
{
void HostLoop::setIHostApplication (Steinberg::FUnknown* host)
{
    if (host == nullptr)
        return;

    void* obj = nullptr;
    if (host->queryInterface (reaper::IReaperHostApplication::iid, &obj) != Steinberg::kResultOk || obj == nullptr)
        return;

    auto* reaperHost = static_cast<reaper::IReaperHostApplication*> (obj);
    getLoopTimeRange = reinterpret_cast<GetSetLoopTimeRange2> (reaperHost->getReaperApi ("GetSet_LoopTimeRange2"));
    getSetRepeat = reinterpret_cast<GetSetRepeat> (reaperHost->getReaperApi ("GetSetRepeat"));
    reaperProject = reaperHost->getReaperParent (3);
    reaperHost->release();
}

void HostLoop::updateFromPlayHead (const juce::AudioPlayHead::PositionInfo& pos)
{
    const auto loop = pos.getLoopPoints();
    const auto bpm = pos.getBpm();
    const auto ppq = pos.getPpqPosition();
    const auto seconds = pos.getTimeInSeconds();

    if (! loop || ! bpm || ! ppq || ! seconds || *bpm <= 0.0 || loop->ppqEnd <= loop->ppqStart)
    {
        playHeadValid = false;
        return;
    }

    // Convert quarter notes to seconds around the current position (exact for a constant tempo).
    const double secondsPerQuarter = 60.0 / *bpm;
    playHeadStart = *seconds + (loop->ppqStart - *ppq) * secondsPerQuarter;
    playHeadEnd = *seconds + (loop->ppqEnd - *ppq) * secondsPerQuarter;
    playHeadLooping = pos.getIsLooping();
    playHeadValid = true;
}

LoopRange HostLoop::get() const
{
    LoopRange r;

    if (getLoopTimeRange != nullptr)
    {
        double start = 0.0, end = 0.0;
        getLoopTimeRange (reaperProject, false, true, &start, &end, false);
        r.valid = end > start;
        r.startSeconds = start;
        r.endSeconds = end;
        r.repeatEnabled = getSetRepeat != nullptr && getSetRepeat (-1) != 0;
        return r;
    }

    if (playHeadValid.load())
    {
        r.valid = true;
        r.startSeconds = playHeadStart.load();
        r.endSeconds = playHeadEnd.load();
        r.repeatEnabled = playHeadLooping.load();
    }
    return r;
}
} // namespace sdf
