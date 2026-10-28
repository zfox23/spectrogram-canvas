import './style.css';
import { decodeAudioFile } from './audio/decode';
import { Player } from './audio/player';
import { DspWorker } from './dsp/workerClient';
import { DEFAULT_ORDER } from './dsp/analysis';
import { MASK_ROWS, MaskGrid } from './model/maskGrid';
import { SpectrogramView } from './ui/view';
import { OverviewView } from './ui/overview';
import { base64ToBytes, bytesToBase64, embedState, extractAudio, extractState, sha256Hex } from './io/statePng';
import type { FreqScaleType } from './model/freqScale';

const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;
const els = {
  toolbar: $('toolbar'), file: $<HTMLInputElement>('file'), play: $<HTMLButtonElement>('play'),
  listen: $<HTMLSelectElement>('listen'), time: $('time'), undo: $<HTMLButtonElement>('undo'),
  redo: $<HTMLButtonElement>('redo'), clear: $<HTMLButtonElement>('clear'), all: $<HTMLButtonElement>('all'),
  fit: $<HTMLButtonElement>('fit'), invert: $<HTMLInputElement>('invert'), bg: $<HTMLInputElement>('bg'),
  stop: $<HTMLButtonElement>('stop'), loopOn: $<HTMLInputElement>('loopOn'), clearLoop: $<HTMLButtonElement>('clearLoop'),
  trim: $<HTMLButtonElement>('trim'),
  exportPng: $<HTMLButtonElement>('exportPng'), pngAudio: $<HTMLInputElement>('pngAudio'), importPng: $<HTMLInputElement>('importPng'),
  overview: $('overview'), bgOut: $('bgOut'), brush: $<HTMLInputElement>('brush'), hardness: $<HTMLInputElement>('hardness'),
  fft: $<HTMLSelectElement>('fft'), scale: $<HTMLSelectElement>('scale'), format: $<HTMLSelectElement>('format'),
  save: $<HTMLButtonElement>('save'), stage: $('stage'), plot: $('plot'), empty: $('empty'),
  busy: $('busy'), busyText: $('busyText'), busyBar: $('busyBar'), status: $('status'),
};

interface Loaded {
  file: File;
  name: string;
  hash: string;
  channels: Float32Array[];
  sampleRate: number;
  duration: number;
  mask: MaskGrid;
}

const worker = new DspWorker();
const player = new Player();
const view = new SpectrogramView(els.plot);
const overview = new OverviewView(els.overview, view);
view.onDraw = () => overview.redraw();
let loaded = null as Loaded | null;
let busy = false;
let analysisToken = 0;
let filtered: { key: string; channels: Float32Array[] } | null = null;
let playerShows = 'original';

const fftOrder = () => Number(els.fft.value) || DEFAULT_ORDER;
const bgDb = () => Number(els.bg.value);
const bgGain = () => (bgDb() <= -144 ? 0 : Math.pow(10, bgDb() / 20));
const filterKey = () => `${loaded!.mask.version}|${fftOrder()}|${bgDb()}|${els.invert.checked}`;
const isFresh = () => !!loaded && filtered?.key === filterKey();

function setStatus(text: string, isError = false) {
  els.status.textContent = text;
  els.status.classList.toggle('error', isError);
}

async function withBusy<T>(label: string, task: (progress: (f: number) => void) => Promise<T>): Promise<T> {
  busy = true;
  els.busy.hidden = false;
  els.busyText.textContent = label;
  els.busyBar.style.width = '0%';
  try {
    return await task((f) => { els.busyBar.style.width = `${Math.round(f * 100)}%`; });
  } finally {
    busy = false;
    els.busy.hidden = true;
  }
}

//==============================================================================
async function analyseLoaded() {
  if (!loaded) return;
  const token = ++analysisToken;
  const { channels, sampleRate } = loaded;
  const spec = await withBusy('Analysing spectrogram…', (p) => worker.analyse(channels, sampleRate, fftOrder(), p));
  if (token === analysisToken && loaded?.channels === channels) view.setSpectrogram(spec);
}

