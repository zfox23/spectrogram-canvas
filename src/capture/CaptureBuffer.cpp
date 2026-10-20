#include "capture/CaptureBuffer.h"

namespace sdf
{
CaptureBuffer::CaptureBuffer()
    : table (new std::atomic<float*>[(size_t) kMaxChunks])
{
    for (int i = 0; i < kMaxChunks; ++i)
        table[(size_t) i].store (nullptr, std::memory_order_relaxed);
    topUpPool();
}

CaptureBuffer::~CaptureBuffer() = default;

void CaptureBuffer::atomicMin (std::atomic<juce::int64>& a, juce::int64 v)
{
    auto cur = a.load();
    while ((cur == kNone || v < cur) && ! a.compare_exchange_weak (cur, v)) {}
}

void CaptureBuffer::atomicMax (std::atomic<juce::int64>& a, juce::int64 v)
{
    auto cur = a.load();
    while ((cur == kNone || v > cur) && ! a.compare_exchange_weak (cur, v)) {}
}

float* CaptureBuffer::allocateChunk()
{
    auto chunk = std::make_unique<float[]> ((size_t) kChunkSize); // value-initialised to zero
    auto* raw = chunk.get();
    const juce::ScopedLock sl (ownerLock);
    ownedChunks.push_back (std::move (chunk));
    return raw;
}

void CaptureBuffer::topUpPool()
{
    while (poolFifo.getNumReady() < kPoolTarget)
    {
        auto* chunk = allocateChunk();
        const auto scope = poolFifo.write (1);
        if (scope.blockSize1 > 0)
            poolSlots[(size_t) scope.startIndex1] = chunk;
    }
}

float* CaptureBuffer::getChunkForWrite (int index, bool allowAllocation)
{
    if (auto* p = table[(size_t) index].load (std::memory_order_acquire))
        return p;

    float* chunk = nullptr;
    if (poolFifo.getNumReady() > 0)
    {
        const auto scope = poolFifo.read (1);
        if (scope.blockSize1 > 0)
            chunk = poolSlots[(size_t) scope.startIndex1];
    }

    if (chunk == nullptr && allowAllocation)
        chunk = allocateChunk();

    if (chunk != nullptr)
        table[(size_t) index].store (chunk, std::memory_order_release);

    return chunk;
}

bool CaptureBuffer::write (juce::int64 timelineStart, const float* mono, int numSamples, bool allowAllocation)
{
    // Ignore anything before the start of the timeline or beyond capacity.
    int offset = 0;
    if (timelineStart < 0)
    {
        offset = (int) juce::jmin ((juce::int64) numSamples, -timelineStart);
        timelineStart += offset;
    }

    bool ok = true;
    juce::int64 pos = timelineStart;
    int i = offset;

    while (i < numSamples && pos < kMaxSamples)
    {
        const int chunkIndex = (int) (pos >> kChunkBits);
        const int within = (int) (pos & (kChunkSize - 1));
        const int n = juce::jmin (numSamples - i, kChunkSize - within);

        if (auto* chunk = getChunkForWrite (chunkIndex, allowAllocation))
            std::copy (mono + i, mono + i + n, chunk + within);
        else
            ok = false;

        i += n;
        pos += n;
    }

    if (pos > timelineStart)
    {
        if (! hasData())
        {
            startSample = timelineStart;
            endSample = pos;
        }
        else
        {
            atomicMin (startSample, timelineStart);
            atomicMax (endSample, pos);
        }
        atomicMin (dirtyStart, timelineStart);
        atomicMax (dirtyEnd, pos);
    }

    if (! ok)
        overflow = true;
    return ok;
}

void CaptureBuffer::read (juce::int64 start, float* dest, int numSamples) const
{
    int i = 0;
    while (i < numSamples)
    {
        const juce::int64 pos = start + i;
        if (pos < 0 || pos >= kMaxSamples)
        {
            dest[i++] = 0.0f;
            continue;
        }

        const int chunkIndex = (int) (pos >> kChunkBits);
        const int within = (int) (pos & (kChunkSize - 1));
        const int n = juce::jmin (numSamples - i, kChunkSize - within);

        if (const auto* chunk = table[(size_t) chunkIndex].load (std::memory_order_acquire))
            std::copy (chunk + within, chunk + within + n, dest + i);
        else
            std::fill (dest + i, dest + i + n, 0.0f);

        i += n;
    }
}

bool CaptureBuffer::takeDirtyRange (juce::int64& start, juce::int64& end)
{
    // end is taken first so a concurrent write can only widen the next range, never lose data.
    end = dirtyEnd.exchange (kNone);
    start = dirtyStart.exchange (kNone);
    if (start == kNone || end == kNone)
    {
        if (start != kNone)
            atomicMin (dirtyStart, start);
        if (end != kNone)
            atomicMax (dirtyEnd, end);
        return false;
    }
    return end > start;
}

void CaptureBuffer::markDirty (juce::int64 start, juce::int64 end)
{
    if (end > start)
    {
        atomicMin (dirtyStart, start);
        atomicMax (dirtyEnd, end);
    }
}

void CaptureBuffer::markAllDirty()
{
    if (hasData())
    {
        atomicMin (dirtyStart, startSample.load());
        atomicMax (dirtyEnd, endSample.load());
    }
}

void CaptureBuffer::clear()
{
    for (int i = 0; i < kMaxChunks; ++i)
        table[(size_t) i].store (nullptr, std::memory_order_relaxed);

    poolFifo.reset();
    {
        const juce::ScopedLock sl (ownerLock);
        ownedChunks.clear();
    }

    startSample = 0;
    endSample = 0;
    dirtyStart = kNone;
    dirtyEnd = kNone;
    overflow = false;
    topUpPool();
}

bool CaptureBuffer::saveToWav (const juce::File& file) const
{
    if (! hasData())
        return false;

    file.getParentDirectory().createDirectory();
    const auto temp = file.getSiblingFile (file.getFileName() + ".tmp");
    temp.deleteFile();

    {
        std::unique_ptr<juce::OutputStream> stream (temp.createOutputStream());
        if (stream == nullptr)
            return false;

        juce::WavAudioFormat wav;
        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                       .withSampleRate (getSampleRate())
                                                       .withNumChannels (1)
                                                       .withBitsPerSample (16));
        if (writer == nullptr)
            return false;

        const juce::int64 start = getStartSample(), end = getEndSample();
        juce::AudioBuffer<float> block (1, 65536);
        for (juce::int64 pos = start; pos < end; pos += block.getNumSamples())
        {
            const int n = (int) juce::jmin ((juce::int64) block.getNumSamples(), end - pos);
            read (pos, block.getWritePointer (0), n);
            if (! writer->writeFromAudioSampleBuffer (block, 0, n))
                return false;
        }
    }

    return temp.moveFileTo (file);
}

bool CaptureBuffer::loadFromWav (const juce::File& file, juce::int64 timelineStart)
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (file.createInputStream().release(), true));
    if (reader == nullptr)
        return false;

    clear();
    setSampleRate (reader->sampleRate);

    juce::AudioBuffer<float> block (1, 65536);
    for (juce::int64 pos = 0; pos < reader->lengthInSamples; pos += block.getNumSamples())
    {
        const int n = (int) juce::jmin ((juce::int64) block.getNumSamples(), reader->lengthInSamples - pos);
        reader->read (&block, 0, n, pos, true, false);
        write (timelineStart + pos, block.getReadPointer (0), n, true);
    }

    return true;
}
} // namespace sdf
