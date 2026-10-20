/**
 * The painted time-frequency mask.
 *
 * Lives on a canonical grid that does not depend on FFT size or the display's frequency scale:
 * fixed time columns spanning the audio, and log-spaced frequency rows covering MIN_HZ..MAX_HZ.
 * Each cell holds 0..255 (0 = not painted, 255 = fully painted). Column-major, so a column
 * is contiguous.
 */
export const MASK_ROWS = 768;
export const MIN_HZ = 20;
export const MAX_HZ = 24000;
const TARGET_COLUMN_SECONDS = 0.005;
const MAX_COLS = 8192;
const TILE_COLS = 64;

const LOG_RANGE = Math.log(MAX_HZ / MIN_HZ);

export const hzToRow = (hz: number) => (Math.log(Math.max(hz, 1e-3) / MIN_HZ) / LOG_RANGE) * (MASK_ROWS - 1);
export const rowToHz = (row: number) => MIN_HZ * Math.exp((row / (MASK_ROWS - 1)) * LOG_RANGE);

/** Brush -> screen mapping, supplied by the view so the brush is round on screen at any zoom. */
export interface ScreenMapping {
  secondsToX(s: number): number;
  xToSeconds(x: number): number;
  hzToY(hz: number): number;
  yToHz(y: number): number;
}

interface Edit {
  before: Map<number, Uint8Array>;
  after: Map<number, Uint8Array>;
}

export class MaskGrid {
  readonly cols: number;
  readonly colSeconds: number;
  readonly data: Uint8Array;
  version = 0;

  private history: Edit[] = [];
  private undoIndex = 0;
  private current: Edit | null = null;
  private static readonly MAX_HISTORY = 64;

  constructor(readonly durationSeconds: number) {
    this.cols = Math.max(1, Math.min(MAX_COLS, Math.ceil(durationSeconds / TARGET_COLUMN_SECONDS)));
    this.colSeconds = durationSeconds / this.cols;
    this.data = new Uint8Array(this.cols * MASK_ROWS);
  }

  secondsToCol(s: number) {
    return s / this.colSeconds - 0.5; // column c is centred on (c + 0.5) * colSeconds
  }
  colToSeconds(c: number) {
    return (c + 0.5) * this.colSeconds;
  }

  /** Bilinearly-interpolated value in [0, 1]. */
  sample(seconds: number, row: number): number {
    const colPos = this.secondsToCol(seconds);
    const c0 = Math.floor(colPos);
    const cf = colPos - c0;
    const r = Math.min(Math.max(row, 0), MASK_ROWS - 1);
    const r0 = Math.min(Math.floor(r), MASK_ROWS - 2);
    const rf = r - r0;
    // Columns beyond the audio's ends repeat the edge column, so the ends don't fade out.
    const last = this.cols - 1;
    const cell = (c: number, rr: number) => this.data[(c < 0 ? 0 : c > last ? last : c) * MASK_ROWS + rr];
    const a = cell(c0, r0) + rf * (cell(c0, r0 + 1) - cell(c0, r0));
    const b = cell(c0 + 1, r0) + rf * (cell(c0 + 1, r0 + 1) - cell(c0 + 1, r0));
    return ((a + cf * (b - a)) / 255);
  }

  /** Fills out[i] with the mask value at one time for each fractional row in rows[i]. */
  sampleColumn(seconds: number, rows: ArrayLike<number>, out: Float32Array, num = rows.length) {
    for (let i = 0; i < num; i++) out[i] = this.sample(seconds, rows[i]);
  }

  /** Replaces every cell (not undoable; the edit history is dropped). */
  replaceAll(data: Uint8Array) {
    if (data.length !== this.data.length) throw new Error('Mask size mismatch');
    this.data.set(data);
    this.history = [];
    this.undoIndex = 0;
    this.current = null;
    this.version++;
  }

  isEmpty() {
    return this.data.every((v) => v === 0);
  }

  //==========================================================================
  static brushProfile(d: number, hardness: number): number {
    if (d >= 1) return 0;
    const inner = Math.min(Math.max(hardness, 0), 1);
    if (d <= inner) return 1;
    const t = (d - inner) / (1 - inner);
    return 1 - t * t * (3 - 2 * t);
  }

