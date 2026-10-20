import { FFT } from './fft';
import { MASK_ROWS, hzToRow } from '../model/maskGrid';

export interface MaskData {
  cols: number;
  colSeconds: number;
  data: Uint8Array;
}

export interface FilterSettings {
  /** Linear gain applied to unpainted audio (0 = silent). */
  backgroundGain: number;
  /** Keep everything except what was painted. */
  invert: boolean;
}

/**
 * Applies the painted mask with an STFT: each frame's spectrum is multiplied by gains read from
 * the mask at that frame's time. Uses a sqrt-Hann analysis/synthesis window pair with 75%
 * overlap, which reconstructs the input exactly when every gain is 1. The result is aligned
 * with the input (no latency).
 *
 * Channels are processed in pairs as one complex FFT (left in the real part, right in the
 * imaginary part). The gains are real and symmetric, so they scale both channels independently.
 */
export function applyMask(
  channels: Float32Array[],
  sampleRate: number,
  order: number,
  mask: MaskData | null,
  settings: FilterSettings,
  onProgress?: (fraction: number) => void,
): Float32Array[] {
  const n = 1 << order, hop = n / 4, half = n / 2, numBins = half + 1;
  const length = channels[0].length;
  const padded = length + 2 * n;

  const window = new Float32Array(n);
  for (let i = 0; i < n; i++) window[i] = Math.sqrt(0.5 - 0.5 * Math.cos((2 * Math.PI * i) / n));

  // Row position of each bin on the mask grid, split into integer + fraction once.
  const rowIndex = new Int32Array(numBins), rowFrac = new Float32Array(numBins);
  for (let k = 0; k < numBins; k++) {
    const r = Math.min(Math.max(hzToRow((k * sampleRate) / n), 0), MASK_ROWS - 1);
    const r0 = Math.min(Math.floor(r), MASK_ROWS - 2);
    rowIndex[k] = r0;
    rowFrac[k] = r - r0;
  }

  const fft = new FFT(n);
  const re = new Float32Array(n), im = new Float32Array(n);
  const gains = new Float32Array(numBins);
  const bg = settings.backgroundGain;

  const numPairs = Math.ceil(channels.length / 2);
  const outRe: Float32Array[] = [], outIm: Float32Array[] = [];
  for (let p = 0; p < numPairs; p++) {
    outRe.push(new Float32Array(padded));
    outIm.push(new Float32Array(padded));
  }

  const computeGains = (seconds: number) => {
    if (!mask) {
      gains.fill(0);
    } else {
      const colPos = seconds / mask.colSeconds - 0.5;
      const c0 = Math.floor(colPos), cf = colPos - c0;
      const d = mask.data;
      const last = mask.cols - 1;
      const base0 = (c0 < 0 ? 0 : c0 > last ? last : c0) * MASK_ROWS;
      const base1 = (c0 + 1 < 0 ? 0 : c0 + 1 > last ? last : c0 + 1) * MASK_ROWS;
      for (let k = 0; k < numBins; k++) {
        const r0 = rowIndex[k], rf = rowFrac[k];
        const a = d[base0 + r0] + rf * (d[base0 + r0 + 1] - d[base0 + r0]);
        const b = d[base1 + r0] + rf * (d[base1 + r0 + 1] - d[base1 + r0]);
        gains[k] = (a + cf * (b - a)) / 255;
      }
    }
    for (let k = 0; k < numBins; k++) {
      const m = settings.invert ? 1 - gains[k] : gains[k];
      gains[k] = bg + (1 - bg) * m;
    }
  };

  const numFrames = Math.floor((padded - n) / hop) + 1;
  for (let f = 0; f < numFrames; f++) {
    const start = f * hop; // in padded samples; the original signal begins at offset n
    computeGains((start + half - n) / sampleRate);

    for (let p = 0; p < numPairs; p++) {
      const a = channels[2 * p], b = channels[2 * p + 1];
      for (let i = 0; i < n; i++) {
        const o = start + i - n;
        const inside = o >= 0 && o < length;
        re[i] = inside ? a[o] * window[i] : 0;
        im[i] = inside && b ? b[o] * window[i] : 0;
      }
      fft.transform(re, im);
      for (let k = 0; k <= half; k++) {
        const g = gains[k];
        re[k] *= g; im[k] *= g;
        if (k > 0 && k < half) { re[n - k] *= g; im[n - k] *= g; }
      }
      fft.transform(re, im, true);

      // sqrt-Hann squared at 75% overlap sums to 2, so scale by 1/2.
      const oRe = outRe[p], oIm = outIm[p];
      for (let i = 0; i < n; i++) {
        const w = window[i] * 0.5;
        oRe[start + i] += re[i] * w;
        oIm[start + i] += im[i] * w;
      }
    }
    if (onProgress && (f & 0x3f) === 0) onProgress(f / numFrames);
  }

  const result: Float32Array[] = [];
  for (let c = 0; c < channels.length; c++) {
    const src = c % 2 === 0 ? outRe[c >> 1] : outIm[c >> 1];
    result.push(src.slice(n, n + length));
  }
  return result;
}
