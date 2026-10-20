import { RULER_W, colors, renderMaskTo, renderSpectrogramTo, type SpectrogramView } from './view';

const MIN_WINDOW = 0.02;
const EDGE_PX = 5;
const THROTTLE_MS = 250;

type Drag = 'move' | 'left' | 'right' | null;

/** A strip under the main view showing the whole file, the visible window, the loop and the playhead. */
export class OverviewView {
  readonly canvas = document.createElement('canvas');
  private ctx = this.canvas.getContext('2d')!;
  private specLayer = document.createElement('canvas');
  private maskLayer = document.createElement('canvas');
  private cssW = 0;
  private cssH = 0;
  private dpr = 1;
  private drawQueued = false;
  private timer = 0;
  private lastRender = 0;
  private rendered = { key: '', spec: null as unknown, mask: null as unknown, version: -1 };
  private drag: Drag = null;
  private dragStart = { x: 0, start: 0, end: 0 };

  constructor(host: HTMLElement, private view: SpectrogramView) {
    host.appendChild(this.canvas);
    this.canvas.style.touchAction = 'none';
    new ResizeObserver(() => this.resize()).observe(host);
    const c = this.canvas;
    c.addEventListener('pointerdown', (e) => this.pointerDown(e));
    c.addEventListener('pointermove', (e) => this.pointerMove(e));
    c.addEventListener('pointerup', () => { this.drag = null; });
    c.addEventListener('pointercancel', () => { this.drag = null; });
  }

  redraw() {
    if (this.drawQueued) return;
    this.drawQueued = true;
    requestAnimationFrame(() => { this.drawQueued = false; this.draw(); });
  }

  //==========================================================================
  private plot() { return { x: RULER_W, y: 2, w: Math.max(1, this.cssW - RULER_W), h: Math.max(1, this.cssH - 4) }; }
  private get total() { return Math.max(this.view.duration, MIN_WINDOW); }
  private xToSeconds = (x: number) => ((x - RULER_W) / this.plot().w) * this.total;
  private secondsToX = (s: number) => RULER_W + (s / this.total) * this.plot().w;

  private windowRect() {
    const p = this.plot();
    const x0 = this.secondsToX(this.view.startSeconds), x1 = this.secondsToX(this.view.endSeconds);
    return { x: x0, y: p.y, w: Math.max(2, x1 - x0), h: p.h };
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
    this.rendered.key = '';
    this.redraw();
  }

  private ensureLayers() {
    const v = this.view, p = this.plot();
    const key = `${v.scale.type}|${v.scale.maxHz}|${v.invert}|${v.duration}|${p.w}x${p.h}`;
    const forced = key !== this.rendered.key || v.spec !== this.rendered.spec || v.mask !== this.rendered.mask;
    const maskStale = (v.mask?.version ?? -1) !== this.rendered.version;
    if (!forced && !maskStale) return;

    const wait = THROTTLE_MS - (performance.now() - this.lastRender);
    if (!forced && wait > 0) { // painting: refresh the overview at a gentler rate than the main view
      clearTimeout(this.timer);
      this.timer = window.setTimeout(() => this.redraw(), wait);
      return;
    }
    const win = { start: 0, end: this.total, normLow: 0, normHigh: 1 };
    renderSpectrogramTo(this.specLayer.getContext('2d')!, p.w, p.h, v.spec, v.scale, win);
    renderMaskTo(this.maskLayer.getContext('2d')!, p.w, p.h, v.mask, v.scale, win, v.invert);
    this.rendered = { key, spec: v.spec, mask: v.mask, version: v.mask?.version ?? -1 };
    this.lastRender = performance.now();
  }