  /** Stamps a round brush; centre and radius are in screen pixels. erase lowers instead of raises. */
  stamp(m: ScreenMapping, cx: number, cy: number, radius: number, hardness: number, erase: boolean) {
    radius = Math.max(radius, 0.5);
    const t0 = m.xToSeconds(cx - radius), t1 = m.xToSeconds(cx + radius);
    const hzA = m.yToHz(cy - radius), hzB = m.yToHz(cy + radius);

    const col0 = Math.max(0, Math.ceil(this.secondsToCol(Math.min(t0, t1))));
    const col1 = Math.min(this.cols - 1, Math.floor(this.secondsToCol(Math.max(t0, t1))));
    const row0 = Math.max(0, Math.ceil(hzToRow(Math.min(hzA, hzB))));
    const row1 = Math.min(MASK_ROWS - 1, Math.floor(hzToRow(Math.max(hzA, hzB))));

    const apply = (col: number, row: number, value: number) => {
      this.touch(col);
      const i = col * MASK_ROWS + row;
      const v = Math.round(value * 255);
      this.data[i] = erase ? Math.min(this.data[i], 255 - v) : Math.max(this.data[i], v);
    };

    let hit = false;
    if (col0 <= col1 && row0 <= row1) {
      const rowDy = new Float64Array(row1 - row0 + 1);
      for (let r = row0; r <= row1; r++) rowDy[r - row0] = m.hzToY(rowToHz(r)) - cy;
      const invR = 1 / radius;
      for (let c = col0; c <= col1; c++) {
        const dx = m.secondsToX(this.colToSeconds(c)) - cx;
        for (let r = row0; r <= row1; r++) {
          const dy = rowDy[r - row0];
          const d = Math.hypot(dx, dy) * invR;
          if (d < 1) {
            apply(c, r, MaskGrid.brushProfile(d, hardness));
            hit = true;
          }
        }
      }
    }

    // Zoomed in so far that a cell is bigger than the brush: paint the nearest cell.
    if (!hit) {
      const col = Math.round(this.secondsToCol(m.xToSeconds(cx)));
      const row = Math.round(hzToRow(m.yToHz(cy)));
      if (col >= 0 && col < this.cols && row >= 0 && row < MASK_ROWS) apply(col, row, 1);
    }
    this.version++;
  }

  //==========================================================================
  // Undo: whole tiles (TILE_COLS columns) are snapshotted the first time a stroke touches them.
  private tileOf(col: number) {
    return Math.floor(col / TILE_COLS);
  }
  private tileRange(tile: number): [number, number] {
    return [tile * TILE_COLS * MASK_ROWS, Math.min(this.cols, (tile + 1) * TILE_COLS) * MASK_ROWS];
  }
  private snapshot(tile: number) {
    const [a, b] = this.tileRange(tile);
    return this.data.slice(a, b);
  }
  private restore(tile: number, snap: Uint8Array) {
    this.data.set(snap, this.tileRange(tile)[0]);
  }
  private touch(col: number) {
    if (!this.current) return;
    const tile = this.tileOf(col);
    if (!this.current.before.has(tile)) this.current.before.set(tile, this.snapshot(tile));
  }

  beginStroke() {
    this.current = { before: new Map(), after: new Map() };
  }

  endStroke() {
    const edit = this.current;
    this.current = null;
    if (!edit || edit.before.size === 0) return;
    for (const tile of edit.before.keys()) edit.after.set(tile, this.snapshot(tile));
    this.history.length = this.undoIndex;
    this.history.push(edit);
    if (this.history.length > MaskGrid.MAX_HISTORY) this.history.shift();
    this.undoIndex = this.history.length;
  }

  canUndo() {
    return this.undoIndex > 0;
  }
  canRedo() {
    return this.undoIndex < this.history.length;
  }

  undo() {
    if (!this.canUndo()) return;
    for (const [tile, snap] of this.history[--this.undoIndex].before) this.restore(tile, snap);
    this.version++;
  }

  redo() {
    if (!this.canRedo()) return;
    for (const [tile, snap] of this.history[this.undoIndex++].after) this.restore(tile, snap);
    this.version++;
  }

  /** Erases everything (undoable). */
  clear() {
    this.beginStroke();
    for (let t = 0; t * TILE_COLS < this.cols; t++) {
      const [a, b] = this.tileRange(t);
      if (this.data.subarray(a, b).some((v) => v !== 0)) {
        this.current!.before.set(t, this.snapshot(t));
        this.data.fill(0, a, b);
      }
    }
    this.endStroke();
    this.version++;
  }

  /** Paints everything (undoable). */
  fill(value = 255) {
    this.beginStroke();
    for (let t = 0; t * TILE_COLS < this.cols; t++) {
      const [a, b] = this.tileRange(t);
      this.current!.before.set(t, this.snapshot(t));
      this.data.fill(value, a, b);
    }
    this.endStroke();
    this.version++;
  }
}
