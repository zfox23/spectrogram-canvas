/** Stores the app state inside a PNG as a zlib-compressed iTXt chunk, and reads it back. */

const KEYWORD = 'SpectrogramDrawFilter';
const SIGNATURE = [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a];

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

function crc32(bytes: Uint8Array): number {
  let c = 0xffffffff;
  for (let i = 0; i < bytes.length; i++) c = CRC_TABLE[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

async function pipe(data: Uint8Array, stream: CompressionStream | DecompressionStream): Promise<Uint8Array> {
  const out = new Response(new Blob([data as Uint8Array<ArrayBuffer>]).stream().pipeThrough(stream));
  return new Uint8Array(await out.arrayBuffer());
}

const ascii = (bytes: Uint8Array, a: number, b: number) => String.fromCharCode(...bytes.subarray(a, b));

function checkSignature(png: Uint8Array) {
  if (png.length < 20 || SIGNATURE.some((b, i) => png[i] !== b)) throw new Error('Not a PNG file.');
}

export async function sha256Hex(buffer: ArrayBuffer): Promise<string> {
  if (!globalThis.crypto?.subtle) throw new Error('Hashing needs a secure context (https or localhost).');
  const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', buffer));
  return Array.from(digest, (b) => b.toString(16).padStart(2, '0')).join('');
}

export function bytesToBase64(bytes: Uint8Array): string {
  let s = '';
  for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
  return btoa(s);
}

export function base64ToBytes(text: string): Uint8Array {
  const s = atob(text);
  const out = new Uint8Array(s.length);
  for (let i = 0; i < s.length; i++) out[i] = s.charCodeAt(i);
  return out;
}

/** Returns a copy of `png` with `state` stored as JSON just before the IEND chunk. */
export async function embedState(png: Uint8Array, state: unknown): Promise<Uint8Array> {
  checkSignature(png);
  const text = await pipe(new TextEncoder().encode(JSON.stringify(state)), new CompressionStream('deflate'));
  // iTXt: keyword NUL, compression flag, method, language NUL, translated keyword NUL, text
  const head = new TextEncoder().encode(`${KEYWORD}\0`);
  const body = new Uint8Array(head.length + 4 + text.length);
  body.set(head, 0);
  body.set([1, 0, 0, 0], head.length); // compressed (zlib), empty language tag, empty translated keyword
  body.set(text, head.length + 4);

  const chunk = new Uint8Array(12 + body.length);
  const view = new DataView(chunk.buffer);
  view.setUint32(0, body.length);
  chunk.set(new TextEncoder().encode('iTXt'), 4);
  chunk.set(body, 8);
  view.setUint32(8 + body.length, crc32(chunk.subarray(4, 8 + body.length)));

  const iend = png.length - 12; // IEND is always the last chunk: 4 length + 4 type + 4 crc
  if (ascii(png, iend + 4, iend + 8) !== 'IEND') throw new Error('Malformed PNG (no IEND chunk).');
  const out = new Uint8Array(png.length + chunk.length);
  out.set(png.subarray(0, iend), 0);
  out.set(chunk, iend);
  out.set(png.subarray(iend), iend + chunk.length);
  return out;
}

/** The state stored by embedState, or null if the PNG carries none. */
export async function extractState(png: Uint8Array): Promise<unknown | null> {
  checkSignature(png);
  const view = new DataView(png.buffer, png.byteOffset, png.byteLength);
  for (let o = 8; o + 12 <= png.length; ) {
    const len = view.getUint32(o), type = ascii(png, o + 4, o + 8);
    const start = o + 8, end = start + len;
    if (end + 4 > png.length) break;
    if (type === 'iTXt') {
      const data = png.subarray(start, end);
      const nul = data.indexOf(0);
      if (nul > 0 && ascii(data, 0, nul) === KEYWORD) {
        const compressed = data[nul + 1] === 1;
        let p = nul + 3;
        for (let n = 0; n < 2; n++) p = data.indexOf(0, p) + 1; // language tag, translated keyword
        const payload = data.subarray(p);
        const raw = compressed ? await pipe(payload, new DecompressionStream('deflate')) : payload;
        return JSON.parse(new TextDecoder().decode(raw));
      }
    }
    if (type === 'IEND') break;
    o = end + 4;
  }
  return null;
}
