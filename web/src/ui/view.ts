import type { Spectrogram } from '../dsp/analysis';
import { FreqScale, type FreqScaleType } from '../model/freqScale';
import { MaskGrid, hzToRow, type ScreenMapping } from '../model/maskGrid';

export const RULER_W = 52;
const RULER_H = 22;
const MIN_SPAN = 0.02;
const MIN_NORM_SPAN = 0.02;

export const colors = {
  bg: '#121419', panel: '#1b1e25', text: '#9aa0ad', grid: 'rgba(154,160,173,0.28)',
  paint: [0x4c, 0xc9, 0xf0], erase: '#ff6b6b', playhead: '#ffffff', marker: '#7bd88f',
};

// Magma-like ramp: black -> indigo -> magenta -> orange -> pale yellow.
const COLOR_LUT: Uint8ClampedArray = (() => {
  const stops = ['000004', '1c1044', '4f127b', '812581', 'b5367a', 'e55064', 'fb8761', 'fec287', 'fcfdbf']
    .map((h) => [0, 2, 4].map((i) => parseInt(h.slice(i, i + 2), 16)));
  const lut = new Uint8ClampedArray(256 * 4);
  for (let i = 0; i < 256; i++) {
    const pos = (i / 255) * (stops.length - 1);
    const s = Math.min(Math.floor(pos), stops.length - 2), f = pos - s;
    for (let c = 0; c < 3; c++) lut[i * 4 + c] = stops[s][c] + f * (stops[s + 1][c] - stops[s][c]);
    lut[i * 4 + 3] = 255;
  }
  return lut;
})();

export interface TimeFreqWindow { start: number; end: number; normLow: number; normHigh: number }

export interface LoopRange { start: number; end: number; enabled: boolean }

type Tool = 'paint' | 'erase' | 'pan' | 'scrub' | 'pinch' | 'loop-start' | 'loop-end' | 'loop-new' | null;

const EDGE_PX = 6;
const DRAG_PX = 4;

export class SpectrogramView {
  readonly canvas = document.createElement('canvas');
  private ctx = this.canvas.getContext('2d')!;
  private specLayer = document.createElement('canvas');
  private maskLayer = document.createElement('canvas');

  spec: Spectrogram | null = null;
  mask: MaskGrid | null = null;
  duration = 0;
  readonly scale = new FreqScale('log', 20, 22050);

  startSeconds = 0;
  endSeconds = 1;
  private normLow = 0;
  private normHigh = 1;

  brushRadius = 18;
  hardness = 0.5;
  invert = false;
  playhead: number | null = null;
  /** Where playback begins; set by clicking the time ruler. */
  startMarker = 0;
  loop: LoopRange | null = null;

  /** Called after a stroke edits the mask. */
  onEdit: (() => void) | null = null;
  /** Called when the user clicks the time ruler: seek to this time. */
  onSeek: ((seconds: number) => void) | null = null;
  /** Called while the loop range is dragged (final = false) and when the drag ends (final = true). */
  onLoopChange: ((range: { start: number; end: number } | null, final: boolean) => void) | null = null;
  /** Called after every draw so dependent views (the overview) can follow. */
  onDraw: (() => void) | null = null;

  private specDirty = true;
  private maskDirty = true;
  private renderedMaskVersion = -1;
  private drawQueued = false;
  private tool: Tool = null;
  private last = { x: 0, y: 0 };
  private mouse: { x: number; y: number } | null = null;
  private pointers = new Map<number, { x: number; y: number }>();
  private pinch = { dist: 0, span: 1, anchor: 0 };
  private strokeTravel = 0;
  private rulerDown = { x: 0, dragged: false };
  private dpr = 1;
  private cssW = 0;
  private cssH = 0;

