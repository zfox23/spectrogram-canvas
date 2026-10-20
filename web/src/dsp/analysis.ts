import { FFT } from './fft';

export const FLOOR_DB = -110;
export const CEIL_DB = 0;

export const MIN_ORDER = 9; // 512
export const MAX_ORDER = 14; // 16384
export const DEFAULT_ORDER = 12; // 4096

/** Magnitude spectrogram as 8-bit dB: value 0 is FLOOR_DB or below, 255 is CEIL_DB or above. */
export interface Spectrogram {
  order: number;
  size: number;
  hop: number;
  numBins: number;
  numFrames: number;
  sampleRate: number;
  /** frame-major: data[frame * numBins + bin]. Frame f is centred on sample f * hop. */
  data: Uint8Array;
}

const MAX_CELLS = 160e6;

export function mixToMono(channels: Float32Array[]): Float32Array {
  if (channels.length === 1) return channels[0];
  const out = new Float32Array(channels[0].length);
  for (const ch of channels) for (let i = 0; i < out.length; i++) out[i] += ch[i];
  const g = 1 / channels.length;
  for (let i = 0; i < out.length; i++) out[i] *= g;
  return out;
}

export function analyse(
  mono: Float32Array,
  sampleRate: number,
  order: number,
  onProgress?: (fraction: number) => void,
): Spectrogram {
  const size = 1 << order;
  const numBins = size / 2 + 1;
  let hop = size / 4;
  // Very long audio at large FFT sizes: widen the hop to keep memory bounded.
  while ((mono.length / hop + 1) * numBins > MAX_CELLS) hop *= 2;
  const numFrames = Math.floor(mono.length / hop) + 1;

  const window = new Float32Array(size);
  let windowSum = 0;
  for (let i = 0; i < size; i++) {
    window[i] = 0.5 - 0.5 * Math.cos((2 * Math.PI * i) / size);
    windowSum += window[i];
  }
  const scale = 2 / windowSum; // a full-scale sine reads as 0 dB
  const dbScale = 255 / (CEIL_DB - FLOOR_DB);

  const data = new Uint8Array(numFrames * numBins);
  const fft = new FFT(size);
  const re = new Float32Array(size), im = new Float32Array(size);

  const load = (dst: Float32Array, frame: number) => {
    const start = frame * hop - size / 2;
    for (let i = 0; i < size; i++) {
      const p = start + i;
      dst[i] = p >= 0 && p < mono.length ? mono[p] * window[i] : 0;
    }
  };
  const toByte = (mag: number) => {
    const db = 20 * Math.log10(mag * scale + 1e-12);
    const v = Math.round((db - FLOOR_DB) * dbScale);
    return v < 0 ? 0 : v > 255 ? 255 : v;
  };

  // Two real frames per complex FFT: frame a in the real part, frame b in the imaginary part.
  for (let f = 0; f < numFrames; f += 2) {
    const hasB = f + 1 < numFrames;
    load(re, f);
    if (hasB) load(im, f + 1); else im.fill(0);
    fft.transform(re, im);

    const rowA = f * numBins, rowB = (f + 1) * numBins;
    for (let k = 0; k < numBins; k++) {
      const m = (size - k) & (size - 1);
      data[rowA + k] = toByte(0.5 * Math.hypot(re[k] + re[m], im[k] - im[m]));
      if (hasB) data[rowB + k] = toByte(0.5 * Math.hypot(im[k] + im[m], re[k] - re[m]));
    }
    if (onProgress && (f & 0xff) === 0) onProgress(f / numFrames);
  }

  return { order, size, hop, numBins, numFrames, sampleRate, data };
}
