import type * as FlacApi from 'libflacjs/dist/index.d';
import wasmUrl from 'libflacjs/dist/libflac.min.wasm.wasm?url';

type Flac = typeof FlacApi;
const COMPRESSION = 8;
const BLOCK_FRAMES = 65536;

let flacPromise: Promise<Flac> | null = null;

/** Loads libFLAC (wasm) on first use, so it costs nothing until something is encoded. */
function loadFlac(): Promise<Flac> {
  flacPromise ??= (async () => {
    // libflac.js looks this up when it loads, to find its .wasm under the bundler's file name.
    if (typeof self !== 'undefined') (self as { FLAC_SCRIPT_LOCATION?: unknown }).FLAC_SCRIPT_LOCATION = { 'libflac.min.wasm.wasm': wasmUrl };
    const mod = await import('libflacjs/dist/libflac.min.wasm.js');
    const Flac = ((mod as { default?: Flac }).default ?? mod) as Flac;
    if (!Flac.isReady()) await new Promise<void>((resolve) => Flac.on('ready', () => resolve()));
    return Flac;
  })();
  return flacPromise;
}

/** Encodes channels as a lossless 24-bit FLAC file. */
export async function encodeFlac(channels: Float32Array[], sampleRate: number, onProgress?: (f: number) => void): Promise<Uint8Array> {
  const Flac = await loadFlac();
  const frames = channels[0].length;
  const enc = Flac.create_libflac_encoder(sampleRate, channels.length, 24, COMPRESSION, frames, false, 0);
  if (!enc) throw new Error('Could not create the FLAC encoder.');

  const chunks: Uint8Array[] = [];
  let md5 = '';
  try {
    const status = Flac.init_encoder_stream(enc, (data) => { chunks.push(data.slice()); }, (meta) => { if (meta) md5 = meta.md5sum; });
    if (status !== 0) throw new Error(`FLAC encoder init failed (${status}).`);

    // Same quantisation as encodeWav's 'pcm24'.
    const block = channels.map(() => new Int32Array(BLOCK_FRAMES));
    for (let start = 0; start < frames; start += BLOCK_FRAMES) {
      const n = Math.min(BLOCK_FRAMES, frames - start);
      channels.forEach((ch, c) => {
        const out = block[c];
        for (let i = 0; i < n; i++) {
          const s = Math.max(-1, Math.min(1, ch[start + i]));
          out[i] = Math.round(s < 0 ? s * 8388608 : s * 8388607);
        }
      });
      const bufs = n === BLOCK_FRAMES ? block : block.map((b) => b.subarray(0, n));
      if (!Flac.FLAC__stream_encoder_process(enc, bufs, n)) throw new Error('FLAC encoding failed.');
      onProgress?.((start + n) / frames);
    }
    if (!Flac.FLAC__stream_encoder_finish(enc)) throw new Error('FLAC encoding failed to finish.');
  } finally {
    Flac.FLAC__stream_encoder_delete(enc);
  }

  const out = new Uint8Array(chunks.reduce((n, c) => n + c.length, 0));
  let o = 0;
  for (const c of chunks) { out.set(c, o); o += c.length; }
  // A stream encoder can't seek back, so the STREAMINFO it wrote first lacks the MD5: patch it in.
  if (/^[0-9a-f]{32}$/i.test(md5)) for (let i = 0; i < 16; i++) out[8 + 18 + i] = parseInt(md5.slice(i * 2, i * 2 + 2), 16);
  return out;
}