  constructor(host: HTMLElement) {
    host.appendChild(this.canvas);
    this.canvas.style.cursor = 'none';
    this.canvas.style.touchAction = 'none';

    new ResizeObserver(() => this.resize()).observe(host);
    const c = this.canvas;
    c.addEventListener('pointerdown', (e) => this.pointerDown(e));
    c.addEventListener('pointermove', (e) => this.pointerMove(e));
    c.addEventListener('pointerup', (e) => this.pointerUp(e));
    c.addEventListener('pointercancel', (e) => this.pointerUp(e));
    c.addEventListener('pointerleave', () => { this.mouse = null; this.redraw(); });
    c.addEventListener('wheel', (e) => this.wheel(e), { passive: false });
    c.addEventListener('contextmenu', (e) => e.preventDefault());
  }

  //==========================================================================
  setAudio(duration: number, sampleRate: number, mask: MaskGrid) {
    this.duration = duration;
    this.mask = mask;
    this.scale.maxHz = Math.min(sampleRate / 2, 24000);
    this.spec = null;
    this.fit();
  }

  setSpectrogram(spec: Spectrogram | null) {
    this.spec = spec;
    this.specDirty = true;
    this.redraw();
  }

  setScaleType(type: FreqScaleType) {
    this.scale.type = type;
    this.specDirty = this.maskDirty = true;
    this.redraw();
  }

  setInvert(v: boolean) {
    this.invert = v;
    this.maskDirty = true;
    this.redraw();
  }

  getViewWindow(): TimeFreqWindow { return this.window(); }

  setViewWindow(w: TimeFreqWindow) {
    this.setNormWindow(w.normLow, w.normHigh);
    this.setWindow(w.start, w.end);
    this.redraw();
  }

  /** A picture of the whole file: spectrogram, painted mask, loop range and start marker. */
  renderPreview(width: number, height: number): HTMLCanvasElement {
    const out = document.createElement('canvas');
    out.width = width; out.height = height;
    const g = out.getContext('2d')!;
    const whole = { start: 0, end: Math.max(this.duration, MIN_SPAN), normLow: 0, normHigh: 1 };
    renderSpectrogramTo(g, width, height, this.spec, this.scale, whole);
    const mask = document.createElement('canvas');
    mask.width = width; mask.height = height;
    renderMaskTo(mask.getContext('2d')!, width, height, this.mask, this.scale, whole, this.invert);
    g.drawImage(mask, 0, 0);
    const x = (t: number) => (t / whole.end) * width;
    if (this.loop) {
      g.fillStyle = 'rgba(76,201,240,0.18)';
      g.fillRect(x(this.loop.start), 0, Math.max(1, x(this.loop.end) - x(this.loop.start)), height);
      g.fillStyle = 'rgba(76,201,240,0.9)';
      g.fillRect(x(this.loop.start), 0, 2, height);
      g.fillRect(x(this.loop.end) - 2, 0, 2, height);
    }
    g.fillStyle = colors.marker;
    g.fillRect(x(this.startMarker), 0, 2, height);
    return out;
  }

  setTimeRange(start: number, end: number) {
    this.setWindow(start, end);
    this.redraw();
  }

  fit() {
    this.startSeconds = 0;
    this.endSeconds = Math.max(this.duration, MIN_SPAN);
    this.normLow = 0;
    this.normHigh = 1;
    this.specDirty = this.maskDirty = true;
    this.redraw();
  }

  maskChanged() {
    this.maskDirty = true;
    this.redraw();
  }

  redraw() {
    if (this.drawQueued) return;
    this.drawQueued = true;
    requestAnimationFrame(() => { this.drawQueued = false; this.draw(); });
  }

  //==========================================================================
  private get span() { return this.endSeconds - this.startSeconds; }
  private plot() { return { x: RULER_W, y: 0, w: Math.max(1, this.cssW - RULER_W), h: Math.max(1, this.cssH - RULER_H) }; }

