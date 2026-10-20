export interface DecodedAudio {
  channels: Float32Array[];
  sampleRate: number;
  duration: number;
}

/** Reads the sample rate from a RIFF/WAVE header so decoding doesn't resample it. */
function sniffWavRate(buf: ArrayBuffer): number | null {
  if (buf.byteLength < 28) return null;
  const v = new DataView(buf);
  const tag = (o: number) => String.fromCharCode(v.getUint8(o), v.getUint8(o + 1), v.getUint8(o + 2), v.getUint8(o + 3));
  if (tag(0) !== 'RIFF' || tag(8) !== 'WAVE') return null;
  for (let o = 12; o + 8 <= buf.byteLength; ) {
    const size = v.getUint32(o + 4, true);
    if (tag(o) === 'fmt ' && o + 16 <= buf.byteLength) return v.getUint32(o + 12, true);
    o += 8 + size + (size & 1);
  }
  return null;
}

/** Decodes any audio file the browser understands, keeping the file's own sample rate when it can. */
export async function decodeAudioFile(file: File): Promise<DecodedAudio> {
  const bytes = await file.arrayBuffer();
  const rate = sniffWavRate(bytes);

  const attempts: (() => BaseAudioContext)[] = [];
  if (rate) attempts.push(() => new OfflineAudioContext(1, 1, rate));
  attempts.push(() => new AudioContext());

  let lastError: unknown;
  for (const make of attempts) {
    try {
      const ctx = make();
      const buffer = await ctx.decodeAudioData(bytes.slice(0));
      if (ctx instanceof AudioContext) void ctx.close();
      const channels = Array.from({ length: buffer.numberOfChannels }, (_, c) => buffer.getChannelData(c).slice());
      return { channels, sampleRate: buffer.sampleRate, duration: buffer.duration };
    } catch (err) {
      lastError = err;
    }
  }
  throw new Error(`Could not decode "${file.name}": ${lastError instanceof Error ? lastError.message : 'unsupported format'}`);
}
