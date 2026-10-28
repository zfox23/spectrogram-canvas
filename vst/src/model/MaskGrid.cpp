#include "model/MaskGrid.h"

#include <algorithm>
#include <cmath>

namespace sc
{
namespace
{
    constexpr int kFormatMagic = 0x4d464453; // "SDFM"
    constexpr int kFormatVersion = 1;

    const double kLogRange = std::log (MaskGrid::kMaxHz / MaskGrid::kMinHz);
} // namespace

MaskGrid::MaskGrid()
    : tiles (new std::atomic<Tile*>[(size_t) (kColTiles * kRowTiles)])
{
    for (int i = 0; i < kColTiles * kRowTiles; ++i)
        tiles[(size_t) i].store (nullptr, std::memory_order_relaxed);
}

MaskGrid::~MaskGrid() = default;

//==============================================================================
double MaskGrid::hzToRow (double hz)
{
    return std::log (std::max (hz, 1.0e-3) / kMinHz) / kLogRange * (kRows - 1);
}

double MaskGrid::rowToHz (double row)
{
    return kMinHz * std::exp (row / (kRows - 1) * kLogRange);
}

//==============================================================================
uint8_t MaskGrid::getCell (int col, int row) const noexcept
{
    if (col < 0 || col >= kMaxCols || row < 0 || row >= kRows)
        return 0;

    if (auto* tile = getTile (tileIndex (col / kTileCols, row / kTileRows)))
        return (*tile)[(size_t) ((col % kTileCols) * kTileRows + (row % kTileRows))].load (std::memory_order_relaxed);

    return 0;
}

float MaskGrid::sample (double seconds, double row) const noexcept
{
    float r = (float) row;
    float out = 0.0f;
    sampleColumn (seconds, &r, &out, 1);
    return out;
}

void MaskGrid::sampleColumn (double seconds, const float* rows, float* out, int num) const noexcept
{
    const double colPos = secondsToCol (seconds);
    const int c0 = (int) std::floor (colPos);
    const float cf = (float) (colPos - c0);

    for (int i = 0; i < num; ++i)
    {
        const float rowPos = std::clamp (rows[i], 0.0f, (float) (kRows - 1));
        const int r0 = std::min ((int) rowPos, kRows - 2);
        const float rf = rowPos - (float) r0;

        const float a = getCell (c0, r0) + rf * (float) (getCell (c0, r0 + 1) - getCell (c0, r0));
        const float b = getCell (c0 + 1, r0) + rf * (float) (getCell (c0 + 1, r0 + 1) - getCell (c0 + 1, r0));
        out[i] = (a + cf * (b - a)) * (1.0f / 255.0f);
    }
}

//==============================================================================
MaskGrid::Tile& MaskGrid::getOrCreateTile (int index)
{
    if (auto* t = getTile (index))
        return *t;

    auto tile = std::make_unique<Tile>();
    for (auto& cell : *tile)
        cell.store (0, std::memory_order_relaxed);

    auto* raw = tile.get();
    ownedTiles.push_back (std::move (tile));
    tiles[(size_t) index].store (raw, std::memory_order_release);
    ++numAllocatedTiles;
    return *raw;
}

MaskGrid::TileData MaskGrid::snapshot (int index) const
{
    TileData data ((size_t) kTileCells, 0);
    if (auto* t = getTile (index))
        for (size_t i = 0; i < (size_t) kTileCells; ++i)
            data[i] = (*t)[i].load (std::memory_order_relaxed);
    return data;
}

void MaskGrid::restore (int index, const TileData& data)
{
    auto& t = getOrCreateTile (index);
    for (size_t i = 0; i < (size_t) kTileCells; ++i)
        t[i].store (data[i], std::memory_order_relaxed);
}

void MaskGrid::touchTileForUndo (int index)
{
    if (inStroke && currentEdit.before.find (index) == currentEdit.before.end())
        currentEdit.before.emplace (index, snapshot (index));
}

//==============================================================================
float MaskGrid::brushProfile (float d, float hardness) noexcept
{
    if (d >= 1.0f)
        return 0.0f;

    const float inner = std::clamp (hardness, 0.0f, 1.0f);
    if (d <= inner)
        return 1.0f;

    const float t = (d - inner) / (1.0f - inner);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

void MaskGrid::stamp (const ScreenMapping& m, float cx, float cy, float radius, float hardness, bool erase)
{
    radius = std::max (radius, 0.5f);

    const double t0 = m.xToSeconds (cx - radius), t1 = m.xToSeconds (cx + radius);
    const double hzA = m.yToHz (cy - radius), hzB = m.yToHz (cy + radius);

    const int col0 = std::max (0, (int) std::ceil (secondsToCol (std::min (t0, t1))));
    const int col1 = std::min (kMaxCols - 1, (int) std::floor (secondsToCol (std::max (t0, t1))));
    const int row0 = std::max (0, (int) std::ceil (hzToRow (std::min (hzA, hzB))));
    const int row1 = std::min (kRows - 1, (int) std::floor (hzToRow (std::max (hzA, hzB))));

    auto applyCell = [&] (int col, int row, float value)
    {
        const int index = tileIndex (col / kTileCols, row / kTileRows);
        touchTileForUndo (index);
        auto& cell = getOrCreateTile (index)[(size_t) ((col % kTileCols) * kTileRows + (row % kTileRows))];
        const auto v = (uint8_t) std::lround (value * 255.0f);
        const auto cur = cell.load (std::memory_order_relaxed);
        cell.store (erase ? std::min (cur, (uint8_t) (255 - v)) : std::max (cur, v), std::memory_order_relaxed);
    };

    bool hitAny = false;

    if (col0 <= col1 && row0 <= row1)
    {
        std::vector<float> rowDy ((size_t) (row1 - row0 + 1));
        for (int r = row0; r <= row1; ++r)
            rowDy[(size_t) (r - row0)] = (float) m.hzToY (rowToHz (r)) - cy;

        const float invR = 1.0f / radius;

        for (int c = col0; c <= col1; ++c)
        {
            const float dx = (float) m.secondsToX (colToSeconds (c)) - cx;

            for (int r = row0; r <= row1; ++r)
            {
                const float dy = rowDy[(size_t) (r - row0)];
                const float d = std::sqrt (dx * dx + dy * dy) * invR;

                if (d < 1.0f)
                {
                    applyCell (c, r, brushProfile (d, hardness));
                    hitAny = true;
                }
            }
        }
    }

    // When zoomed in so far that a cell is bigger than the brush, paint the nearest cell.
    if (! hitAny)
    {
        const int col = (int) std::lround (secondsToCol (m.xToSeconds (cx)));
        const int row = (int) std::lround (hzToRow (m.yToHz (cy)));
        if (col >= 0 && col < kMaxCols && row >= 0 && row < kRows)
            applyCell (col, row, 1.0f);
    }

    bumpVersion();
}

//==============================================================================
void MaskGrid::beginStroke()
{
    inStroke = true;
    currentEdit = {};
}

void MaskGrid::endStroke()
{
    if (! inStroke)
        return;

    inStroke = false;
    if (currentEdit.before.empty())
        return;

    for (auto& [index, data] : currentEdit.before)
        currentEdit.after.emplace (index, snapshot (index));

    history.resize ((size_t) undoIndex);
    history.push_back (std::move (currentEdit));
    currentEdit = {};

    if ((int) history.size() > kMaxHistory)
        history.erase (history.begin());

    undoIndex = (int) history.size();
}

void MaskGrid::undo()
{
    if (! canUndo())
        return;

    --undoIndex;
    for (auto& [index, data] : history[(size_t) undoIndex].before)
        restore (index, data);
    bumpVersion();
}

void MaskGrid::redo()
{
    if (! canRedo())
        return;

    for (auto& [index, data] : history[(size_t) undoIndex].after)
        restore (index, data);
    ++undoIndex;
    bumpVersion();
}

void MaskGrid::clear()
{
    beginStroke();
    for (int i = 0; i < kColTiles * kRowTiles; ++i)
    {
        if (auto* t = getTile (i))
        {
            touchTileForUndo (i);
            for (auto& cell : *t)
                cell.store (0, std::memory_order_relaxed);
        }
    }
    endStroke();
    bumpVersion();
}

void MaskGrid::fillRect (int col0, int col1, int row0, int row1, uint8_t value)
{
    col0 = std::max (col0, 0);
    col1 = std::min (col1, kMaxCols - 1);
    row0 = std::max (row0, 0);
    row1 = std::min (row1, kRows - 1);

    for (int c = col0; c <= col1; ++c)
        for (int r = row0; r <= row1; ++r)
        {
            const int index = tileIndex (c / kTileCols, r / kTileRows);
            touchTileForUndo (index);
            getOrCreateTile (index)[(size_t) ((c % kTileCols) * kTileRows + (r % kTileRows))].store (value, std::memory_order_relaxed);
        }

    bumpVersion();
}

juce::Range<int> MaskGrid::getPaintedColumnRange() const
{
    int first = -1, last = -1;
    for (int ct = 0; ct < kColTiles; ++ct)
        for (int rt = 0; rt < kRowTiles; ++rt)
            if (getTile (tileIndex (ct, rt)) != nullptr)
            {
                if (first < 0)
                    first = ct;
                last = ct;
            }

    return first < 0 ? juce::Range<int>() : juce::Range<int> (first * kTileCols, (last + 1) * kTileCols);
}

//==============================================================================
void MaskGrid::writeTo (juce::OutputStream& out) const
{
    juce::MemoryOutputStream raw;
    raw.writeInt (kFormatMagic);
    raw.writeInt (kFormatVersion);
    raw.writeInt (kRows);
    raw.writeInt (kTileCols);
    raw.writeInt (kTileRows);
    raw.writeDouble (kColumnSeconds);

    std::vector<int> nonEmpty;
    for (int i = 0; i < kColTiles * kRowTiles; ++i)
        if (auto* t = getTile (i))
            if (std::any_of (t->begin(), t->end(), [] (const auto& c) { return c.load (std::memory_order_relaxed) != 0; }))
                nonEmpty.push_back (i);

    raw.writeInt ((int) nonEmpty.size());
    for (int index : nonEmpty)
    {
        raw.writeInt (index);
        const auto data = snapshot (index);
        raw.write (data.data(), data.size());
    }

    juce::GZIPCompressorOutputStream gz (out, 6);
    gz.write (raw.getData(), raw.getDataSize());
    gz.flush();
}

bool MaskGrid::readFrom (juce::InputStream& in)
{
    juce::GZIPDecompressorInputStream gz (in);
    juce::MemoryBlock block;
    gz.readIntoMemoryBlock (block);
    juce::MemoryInputStream raw (block, false);

    if (raw.readInt() != kFormatMagic || raw.readInt() != kFormatVersion)
        return false;

    if (raw.readInt() != kRows || raw.readInt() != kTileCols || raw.readInt() != kTileRows
        || raw.readDouble() != kColumnSeconds)
        return false;

    // Zero everything currently held, then load the stored tiles.
    for (int i = 0; i < kColTiles * kRowTiles; ++i)
        if (auto* t = getTile (i))
            for (auto& cell : *t)
                cell.store (0, std::memory_order_relaxed);

    const int count = raw.readInt();
    TileData data ((size_t) kTileCells);
    for (int n = 0; n < count; ++n)
    {
        const int index = raw.readInt();
        if (index < 0 || index >= kColTiles * kRowTiles)
            return false;
        if (raw.read (data.data(), kTileCells) != kTileCells)
            return false;
        restore (index, data);
    }

    history.clear();
    undoIndex = 0;
    bumpVersion();
    return true;
}
} // namespace sc