async function openFile(file: File): Promise<boolean> {
  try {
    player.stop();
    const [audio, hash] = await withBusy('Decoding…', () => Promise.all([decodeAudioFile(file), file.arrayBuffer().then(sha256Hex)]));
    startMarker = 0;
    loopRange = null;
    useAudio({ ...audio, file, name: file.name, hash, mask: new MaskGrid(audio.duration) });
    els.empty.classList.add('hidden');
    els.toolbar.setAttribute('aria-disabled', 'false');
    setStatus(`${file.name} — ${audio.channels.length === 1 ? 'mono' : audio.channels.length + ' ch'}, ${audio.sampleRate} Hz, ${fmt(audio.duration)}`);
    await analyseLoaded();
    return true;
  } catch (err) {
    setStatus(err instanceof Error ? err.message : String(err), true);
    return false;
  }
}

/** Makes l the working audio; startMarker and loopRange must already be set for it. */
function useAudio(l: Loaded) {
  loaded = l;
  filtered = null;
  playerShows = 'original';
  applyLoop();
  view.setAudio(l.duration, l.sampleRate, l.mask);
  player.setAudio(l.channels, l.sampleRate);
  player.seek(startMarker);
  view.playhead = startMarker;
  refreshButtons();
}

/** Cuts the audio and the paint down to the loop range, so there is less to look at and exports are smaller. */
async function trimToLoop() {
  if (!loaded || busy || !loopRange) return;
  try {
    player.stop();
    const l = loaded, sr = l.sampleRate;
    const a = Math.round(loopRange.start * sr), b = Math.round(loopRange.end * sr);
    const channels = l.channels.map((ch) => ch.slice(a, b));
    const start = a / sr, duration = (b - a) / sr;
    // The trimmed audio becomes the session's file: it is what PNG exports embed and hash.
    const bytes = await withBusy('Trimming…', (p) => worker.flac(channels, sr, p));
    const name = `${l.name.replace(/\.[^.]+$/, '')}-trim.flac`;
    const file = new File([bytes as Uint8Array<ArrayBuffer>], name, { type: 'audio/flac' });
    const hash = await sha256Hex(await file.arrayBuffer());
    if (loaded !== l) return;
    startMarker = Math.min(Math.max(startMarker - start, 0), duration);
    loopRange = { start: 0, end: duration };
    useAudio({ file, name, hash, channels, sampleRate: sr, duration, mask: l.mask.cropped(start, duration) });
    setStatus(`Trimmed to ${fmt(duration)} (${fmt(start)}–${fmt(start + duration)} of ${l.name}). Reopen the original file to get the rest back.`);
    onMaskEdited();
    await analyseLoaded();
  } catch (err) {
    setStatus(`Trim failed: ${err instanceof Error ? err.message : err}`, true);
  }
}

function runFilter(p?: (f: number) => void) {
  const l = loaded!;
  return worker.filter(l.channels, l.sampleRate, fftOrder(), l.mask.isEmpty() ? null : { cols: l.mask.cols, colSeconds: l.mask.colSeconds, data: l.mask.data },
    { backgroundGain: bgGain(), invert: els.invert.checked }, p);
}

async function ensureFiltered(): Promise<Float32Array[]> {
  if (!loaded) throw new Error('No audio loaded');
  if (isFresh()) return filtered!.channels;
  const key = filterKey();
  const l = loaded;
  const channels = await withBusy('Applying filter…', (p) => runFilter(p));
  if (loaded === l) filtered = { key, channels };
  return channels;
}

// Live update: re-filter quietly in the background whenever the mask or settings change.
let liveTimer: number | undefined;
let liveRunning = false;

function scheduleLive() {
  clearTimeout(liveTimer);
  liveTimer = window.setTimeout(() => void liveRefilter(), 120);
}

