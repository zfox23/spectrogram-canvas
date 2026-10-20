export type WavFormat = 'pcm16' | 'pcm24' | 'float32';

/** Encodes channels as an interleaved RIFF/WAVE file. */
export function encodeWav(channels: Float32Array[], sampleRate: number, format: WavFormat = 'pcm16'): Uint8Array {
  const numCh = channels.length;
  const frames = channels[0].length;
  const bytes = format === 'pcm16' ? 2 : format === 'pcm24' ? 3 : 4;
  const dataSize = frames * numCh * bytes;
  const out = new Uint8Array(44 + dataSize);
  const v = new DataView(out.buffer);

  const str = (o: number, s: string) => { for (let i = 0; i < s.length; i++) v.setUint8(o + i, s.charCodeAt(i)); };
  str(0, 'RIFF'); v.setUint32(4, 36 + dataSize, true); str(8, 'WAVE');
  str(12, 'fmt '); v.setUint32(16, 16, true);
  v.setUint16(20, format === 'float32' ? 3 : 1, true);
  v.setUint16(22, numCh, true);
  v.setUint32(24, sampleRate, true);
  v.setUint32(28, sampleRate * numCh * bytes, true);
  v.setUint16(32, numCh * bytes, true);
  v.setUint16(34, bytes * 8, true);
  str(36, 'data'); v.setUint32(40, dataSize, true);

  let o = 44;
  for (let i = 0; i < frames; i++) {
    for (let c = 0; c < numCh; c++) {
      const x = channels[c][i];
      if (format === 'float32') {
        v.setFloat32(o, x, true);
      } else if (format === 'pcm16') {
        const s = Math.max(-1, Math.min(1, x));
        v.setInt16(o, Math.round(s < 0 ? s * 32768 : s * 32767), true);
      } else {
        const s = Math.max(-1, Math.min(1, x));
        const q = Math.round(s < 0 ? s * 8388608 : s * 8388607);
        v.setUint8(o, q & 0xff); v.setUint8(o + 1, (q >> 8) & 0xff); v.setUint8(o + 2, (q >> 16) & 0xff);
      }
      o += bytes;
    }
  }
  return out;
}
