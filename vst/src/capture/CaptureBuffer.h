#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace sdf
{
/**
    Mono recording of the audio that passed through the plugin, indexed by host-timeline sample.

    Storage is a fixed table of fixed-size chunks. The realtime audio thread never allocates:
    it takes zeroed chunks from a pool that a background thread keeps topped up. During
    offline (non-realtime) rendering the writer may allocate directly.

    Writing must happen from one thread at a time. Reading can happen concurrently with
    writing; clear() and the load functions must not run concurrently with readers or writers.
*/
class CaptureBuffer
{
public:
    static constexpr int kChunkBits = 16;
    static constexpr int kChunkSize = 1 << kChunkBits;
    static constexpr int kMaxChunks = 1 << 16;
    static constexpr juce::int64 kMaxSamples = (juce::int64) kChunkSize * kMaxChunks;

    CaptureBuffer();
    ~CaptureBuffer();

    /** Audio thread. Returns false if samples had to be dropped. */
    bool write (juce::int64 timelineStart, const float* mono, int numSamples, bool allowAllocation);

    /** Copies samples into dest; anything never captured reads as silence. */
    void read (juce::int64 start, float* dest, int numSamples) const;

    bool hasData() const noexcept { return endSample.load() > startSample.load(); }
    juce::int64 getStartSample() const noexcept { return startSample.load(); }
    juce::int64 getEndSample() const noexcept { return endSample.load(); }

    double getSampleRate() const noexcept { return sampleRate.load(); }
    void setSampleRate (double sr) noexcept { sampleRate.store (sr); }

    /** Returns and resets the range written since the last call; false if nothing is new. */
    bool takeDirtyRange (juce::int64& start, juce::int64& end);
    void markAllDirty();
    void markDirty (juce::int64 start, juce::int64 end);

    /** Background thread: keeps the pool of free chunks full. */
    void topUpPool();

    void clear();

    bool saveToWav (const juce::File& file) const;
    bool loadFromWav (const juce::File& file, juce::int64 timelineStart);

    bool hasOverflowed() const noexcept { return overflow.load(); }

private:
    float* getChunkForWrite (int index, bool allowAllocation);
    float* allocateChunk();
    static void atomicMin (std::atomic<juce::int64>& a, juce::int64 v);
    static void atomicMax (std::atomic<juce::int64>& a, juce::int64 v);

    std::unique_ptr<std::atomic<float*>[]> table;

    static constexpr int kPoolCapacity = 64;
    static constexpr int kPoolTarget = 32;
    juce::AbstractFifo poolFifo { kPoolCapacity };
    std::array<float*, kPoolCapacity> poolSlots {};

    juce::CriticalSection ownerLock;
    std::vector<std::unique_ptr<float[]>> ownedChunks;

    static constexpr juce::int64 kNone = -1;
    std::atomic<juce::int64> startSample { 0 }, endSample { 0 };
    std::atomic<juce::int64> dirtyStart { kNone }, dirtyEnd { kNone };
    std::atomic<double> sampleRate { 44100.0 };
    std::atomic<bool> overflow { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CaptureBuffer)
};
} // namespace sdf