async function liveRefilter() {
  if (liveRunning) { scheduleLive(); return; }
  if (!loaded || busy || els.listen.value !== 'filtered' || isFresh()) return;
  liveRunning = true;
  const l = loaded, key = filterKey();
  try {
    const channels = await runFilter();
    if (loaded !== l) return;
    filtered = { key, channels };
    if (key === filterKey() && els.listen.value === 'filtered' && (player.playing || playerShows !== 'original')) {
      player.setAudio(channels, l.sampleRate);
      playerShows = key;
    }
  } catch (err) {
    setStatus(`Filter failed: ${err instanceof Error ? err.message : err}`);
  } finally {
    liveRunning = false;
    if (loaded === l && !isFresh() && els.listen.value === 'filtered') scheduleLive();
  }
}

async function loadIntoPlayer() {
  if (!loaded) return;
  if (els.listen.value === 'original') {
    player.setAudio(loaded.channels, loaded.sampleRate);
  } else {
    player.setAudio(await ensureFiltered(), loaded.sampleRate);
  }
  playerShows = els.listen.value === 'original' ? 'original' : filtered!.key;
}

async function togglePlay() {
  if (!loaded || busy) return;
  if (player.playing) { player.stop(); refreshButtons(); return; }
  const wanted = els.listen.value === 'original' ? 'original' : isFresh() ? filtered!.key : '';
  if (wanted !== playerShows || wanted === '') await loadIntoPlayer();
  player.play();
  refreshButtons();
}

async function save() {
  if (!loaded || busy) return;
  try {
    const out = await ensureFiltered();
    const [kind, opt] = els.format.value.split('-');
    const base = loaded.name.replace(/\.[^.]+$/, '');
    let bytes: Uint8Array, ext: string, mime: string;
    if (kind.startsWith('wav')) {
      const format = kind === 'wav16' ? 'pcm16' : kind === 'wav24' ? 'pcm24' : 'float32';
      bytes = await withBusy('Encoding WAV…', () => worker.wav(out, loaded!.sampleRate, format));
      ext = 'wav'; mime = 'audio/wav';
    } else {
      bytes = await withBusy('Encoding MP3…', (p) => worker.mp3(out, loaded!.sampleRate, Number(opt), p));
      ext = 'mp3'; mime = 'audio/mpeg';
    }
    const url = URL.createObjectURL(new Blob([bytes as Uint8Array<ArrayBuffer>], { type: mime }));
    const a = document.createElement('a');
    a.href = url;
    a.download = `${base}-filtered.${ext}`;
    a.click();
    setTimeout(() => URL.revokeObjectURL(url), 10_000);
    setStatus(`Saved ${a.download} (${(bytes.length / 1048576).toFixed(1)} MB)`);
  } catch (err) {
    setStatus(`Save failed: ${err instanceof Error ? err.message : err}`);
  }
}

//==============================================================================
function fmt(t: number) {
  const m = Math.floor(t / 60);
  return `${m}:${(t - m * 60).toFixed(1).padStart(4, '0')}`;
}

function refreshButtons() {
  els.play.textContent = player.playing ? '⏸ Pause' : '▶ Play';
  els.undo.disabled = !loaded?.mask.canUndo();
  els.redo.disabled = !loaded?.mask.canRedo();
}

function onMaskEdited() {
  refreshButtons();
  if (loaded && !isFresh() && els.listen.value === 'filtered') scheduleLive();
}

function undo() { if (loaded?.mask.canUndo()) { loaded.mask.undo(); view.maskChanged(); onMaskEdited(); } }
function redo() { if (loaded?.mask.canRedo()) { loaded.mask.redo(); view.maskChanged(); onMaskEdited(); } }

view.onEdit = onMaskEdited;
view.onBrushChange = (r) => { els.brush.value = String(Math.round(r)); };
let startMarker = 0;
let loopRange: { start: number; end: number } | null = null;

function applyLoop() {
  view.loop = loopRange && { ...loopRange, enabled: els.loopOn.checked };
  view.startMarker = startMarker;
  player.setLoop(loopRange && els.loopOn.checked ? loopRange : null);
  els.clearLoop.disabled = !loopRange;
  els.trim.disabled = !loopRange || !loaded || (loopRange.start <= 0 && loopRange.end >= loaded.duration);
  view.redraw();
}