  private xToSeconds = (x: number) => this.startSeconds + ((x - RULER_W) / this.plot().w) * this.span;
  private secondsToX = (s: number) => RULER_W + ((s - this.startSeconds) / this.span) * this.plot().w;
  private yToNorm = (y: number) => this.normHigh - (y / this.plot().h) * (this.normHigh - this.normLow);
  private normToY = (n: number) => ((this.normHigh - n) / (this.normHigh - this.normLow)) * this.plot().h;
  private hzToY = (hz: number) => this.normToY(this.scale.hzToNorm(hz));

  private mapping(): ScreenMapping {
    return {
      secondsToX: this.secondsToX,
      xToSeconds: this.xToSeconds,
      hzToY: this.hzToY,
      yToHz: (y) => this.scale.normToHz(this.yToNorm(y)),
    };
  }

  private resize() {
    const r = this.canvas.parentElement!.getBoundingClientRect();
    this.dpr = window.devicePixelRatio || 1;
    this.cssW = Math.floor(r.width);
    this.cssH = Math.floor(r.height);
    this.canvas.width = Math.floor(this.cssW * this.dpr);
    this.canvas.height = Math.floor(this.cssH * this.dpr);
    this.canvas.style.width = `${this.cssW}px`;
    this.canvas.style.height = `${this.cssH}px`;
    const p = this.plot();
    for (const l of [this.specLayer, this.maskLayer]) { l.width = p.w; l.height = p.h; }
    this.specDirty = this.maskDirty = true;
    this.redraw();
  }

  //==========================================================================
  private window(): TimeFreqWindow {
    return { start: this.startSeconds, end: this.endSeconds, normLow: this.normLow, normHigh: this.normHigh };
  }

  private renderSpectrogram() {
    const { w, h } = this.plot();
    renderSpectrogramTo(this.specLayer.getContext('2d')!, w, h, this.spec, this.scale, this.window());
  }

  private renderMask() {
    const { w, h } = this.plot();
    renderMaskTo(this.maskLayer.getContext('2d')!, w, h, this.mask, this.scale, this.window(), this.invert);
    this.renderedMaskVersion = this.mask?.version ?? -1;
  }

  private draw() {
    if (this.cssW <= 0 || this.cssH <= 0) return;
    if (this.specDirty) { this.renderSpectrogram(); this.specDirty = false; }
    if (this.maskDirty || (this.mask && this.mask.version !== this.renderedMaskVersion)) {
      this.renderMask();
      this.maskDirty = false;
    }

    const g = this.ctx;
    const p = this.plot();
    g.setTransform(this.dpr, 0, 0, this.dpr, 0, 0);
    g.fillStyle = colors.bg;
    g.fillRect(0, 0, this.cssW, this.cssH);
    g.imageSmoothingEnabled = false;
    g.drawImage(this.specLayer, p.x, p.y);
    g.drawImage(this.maskLayer, p.x, p.y);

    this.drawRulers(g, p);

    this.drawLoop(g, p);

    g.save();
    g.beginPath();
    g.rect(p.x, p.y, p.w, p.h);
    g.clip();
    if (this.startMarker >= this.startSeconds && this.startMarker <= this.endSeconds) {
      const x = Math.round(this.secondsToX(this.startMarker)) + 0.5;
      g.strokeStyle = colors.marker;
      g.lineWidth = 1;
      g.beginPath(); g.moveTo(x, 0); g.lineTo(x, p.h); g.stroke();
    }
    if (this.playhead !== null) {
      const x = Math.round(this.secondsToX(this.playhead)) + 0.5;
      g.strokeStyle = colors.playhead;
      g.lineWidth = 1;
      g.beginPath(); g.moveTo(x, 0); g.lineTo(x, p.h); g.stroke();
    }
    if (this.mouse && this.mouse.x >= p.x && this.mouse.y < p.h) {
      g.strokeStyle = this.tool === 'erase' ? colors.erase : '#ffffff';
      g.lineWidth = 1.5;
      g.beginPath(); g.arc(this.mouse.x, this.mouse.y, this.brushRadius, 0, Math.PI * 2); g.stroke();
    }
    g.restore();
    this.onDraw?.();
  }

