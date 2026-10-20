import { describe, expect, it } from 'vitest';
import { FFT } from '../src/dsp/fft';
import { analyse } from '../src/dsp/analysis';
import { applyMask } from '../src/dsp/filter';
import { MASK_ROWS, MaskGrid, hzToRow, rowToHz, type ScreenMapping } from '../src/model/maskGrid';
import { encodeWav } from '../src/encode/wav';
import { encodeMp3 } from '../src/encode/mp3';

const SR = 44100;
const sine = (hz: number, seconds: number, amp = 0.5) =>
  Float32Array.from({ length: Math.floor(SR * seconds) }, (_, i) => amp * Math.sin((2 * Math.PI * hz * i) / SR));
const rms = (x: Float32Array, a = 0, b = x.length) => {
  let s = 0;
  for (let i = a; i < b; i++) s += x[i] * x[i];
  return Math.sqrt(s / (b - a));
};
// Mapping where 1 px = 1 column-ish and the y axis is log-Hz over 400 px.
const mapping = (m: MaskGrid): ScreenMapping => ({
  secondsToX: (s) => (s / m.durationSeconds) * 1000,
  xToSeconds: (x) => (x / 1000) * m.durationSeconds,
  hzToY: (hz) => 400 - hzToRow(hz) / (MASK_ROWS - 1) * 400,
  yToHz: (y) => rowToHz(((400 - y) / 400) * (MASK_ROWS - 1)),
});

describe('FFT', () => {
  it('round-trips and finds a pure tone', () => {
    const n = 256, fft = new FFT(n);
    const re = Float32Array.from({ length: n }, (_, i) => Math.cos((2 * Math.PI * 8 * i) / n));
    const im = new Float32Array(n), orig = re.slice();
    fft.transform(re, im);
    expect(re[8]).toBeCloseTo(n / 2, 3);
    fft.transform(re, im, true);
    for (let i = 0; i < n; i++) expect(re[i]).toBeCloseTo(orig[i], 4);
  });
});

describe('analyse', () => {
  it('reads a full-scale sine as about 0 dB at the right bin', () => {
    const spec = analyse(sine(1000, 1, 1), SR, 12);
    const f = Math.floor(spec.numFrames / 2);
    let peak = 0, peakBin = 0;
    for (let k = 0; k < spec.numBins; k++) if (spec.data[f * spec.numBins + k] > peak) { peak = spec.data[f * spec.numBins + k]; peakBin = k; }
    expect(peakBin).toBe(Math.round((1000 * spec.size) / SR));
    expect(peak).toBeGreaterThan(245);
  });
});

describe('applyMask', () => {
  const x = sine(440, 1), y = sine(2000, 1);
  const mix = x.map((v, i) => v + y[i]);

  it('reconstructs the input when everything is painted (stereo and mono)', () => {
    const m = new MaskGrid(1);
    m.fill();
    const out = applyMask([mix, y], SR, 12, m, { backgroundGain: 0, invert: false });
    for (const i of [0, 1, 1000, 22050, 44099]) {
      expect(out[0][i]).toBeCloseTo(mix[i], 4);
      expect(out[1][i]).toBeCloseTo(y[i], 4);
    }
    const mono = applyMask([mix], SR, 12, m, { backgroundGain: 0, invert: false });
    expect(mono[0][30000]).toBeCloseTo(mix[30000], 4);
  });

  it('keeps only the painted band, independently per channel', () => {
    const m = new MaskGrid(1);
    const lo = Math.floor(hzToRow(300)), hi = Math.ceil(hzToRow(700));
    for (let c = 0; c < m.cols; c++) for (let r = lo; r <= hi; r++) m.data[c * MASK_ROWS + r] = 255;
    const out = applyMask([mix, mix.map((_, i) => y[i])], SR, 12, m, { backgroundGain: 0, invert: false });
    expect(rms(out[0], 8000, 36000)).toBeCloseTo(rms(x, 8000, 36000), 1); // 440 Hz survives
    expect(rms(out[1], 8000, 36000)).toBeLessThan(0.01); // channel with only 2 kHz is removed

    const inv = applyMask([mix], SR, 12, m, { backgroundGain: 0, invert: true });
    expect(rms(inv[0], 8000, 36000)).toBeCloseTo(rms(y, 8000, 36000), 1); // 2 kHz survives
  });

  it('with no mask everything goes to the background gain', () => {
    const out = applyMask([mix], SR, 11, null, { backgroundGain: 0.5, invert: false });
    expect(out[0][20000]).toBeCloseTo(mix[20000] * 0.5, 4);
  });
});

describe('MaskGrid', () => {
  it('paints, erases and undoes strokes', () => {
    const m = new MaskGrid(2), map = mapping(m);
    m.beginStroke();
    m.stamp(map, 500, 200, 15, 1, false);
    m.endStroke();
    const row = Math.round(hzToRow(map.yToHz(200)));
    expect(m.sample(1, row)).toBeGreaterThan(0.9);
    m.beginStroke();
    m.stamp(map, 500, 200, 30, 1, true);
    m.endStroke();
    expect(m.sample(1, row)).toBe(0);
    m.undo();
    expect(m.sample(1, row)).toBeGreaterThan(0.9);
    m.undo();
    expect(m.isEmpty()).toBe(true);
    m.redo();
    expect(m.isEmpty()).toBe(false);
  });
});

describe('encoders', () => {
  it('writes a valid 16-bit WAV', () => {
    const wav = encodeWav([Float32Array.of(0, 1, -1, 0.5)], SR, 'pcm16');
    const v = new DataView(wav.buffer);
    expect(String.fromCharCode(...wav.slice(0, 4))).toBe('RIFF');
    expect(v.getUint32(24, true)).toBe(SR);
    expect(v.getUint32(40, true)).toBe(8);
    expect(v.getInt16(46, true)).toBe(32767);
    expect(v.getInt16(48, true)).toBe(-32768);
  });

  it('writes MP3 frames', () => {
    const mp3 = encodeMp3([sine(440, 1), sine(660, 1)], SR, 128);
    expect(mp3.length).toBeGreaterThan(10000);
    expect(mp3[0]).toBe(0xff); // frame sync
  });
});