view.onSeek = (s) => { startMarker = s; view.startMarker = s; player.seek(s); view.playhead = s; view.redraw(); };
view.onLoopChange = (r, final) => {
  if (!final) return;
  loopRange = r;
  if (r) els.loopOn.checked = true;
  applyLoop();
};
els.loopOn.addEventListener('change', () => {
  applyLoop();
  if (els.loopOn.checked && !loopRange) setStatus('Drag along the time ruler to set the loop points.');
});
els.clearLoop.addEventListener('click', () => { loopRange = null; applyLoop(); });
els.trim.addEventListener('click', () => void trimToLoop());
els.stop.addEventListener('click', () => stopToStart());

function stopToStart() {
  player.stop();
  player.seek(startMarker);
  view.playhead = startMarker;
  refreshButtons();
  view.redraw();
}

els.file.addEventListener('change', () => { const f = els.file.files?.[0]; if (f) void handleDrop([f]); els.file.value = ''; });
els.play.addEventListener('click', () => void togglePlay());
els.listen.addEventListener('change', () => { if (player.playing) { player.stop(); void togglePlay(); } });
els.undo.addEventListener('click', undo);
els.redo.addEventListener('click', redo);
els.clear.addEventListener('click', () => { loaded?.mask.clear(); view.maskChanged(); onMaskEdited(); });
els.all.addEventListener('click', () => { loaded?.mask.fill(); view.maskChanged(); onMaskEdited(); });
els.fit.addEventListener('click', () => view.fit());
els.invert.addEventListener('change', () => { view.setInvert(els.invert.checked); onMaskEdited(); });
function syncBgLabel() {
  els.bgOut.textContent = bgDb() <= -144 ? '−∞ dB' : `${bgDb() > 0 ? '+' : ''}${bgDb()} dB`;
}
els.bg.addEventListener('input', () => { syncBgLabel(); onMaskEdited(); });
els.brush.addEventListener('input', () => { view.brushRadius = Number(els.brush.value); });
els.hardness.addEventListener('input', () => { view.hardness = Number(els.hardness.value) / 100; });
els.fft.addEventListener('change', () => { void analyseLoaded(); onMaskEdited(); });
els.scale.addEventListener('change', () => view.setScaleType(els.scale.value as FreqScaleType));
els.format.addEventListener('change', () => undefined);
els.save.addEventListener('click', () => void save());

window.addEventListener('keydown', (e) => {
  const t = e.target as HTMLElement;
  if (t instanceof HTMLSelectElement || (t instanceof HTMLInputElement && t.type !== 'range' && t.type !== 'checkbox')) return;
  if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'z') { e.preventDefault(); e.shiftKey ? redo() : undo(); }
  else if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'y') { e.preventDefault(); redo(); }
  else if (e.key === 'Enter' && !(t instanceof HTMLButtonElement)) { e.preventDefault(); stopToStart(); }
  else if (e.key === ' ' && !(t instanceof HTMLButtonElement)) { e.preventDefault(); void togglePlay(); }
});

['dragenter', 'dragover'].forEach((n) => els.stage.addEventListener(n, (e) => { e.preventDefault(); els.stage.classList.add('dragging'); }));
['dragleave', 'drop'].forEach((n) => els.stage.addEventListener(n, (e) => { e.preventDefault(); els.stage.classList.remove('dragging'); }));
els.stage.addEventListener('drop', (e) => void handleDrop([...((e as DragEvent).dataTransfer?.files ?? [])]));

//==============================================================================
// State export / import: a PNG picture of the painted spectrogram carrying the app state as JSON.
const STATE_VERSION = 1;
const isPng = (f: File) => f.type === 'image/png' || /\.png$/i.test(f.name);

const APP_ID = 'spectrogram-canvas';
const LEGACY_APP_IDS = ['spectrogram-draw-filter']; // pre-rename exports

