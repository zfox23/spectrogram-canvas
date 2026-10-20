import { Mp3Encoder } from '@breezystack/lamejs';

const BLOCK = 1152 * 8;

const toInt16 = (src: Float32Array, start: number, end: number) => {
  const out = new Int16Array(end - start);
  for (let i = start; i < end; i++) {
    const s = Math.max(-1, Math.min(1, src[i]));
    out[i - start] = Math.round(s < 0 ? s * 32768 : s * 32767);
  }
  return out;
};

/** Encodes one or two channels (extra channels are dropped) to MP3 at a constant bitrate. */
export function encodeMp3(
  channels: Float32Array[],
  sampleRate: number,
  kbps: number,
  onProgress?: (fraction: number) => void,
): Uint8Array {
  const numCh = Math.min(channels.length, 2);
  const enc = new Mp3Encoder(numCh, sampleRate, kbps);
  const chunks: Uint8Array[] = [];
  const length = channels[0].length;

  for (let i = 0; i < length; i += BLOCK) {
    const end = Math.min(length, i + BLOCK);
    const l = toInt16(channels[0], i, end);
    const buf = numCh === 2 ? enc.encodeBuffer(l, toInt16(channels[1], i, end)) : enc.encodeBuffer(l);
    if (buf.length) chunks.push(buf);
    onProgress?.(end / length);
  }
  const tail = enc.flush();
  if (tail.length) chunks.push(tail);

  const out = new Uint8Array(chunks.reduce((s, c) => s + c.length, 0));
  let o = 0;
  for (const c of chunks) { out.set(c, o); o += c.length; }
  return out;
}