  private drawLoop(g: CanvasRenderingContext2D, p: { x: number; y: number; w: number; h: number }) {
    const loop = this.loop;
    if (!loop) return;
    const x0 = Math.max(p.x, this.secondsToX(loop.start)), x1 = Math.min(p.x + p.w, this.secondsToX(loop.end));
    const rgb = loop.enabled ? '76,201,240' : '154,160,173';
    if (x1 > x0) {
      g.fillStyle = `rgba(${rgb},${loop.enabled ? 0.14 : 0.08})`;
      g.fillRect(x0, 0, x1 - x0, p.h);
      g.fillStyle = `rgba(${rgb},${loop.enabled ? 0.55 : 0.3})`;
      g.fillRect(x0, p.h, x1 - x0, RULER_H);
    }
    g.strokeStyle = `rgba(${rgb},0.9)`;
    g.lineWidth = 1.5;
    for (const t of [loop.start, loop.end]) {
      const x = Math.round(this.secondsToX(t)) + 0.5;
      if (x < p.x || x > p.x + p.w) continue;
      g.beginPath(); g.moveTo(x, 0); g.lineTo(x, p.h + RULER_H); g.stroke();
    }
  }

  private drawRulers(g: CanvasRenderingContext2D, p: { x: number; y: number; w: number; h: number }) {
    g.fillStyle = colors.panel;
    g.fillRect(0, 0, RULER_W, this.cssH);
    g.fillRect(0, p.h, this.cssW, RULER_H);
    g.font = '11px system-ui, sans-serif';
    g.fillStyle = colors.text;
    g.strokeStyle = colors.grid;
    g.lineWidth = 1;

    // Time
    const steps = [0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800];
    const step = steps.find((s) => (s / this.span) * p.w >= 80) ?? steps[steps.length - 1];
    g.textAlign = 'left';
    g.textBaseline = 'middle';
    for (let t = Math.ceil(this.startSeconds / step) * step; t <= this.endSeconds; t += step) {
      const x = Math.round(this.secondsToX(t)) + 0.5;
      g.beginPath(); g.moveTo(x, 0); g.lineTo(x, p.h + 4); g.stroke();
      g.fillText(formatTime(t, step), x + 3, p.h + RULER_H / 2 + 1);
    }

    // Start marker
    if (this.startMarker >= this.startSeconds && this.startMarker <= this.endSeconds) {
      const x = this.secondsToX(this.startMarker);
      g.fillStyle = colors.marker;
      g.beginPath(); g.moveTo(x - 5, p.h + RULER_H); g.lineTo(x + 5, p.h + RULER_H); g.lineTo(x, p.h + RULER_H - 7); g.fill();
    }

    // Frequency
    let ticks: number[];
    if (this.scale.type === 'linear') {
      const range = this.scale.maxHz * (this.normHigh - this.normLow);
      const nice = [100, 200, 500, 1000, 2000, 5000];
      const stepHz = nice.find((s) => (s / range) * p.h >= 36) ?? 5000;
      ticks = [];
      for (let f = stepHz; f < this.scale.maxHz; f += stepHz) ticks.push(f);
    } else {
      ticks = [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000];
    }
    g.textAlign = 'right';
    let lastY = Infinity;
    for (const hz of ticks) {
      if (hz > this.scale.maxHz) continue;
      const y = this.hzToY(hz);
      if (y < 6 || y > p.h - 6 || Math.abs(y - lastY) < 16) continue;
      lastY = y;
      g.beginPath(); g.moveTo(RULER_W - 4, Math.round(y) + 0.5); g.lineTo(this.cssW, Math.round(y) + 0.5); g.stroke();
      g.fillText(hz >= 1000 ? `${hz / 1000}k` : `${hz}`, RULER_W - 7, y);
    }
  }