interface AppState {
  app: string;
  version: number;
  file: { name: string; sha256: string; duration: number; sampleRate: number; channels: number };
  settings: { fftOrder: number; scale: string; invert: boolean; backgroundDb: number; brush: number; hardness: number; listen: string; format: string };
  playback: { playhead: number; startMarker: number; loopEnabled: boolean; loop: { start: number; end: number } | null };
  view: { start: number; end: number; normLow: number; normHigh: number };
  mask: { cols: number; rows: number; encoding: 'base64'; data: string };
}

function captureState(): AppState {
  const l = loaded!;
  return {
    app: APP_ID,
    version: STATE_VERSION,
    file: { name: l.name, sha256: l.hash, duration: l.duration, sampleRate: l.sampleRate, channels: l.channels.length },
    settings: {
      fftOrder: fftOrder(), scale: els.scale.value, invert: els.invert.checked, backgroundDb: bgDb(),
      brush: Number(els.brush.value), hardness: Number(els.hardness.value), listen: els.listen.value, format: els.format.value,
    },
    playback: { playhead: player.position, startMarker, loopEnabled: els.loopOn.checked, loop: loopRange },
    view: view.getViewWindow(),
    mask: { cols: l.mask.cols, rows: MASK_ROWS, encoding: 'base64', data: bytesToBase64(l.mask.data) },
  };
}