  private draw() {
    if (this.cssW <= 0 || this.cssH <= 0) return;
    this.ensureLayers();
    const g = this.ctx, v = this.view, p = this.plot();
    g.setTransform(this.dpr, 0, 0, this.dpr, 0, 0);
    g.fillStyle = colors.bg;
    g.fillRect(0, 0, this.cssW, this.cssH);
    g.imageSmoothingEnabled = false;
    g.drawImage(this.specLayer, p.x, p.y);
    g.drawImage(this.maskLayer, p.x, p.y);

    if (v.loop) {
      const x0 = this.secondsToX(v.loop.start), x1 = this.secondsToX(v.loop.end);
      const rgb = v.loop.enabled ? '76,201,240' : '154,160,173';
      g.fillStyle = `rgba(${rgb},${v.loop.enabled ? 0.22 : 0.12})`;
      g.fillRect(x0, p.y, Math.max(1, x1 - x0), p.h);
      g.fillStyle = `rgba(${rgb},0.9)`;
      g.fillRect(x0 - 0.5, p.y, 1.5, p.h);
      g.fillRect(x1 - 0.5, p.y, 1.5, p.h);
    }

    // Dim everything outside the main view's window.
    const win = this.windowRect();
    g.fillStyle = 'rgba(0,0,0,0.5)';
    g.fillRect(p.x, win.y, win.x - p.x, win.h);
    g.fillRect(win.x + win.w, win.y, p.x + p.w - (win.x + win.w), win.h);
    g.strokeStyle = colors.text;
    g.lineWidth = 1.5;
    g.strokeRect(win.x, win.y, win.w, win.h);

    const line = (t: number | null, color: string) => {
      if (t === null) return;
      const x = Math.round(this.secondsToX(t)) + 0.5;
      g.strokeStyle = color;
      g.lineWidth = 1;
      g.beginPath(); g.moveTo(x, p.y); g.lineTo(x, p.y + p.h); g.stroke();
    };
    line(v.startMarker, colors.marker);
    line(v.playhead, 'rgba(255,255,255,0.85)');

    g.fillStyle = colors.panel;
    g.fillRect(0, 0, RULER_W, this.cssH);
    g.fillStyle = colors.text;
    g.font = '11px system-ui, sans-serif';
    g.textAlign = 'right';
    g.textBaseline = 'middle';
    g.fillText('Overview', RULER_W - 7, this.cssH / 2);
  }

  //==========================================================================
  private local(e: PointerEvent) {
    const r = this.canvas.getBoundingClientRect();
    return { x: e.clientX - r.left, y: e.clientY - r.top };
  }

  private pointerDown(e: PointerEvent) {
    if (e.button !== 0 || this.view.duration <= 0) return;
    const { x } = this.local(e);
    if (x < RULER_W) return;
    this.canvas.setPointerCapture(e.pointerId);
    const win = this.windowRect();
    if (Math.abs(x - win.x) < EDGE_PX) this.drag = 'left';
    else if (Math.abs(x - (win.x + win.w)) < EDGE_PX) this.drag = 'right';
    else if (x > win.x && x < win.x + win.w) this.drag = 'move';
    else { // jump: centre the window on the click, then keep dragging it
      const span = this.view.endSeconds - this.view.startSeconds, c = this.xToSeconds(x);
      this.view.setTimeRange(c - span / 2, c + span / 2);
      this.drag = 'move';
    }
    this.dragStart = { x, start: this.view.startSeconds, end: this.view.endSeconds };
  }

  private pointerMove(e: PointerEvent) {
    const { x } = this.local(e);
    if (!this.drag) {
      const win = this.windowRect();
      const edge = Math.abs(x - win.x) < EDGE_PX || Math.abs(x - (win.x + win.w)) < EDGE_PX;
      this.canvas.style.cursor = x < RULER_W ? 'default' : edge ? 'ew-resize' : x > win.x && x < win.x + win.w ? 'grab' : 'pointer';
      return;
    }
    const dt = this.xToSeconds(x) - this.xToSeconds(this.dragStart.x);
    const { start, end } = this.dragStart;
    if (this.drag === 'move') this.view.setTimeRange(start + dt, end + dt);
    else if (this.drag === 'left') this.view.setTimeRange(Math.max(0, Math.min(start + dt, end - MIN_WINDOW)), end);
    else this.view.setTimeRange(start, Math.min(this.total, Math.max(end + dt, start + MIN_WINDOW)));
  }
}