  //==========================================================================
  private local(e: MouseEvent) {
    const r = this.canvas.getBoundingClientRect();
    return { x: e.clientX - r.left, y: e.clientY - r.top };
  }

  private stampLine(from: { x: number; y: number }, to: { x: number; y: number }, erase: boolean) {
    if (!this.mask) return;
    const m = this.mapping();
    const dist = Math.hypot(to.x - from.x, to.y - from.y);
    const n = Math.max(1, Math.ceil(dist / Math.max(2, this.brushRadius / 3)));
    for (let i = 1; i <= n; i++) {
      const t = i / n;
      this.mask.stamp(m, from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t, this.brushRadius, this.hardness, erase);
    }
  }

  private clampTime = (t: number) => Math.min(Math.max(t, 0), this.duration);

  private beginPinch() {
    if (this.tool === 'paint' || this.tool === 'erase') {
      // The first finger only started a stroke: drop it if it was just a dab.
      this.mask?.endStroke();
      if (this.strokeTravel < 12) this.mask?.undo();
      this.maskDirty = true;
      this.onEdit?.();
    }
    const [a, b] = [...this.pointers.values()];
    this.tool = 'pinch';
    this.pinch = {
      dist: Math.max(1, Math.abs(a.x - b.x)),
      span: this.span,
      anchor: this.xToSeconds((a.x + b.x) / 2),
    };
  }

  private pointerDown(e: PointerEvent) {
    if (!this.mask) return;
    const pos = this.local(e);
    const p = this.plot();
    if (e.pointerType === 'touch') {
      this.pointers.set(e.pointerId, pos);
      this.canvas.setPointerCapture(e.pointerId);
      if (this.pointers.size === 2) { this.beginPinch(); e.preventDefault(); return; }
    }
    if (this.tool) return;

    if (pos.y >= p.h && pos.x >= p.x) { // time ruler: click seeks, drag sets loop points
      if (e.button !== 0) return;
      this.canvas.setPointerCapture(e.pointerId);
      this.rulerDown = { x: pos.x, dragged: false };
      const loop = this.loop;
      if (loop && Math.abs(pos.x - this.secondsToX(loop.start)) <= EDGE_PX) this.tool = 'loop-start';
      else if (loop && Math.abs(pos.x - this.secondsToX(loop.end)) <= EDGE_PX) this.tool = 'loop-end';
      else this.tool = 'loop-new';
      return;
    }
    if (pos.x < p.x || pos.y >= p.h) return;

    this.canvas.setPointerCapture(e.pointerId);
    this.strokeTravel = 0;
    this.last = pos;
    if (e.button === 1) this.tool = 'scrub';
    else if (e.button === 0 && e.altKey) this.tool = 'pan';
    else if (e.button === 2) this.tool = 'erase';
    else if (e.button === 0) this.tool = 'paint';
    else return;
    e.preventDefault();

    if (this.tool === 'paint' || this.tool === 'erase') {
      this.mask.beginStroke();
      this.stampLine(pos, pos, this.tool === 'erase');
      this.redraw();
    }
  }

