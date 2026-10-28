#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace sc
{
/**
    The painted time-frequency mask.

    The mask lives on a canonical grid that does not depend on FFT size, sample rate or the
    display's frequency scale: fixed-width time columns, and log-spaced frequency rows
    covering kMinHz..kMaxHz. Each cell holds 0..255 (0 = not painted, 255 = fully painted).

    Storage is a fixed table of lazily-allocated 2D tiles. Cells are relaxed atomics, so the
    message thread can paint while the audio thread reads, without locks. Tiles are only
    ever allocated (never freed) while the plugin runs, so the audio thread can safely
    follow a tile pointer once it has loaded it.
*/
class MaskGrid
{
public:
    static constexpr double kColumnSeconds = 0.005;
    static constexpr int kRows = 1024;
    static constexpr double kMinHz = 20.0;
    static constexpr double kMaxHz = 24000.0;
    static constexpr int kTileCols = 256;
    static constexpr int kTileRows = 128;
    static constexpr int kRowTiles = kRows / kTileRows;
    static constexpr double kMaxSeconds = 4.0 * 60.0 * 60.0; // 4 hours
    static constexpr int kColTiles = (int) (kMaxSeconds / kColumnSeconds) / kTileCols + 1;
    static constexpr int kMaxCols = kColTiles * kTileCols;

    MaskGrid();
    ~MaskGrid();

    //==============================================================================
    // Coordinate helpers
    static double hzToRow (double hz);
    static double rowToHz (double row);
    static double secondsToCol (double seconds) { return seconds / kColumnSeconds; }
    static double colToSeconds (double col) { return col * kColumnSeconds; }

    //==============================================================================
    // Reading (any thread, realtime-safe)
    uint8_t getCell (int col, int row) const noexcept;

    /** Bilinearly-interpolated mask value in [0, 1] at a time (seconds) and frequency (rows). */
    float sample (double seconds, double row) const noexcept;

    /** Fills out[i] with the mask value at the given time for each fractional row in rows[]. */
    void sampleColumn (double seconds, const float* rows, float* out, int num) const noexcept;

    bool isEmpty() const noexcept { return numAllocatedTiles.load() == 0; }

    /** Increments every time the mask changes; the UI uses it to know when to repaint. */
    uint32_t getVersion() const noexcept { return version.load (std::memory_order_acquire); }

    //==============================================================================
    // Writing (message thread)

    /** Maps between canonical mask coordinates and screen pixels for brush stamping. */
    struct ScreenMapping
    {
        std::function<double (double seconds)> secondsToX;
        std::function<double (double x)> xToSeconds;
        std::function<double (double hz)> hzToY;
        std::function<double (double y)> yToHz;
    };

    /**
        Stamps a round brush whose centre and radius are given in screen pixels. The brush
        is round on screen regardless of zoom or frequency scale.
        hardness is in [0, 1]: 1 = hard edge, 0 = fully feathered.
    */
    void stamp (const ScreenMapping& mapping, float cx, float cy, float radius, float hardness, bool erase);

    /** Profile of the brush at a normalised distance d (0 = centre, 1 = edge). */
    static float brushProfile (float d, float hardness) noexcept;

    void beginStroke();
    void endStroke();

    bool canUndo() const { return undoIndex > 0; }
    bool canRedo() const { return undoIndex < (int) history.size(); }
    void undo();
    void redo();

    /** Clears the whole mask (undoable). */
    void clear();

    /** Sets every cell in a column/row rectangle to the given value (used by tests). */
    void fillRect (int col0, int col1, int row0, int row1, uint8_t value);

    //==============================================================================
    void writeTo (juce::OutputStream& out) const;
    bool readFrom (juce::InputStream& in);

    /** Range of columns that contain painted tiles, or {0, 0} if empty. */
    juce::Range<int> getPaintedColumnRange() const;

private:
    static constexpr int kTileCells = kTileCols * kTileRows;
    using Tile = std::array<std::atomic<uint8_t>, kTileCells>;
    using TileData = std::vector<uint8_t>;

    static int tileIndex (int colTile, int rowTile) noexcept { return colTile * kRowTiles + rowTile; }

    Tile* getTile (int index) const noexcept { return tiles[(size_t) index].load (std::memory_order_acquire); }
    Tile& getOrCreateTile (int index);
    TileData snapshot (int index) const;
    void restore (int index, const TileData& data);
    void touchTileForUndo (int index);
    void bumpVersion() { version.fetch_add (1, std::memory_order_acq_rel); }

    std::unique_ptr<std::atomic<Tile*>[]> tiles;
    std::vector<std::unique_ptr<Tile>> ownedTiles;
    std::atomic<int> numAllocatedTiles { 0 };
    std::atomic<uint32_t> version { 0 };

    struct Edit
    {
        std::map<int, TileData> before, after;
    };

    bool inStroke = false;
    Edit currentEdit;
    std::vector<Edit> history;
    int undoIndex = 0;
    static constexpr int kMaxHistory = 64;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MaskGrid)
};
} // namespace sc