/** Local time as YYYY-MM-DD HH-mm. */
function formatStamp(d: Date) {
  const p = (n: number) => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}-${p(d.getMinutes())}`;
}

async function exportPng() {
  if (!loaded || busy) return;
  try {
    const state = captureState();
    const withAudio = els.pngAudio.checked;
    const appUrl = location.origin + location.pathname;
    const canvas = view.renderShareCard({
      title: 'Spectrogram Canvas',
      date: formatStamp(new Date()),
      fileName: loaded.name,
      hint: withAudio ? `Open this PNG at ${appUrl}` : `Open at ${appUrl} together with ${loaded.name}`,
      audioEmbedded: withAudio,
    });
    const blob = await new Promise<Blob | null>((r) => canvas.toBlob(r, 'image/png'));
    if (!blob) throw new Error('Could not render the image.');
    const audio = withAudio ? new Uint8Array(await loaded.file.arrayBuffer()) : undefined;
    const png = await embedState(new Uint8Array(await blob.arrayBuffer()), state, audio);
    const url = URL.createObjectURL(new Blob([png as Uint8Array<ArrayBuffer>], { type: 'image/png' }));
    const a = document.createElement('a');
    a.href = url;
    a.download = `${loaded.name.replace(/\.[^.]+$/, '')}-spectrogram-canvas.png`;
    a.click();
    setTimeout(() => URL.revokeObjectURL(url), 10_000);
    setStatus(`Exported ${a.download} (${(png.length / 1048576).toFixed(1)} MB)`);
  } catch (err) {
    setStatus(`Export failed: ${err instanceof Error ? err.message : err}`, true);
  }
}

const num = (v: unknown, fallback: number) => (typeof v === 'number' && Number.isFinite(v) ? v : fallback);
const pick = (el: HTMLSelectElement, v: unknown) => { if (typeof v === 'string' && [...el.options].some((o) => o.value === v)) el.value = v; };

async function importPngFile(file: File): Promise<boolean> {
  try {
    const bytes = new Uint8Array(await file.arrayBuffer());
    const s = (await extractState(bytes)) as Partial<AppState> | null;
    if (!s || typeof s.app !== 'string' || ![APP_ID, ...LEGACY_APP_IDS].includes(s.app)) throw new Error(`${file.name} has no Spectrogram Canvas state in it.`);
    if (num(s.version, 0) > STATE_VERSION) throw new Error(`${file.name} was made by a newer version of this app.`);
    if (!s.file || typeof s.file.sha256 !== 'string') throw new Error(`${file.name} is missing its audio file hash.`);

    // A PNG exported "with audio" carries the original file; use it unless that file is already loaded.
    const embedded = extractAudio(bytes);
    if (embedded && loaded?.hash !== s.file.sha256) {
      const name = typeof s.file.name === 'string' && s.file.name ? s.file.name : 'audio';
      if (!(await openFile(new File([embedded as Uint8Array<ArrayBuffer>], name)))) return false;
    }
    if (!loaded) {
      throw new Error(`${file.name} has no audio in it (it may have been stripped by the site it was shared on). Open the matching audio file first, then import the PNG.`);
    }
    if (s.file.sha256 !== loaded.hash) {
      throw new Error(`File hash mismatch: ${file.name} was made from “${s.file.name}” (${s.file.sha256.slice(0, 12)}…), but the loaded file “${loaded.name}” is ${loaded.hash.slice(0, 12)}….`);
    }
    const m = s.mask;
    if (!m || m.encoding !== 'base64' || typeof m.data !== 'string' || m.cols !== loaded.mask.cols || m.rows !== MASK_ROWS) throw new Error('The painted mask in the PNG does not fit this audio file.');
    const maskData = base64ToBytes(m.data);
    if (maskData.length !== loaded.mask.data.length) throw new Error('The painted mask in the PNG is damaged.');

    // Everything checked: apply.
    const st = s.settings ?? ({} as Partial<AppState['settings']>);
    const pb = s.playback ?? ({} as Partial<AppState['playback']>);
    const dur = loaded.duration;
    const clampT = (t: number) => Math.min(Math.max(t, 0), dur);
    const order = num(st.fftOrder, fftOrder());
    pick(els.scale, st.scale); view.setScaleType(els.scale.value as FreqScaleType);
    els.invert.checked = !!st.invert; view.setInvert(els.invert.checked);
    els.bg.value = String(num(st.backgroundDb, bgDb())); syncBgLabel();
    els.brush.value = String(num(st.brush, Number(els.brush.value))); view.brushRadius = Number(els.brush.value);
    els.hardness.value = String(num(st.hardness, Number(els.hardness.value))); view.hardness = Number(els.hardness.value) / 100;
    pick(els.listen, st.listen); pick(els.format, st.format);
    loaded.mask.replaceAll(maskData);
    view.maskChanged();

    const lp = pb.loop;
    loopRange = lp && num(lp.end, 0) - num(lp.start, 0) > 0 ? { start: clampT(lp.start), end: clampT(lp.end) } : null;
    els.loopOn.checked = !!pb.loopEnabled;
    startMarker = clampT(num(pb.startMarker, 0));
    applyLoop();
    player.seek(clampT(num(pb.playhead, startMarker)));
    view.playhead = player.position;
    const w = s.view;
    if (w) view.setViewWindow({ start: num(w.start, 0), end: num(w.end, dur), normLow: num(w.normLow, 0), normHigh: num(w.normHigh, 1) });
    refreshButtons();
    onMaskEdited();
    setStatus(`Imported state from ${file.name}`);

    if (String(order) !== els.fft.value && [...els.fft.options].some((o) => o.value === String(order))) {
      els.fft.value = String(order);
      await analyseLoaded();
      onMaskEdited();
    }
    return true;
  } catch (err) {
    setStatus(err instanceof Error ? err.message : String(err), true);
    return false;
  }
}

/** Audio first (so the hash is known), then the PNG. */
async function handleDrop(files: File[]) {
  if (busy) return;
  const png = files.find(isPng);
  const audio = files.find((f) => !isPng(f));
  if (audio && !(await openFile(audio))) return;
  if (png) await importPngFile(png);
}

els.exportPng.addEventListener('click', () => void exportPng());
els.importPng.addEventListener('change', () => { const f = els.importPng.files?.[0]; if (f) void importPngFile(f); els.importPng.value = ''; });

player.onEnded = () => { player.seek(startMarker); refreshButtons(); };
(function tick() {
  if (loaded) {
    const pos = player.position;
    els.time.textContent = `${fmt(pos)} / ${fmt(loaded.duration)}`;
    if (view.playhead !== pos) {
      view.playhead = pos;
      if (player.playing) view.followPlayhead();
      view.redraw();
    }
  }
  requestAnimationFrame(tick);
})();
refreshButtons();