  private pointerMove(e: PointerEvent) {
    const pos = this.local(e);
    this.mouse = pos;
    if (this.pointers.has(e.pointerId)) this.pointers.set(e.pointerId, pos);
    this.updateCursor(pos);
    if (this.tool === 'pinch') {
      if (this.pointers.size >= 2) {
        const [a, b] = [...this.pointers.values()];
        const dist = Math.max(1, Math.abs(a.x - b.x));
        const span = Math.min(Math.max(this.duration, MIN_SPAN), Math.max(MIN_SPAN, (this.pinch.span * this.pinch.dist) / dist));
        const frac = ((a.x + b.x) / 2 - RULER_W) / this.plot().w;
        const start = this.pinch.anchor - frac * span;
        this.setWindow(start, start + span);
      }
    } else if (this.tool === 'loop-start' || this.tool === 'loop-end' || this.tool === 'loop-new') {
      if (!this.rulerDown.dragged && Math.abs(pos.x - this.rulerDown.x) > DRAG_PX) this.rulerDown.dragged = true;
      if (this.rulerDown.dragged) this.dragLoop(pos.x);
    } else if (this.tool === 'paint' || this.tool === 'erase') {
      this.strokeTravel += Math.hypot(pos.x - this.last.x, pos.y - this.last.y);
      this.stampLine(this.last, pos, this.tool === 'erase');
    } else if (this.tool === 'pan' || this.tool === 'scrub') {
      const dt = ((this.last.x - pos.x) / this.plot().w) * this.span;
      this.setWindow(this.startSeconds + dt, this.endSeconds + dt);
      if (this.tool === 'pan') {
        const dn = ((pos.y - this.last.y) / this.plot().h) * (this.normHigh - this.normLow);
        this.setNormWindow(this.normLow + dn, this.normHigh + dn);
      }
    }
    this.last = pos;
    this.redraw();
  }

  private dragLoop(x: number) {
    const t = this.clampTime(this.xToSeconds(x));
    const min = MIN_SPAN;
    let { start, end } = this.loop ?? { start: t, end: t };
    if (this.tool === 'loop-start') start = Math.min(t, end - min);
    else if (this.tool === 'loop-end') end = Math.max(t, start + min);
    else {
      const t0 = this.clampTime(this.xToSeconds(this.rulerDown.x));
      start = Math.min(t0, t); end = Math.max(t0, t);
    }
    this.loop = { start, end, enabled: this.loop?.enabled ?? true };
    this.onLoopChange?.({ start, end }, false);
  }

  private updateCursor(pos: { x: number; y: number }) {
    const p = this.plot();
    let cursor = 'none';
    if (pos.x < p.x) cursor = 'default';
    else if (pos.y >= p.h) {
      const l = this.loop;
      const edge = l && (Math.abs(pos.x - this.secondsToX(l.start)) <= EDGE_PX || Math.abs(pos.x - this.secondsToX(l.end)) <= EDGE_PX);
      cursor = this.tool === 'loop-start' || this.tool === 'loop-end' || edge ? 'ew-resize' : 'pointer';
    }
    this.canvas.style.cursor = cursor;
  }

  private pointerUp(e: PointerEvent) {
    this.pointers.delete(e.pointerId);
    if (this.tool === 'pinch') {
      if (this.pointers.size < 2) this.tool = null;
    } else if (this.tool === 'loop-start' || this.tool === 'loop-end' || this.tool === 'loop-new') {
      if (this.rulerDown.dragged) this.onLoopChange?.(this.loop && { start: this.loop.start, end: this.loop.end }, true);
      else this.onSeek?.(this.clampTime(this.xToSeconds(this.rulerDown.x)));
      this.tool = null;
    } else if (this.tool === 'paint' || this.tool === 'erase') {
      this.mask?.endStroke();
      this.onEdit?.();
      this.tool = null;
    } else this.tool = null;
    if (this.canvas.hasPointerCapture(e.pointerId)) this.canvas.releasePointerCapture(e.pointerId);
  }

  private wheel(e: WheelEvent) {
    e.preventDefault();
    const pos = this.local(e);
    const unit = e.deltaMode === 1 ? 16 : e.deltaMode === 2 ? 400 : 1;
    const dx = e.deltaX * unit, dy = e.deltaY * unit;
    // A trackpad pinch arrives as a ctrl+wheel with small deltas, so it needs a stronger factor.
    const zoomFactor = (d: number) => Math.exp(Math.max(-200, Math.min(200, d)) * (e.ctrlKey ? 0.01 : 0.0015));

    if (e.ctrlKey && e.shiftKey) {
      const anchor = this.yToNorm(pos.y);
      const span = Math.min(1, Math.max(MIN_NORM_SPAN, (this.normHigh - this.normLow) * zoomFactor(dy)));
      const frac = (anchor - this.normLow) / (this.normHigh - this.normLow);
      this.setNormWindow(anchor - frac * span, anchor - frac * span + span);
    } else if (e.altKey) {
      this.brushRadius = Math.min(120, Math.max(3, this.brushRadius * (dy < 0 ? 1.1 : 1 / 1.1)));
      this.onBrushChange?.(this.brushRadius);
    } else if (e.shiftKey || (!e.ctrlKey && Math.abs(dx) > Math.abs(dy))) {
      const d = e.shiftKey && dy !== 0 ? dy : dx;
      const dt = (d / 500) * this.span;
      this.setWindow(this.startSeconds + dt, this.endSeconds + dt);
    } else {
      const anchor = this.xToSeconds(pos.x);
      const span = Math.min(Math.max(this.duration, MIN_SPAN), Math.max(MIN_SPAN, this.span * zoomFactor(dy)));
      const frac = (anchor - this.startSeconds) / this.span;
      this.setWindow(anchor - frac * span, anchor - frac * span + span);
    }
    this.mouse = pos;
    this.redraw();
  }

  onBrushChange: ((radius: number) => void) | null = null;

  private setWindow(start: number, end: number) {
    const span = end - start, max = Math.max(this.duration, MIN_SPAN);
    start = Math.max(0, Math.min(start, max - span));
    this.startSeconds = start;
    this.endSeconds = start + span;
    this.specDirty = this.maskDirty = true;
  }

  private setNormWindow(low: number, high: number) {
    const span = high - low;
    low = Math.max(0, Math.min(low, 1 - span));
    this.normLow = low;
    this.normHigh = low + span;
    this.specDirty = this.maskDirty = true;
  }

  /** Keeps the playhead visible while playing. */
  followPlayhead() {
    if (this.playhead === null || this.playhead <= this.endSeconds || this.span >= this.duration) return;
    this.setWindow(this.playhead, this.playhead + this.span);
  }
}

export function renderSpectrogramTo(g: CanvasRenderingContext2D, w: number, h: number, spec: Spectrogram | null, scale: FreqScale, win: TimeFreqWindow) {
  const yToNorm = (y: number) => win.normHigh - (y / h) * (win.normHigh - win.normLow);
  const span = win.end - win.start;
  const img = g.createImageData(w, h);
  const px = img.data;
  if (!spec) { g.putImageData(img, 0, 0); return; }

  // Per-column frame ranges and per-row bin ranges, computed once.
  const framesPerSec = spec.sampleRate / spec.hop;
  const fLo = new Int32Array(w), fHi = new Int32Array(w), fFrac = new Float32Array(w);
  for (let x = 0; x < w; x++) {
    const a = (win.start + (x / w) * span) * framesPerSec;
    const b = (win.start + ((x + 1) / w) * span) * framesPerSec;
    if (b - a <= 1) {
      const c = (a + b) / 2;
      fLo[x] = Math.floor(c); fHi[x] = fLo[x] + 1; fFrac[x] = c - fLo[x];
    } else {
      fLo[x] = Math.floor(a); fHi[x] = Math.max(fLo[x], Math.ceil(b) - 1); fFrac[x] = -1;
    }
  }
  const binsPerHz = spec.size / spec.sampleRate;
  const bLo = new Int32Array(h), bHi = new Int32Array(h), bFrac = new Float32Array(h);
  for (let y = 0; y < h; y++) {
    const a = scale.normToHz(yToNorm(y + 1)) * binsPerHz; // lower edge of the pixel
    const b = scale.normToHz(yToNorm(y)) * binsPerHz;
    if (b - a <= 1) {
      const c = (a + b) / 2;
      bLo[y] = Math.floor(c); bHi[y] = bLo[y] + 1; bFrac[y] = c - bLo[y];
    } else {
      bLo[y] = Math.floor(a); bHi[y] = Math.max(bLo[y], Math.ceil(b) - 1); bFrac[y] = -1;
    }
  }

  const { data, numBins, numFrames } = spec;
  const at = (f: number, b: number) => (f < 0 || f >= numFrames || b < 0 || b >= numBins ? 0 : data[f * numBins + b]);

  for (let x = 0; x < w; x++) {
    const f0 = fLo[x], f1 = fHi[x], ff = fFrac[x];
    for (let y = 0; y < h; y++) {
      const b0 = bLo[y], b1 = bHi[y], bf = bFrac[y];
      let v: number;
      if (ff >= 0 && bf >= 0) {
        const top = at(f0, b0) + bf * (at(f0, b1) - at(f0, b0));
        const bot = at(f1, b0) + bf * (at(f1, b1) - at(f1, b0));
        v = top + ff * (bot - top);
      } else {
        // At least one axis covers several cells: take the loudest, so narrow partials survive zooming out.
        const fa = ff >= 0 ? (ff < 0.5 ? f0 : f1) : f0, fb = ff >= 0 ? fa : f1;
        const ba = bf >= 0 ? (bf < 0.5 ? b0 : b1) : b0, bb = bf >= 0 ? ba : b1;
        v = 0;
        for (let f = fa; f <= fb; f++) for (let b = ba; b <= bb; b++) v = Math.max(v, at(f, b));
      }
      const o = (y * w + x) * 4, li = (v < 0 ? 0 : v > 255 ? 255 : Math.round(v)) * 4;
      px[o] = COLOR_LUT[li]; px[o + 1] = COLOR_LUT[li + 1]; px[o + 2] = COLOR_LUT[li + 2]; px[o + 3] = 255;
    }
  }
  g.putImageData(img, 0, 0);
}

export function renderMaskTo(g: CanvasRenderingContext2D, w: number, h: number, mask: MaskGrid | null, scale: FreqScale, win: TimeFreqWindow, invert: boolean) {
  const yToNorm = (y: number) => win.normHigh - (y / h) * (win.normHigh - win.normLow);
  const span = win.end - win.start;
  const img = g.createImageData(w, h);
  if (mask) {
    const rows = new Float32Array(h);
    for (let y = 0; y < h; y++) rows[y] = hzToRow(scale.normToHz(yToNorm(y + 0.5)));

    // Dim what is filtered out, tint what was painted.
    const lut = new Uint8ClampedArray(256 * 4);
    for (let i = 0; i < 256; i++) {
      const m = i / 255, audible = invert ? 1 - m : m;
      const dim = 0.4 * (1 - audible), tint = 0.45 * m;
      const alpha = dim + tint - dim * tint;
      const k = alpha > 1e-6 ? tint / alpha : 0;
      for (let c = 0; c < 3; c++) lut[i * 4 + c] = colors.paint[c] * k;
      lut[i * 4 + 3] = alpha * 255;
    }
    for (let x = 0; x < w; x++) {
      const t = win.start + ((x + 0.5) / w) * span;
      for (let y = 0; y < h; y++) {
        const level = Math.round(mask.sample(t, rows[y]) * 255) * 4;
        const o = (y * w + x) * 4;
        img.data[o] = lut[level]; img.data[o + 1] = lut[level + 1]; img.data[o + 2] = lut[level + 2]; img.data[o + 3] = lut[level + 3];
      }
    }
  }
  g.putImageData(img, 0, 0);
}

function formatTime(t: number, step: number): string {
  const m = Math.floor(t / 60), s = t - m * 60;
  const dec = step >= 1 ? 0 : step >= 0.1 ? 1 : 2;
  const secs = s.toFixed(dec).padStart(dec ? dec + 3 : 2, '0');
  return m > 0 ? `${m}:${secs}` : `${s.toFixed(dec)}s`;
}
