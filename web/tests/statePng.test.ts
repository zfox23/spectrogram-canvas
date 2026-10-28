import { describe, expect, it } from 'vitest';
import { base64ToBytes, bytesToBase64, embedState, extractAudio, extractState } from '../src/io/statePng';

// Smallest valid PNG: 1x1 grey pixel.
const PNG = Uint8Array.from(atob('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAAAAAA6fptVAAAACklEQVR4nGNgAAAAAgABSK+kcQAAAABJRU5ErkJggg=='), (c) => c.charCodeAt(0));

describe('statePng', () => {
  it('round-trips a state object through a PNG', async () => {
    const state = { a: 1, text: 'héllo', nested: { list: [1, 2, 3] }, blob: bytesToBase64(new Uint8Array(70000).fill(7)) };
    const out = await embedState(PNG, state);
    expect(out.length).toBeGreaterThan(PNG.length);
    expect(await extractState(out)).toEqual(state);
    expect(base64ToBytes(state.blob).length).toBe(70000);
  });

  it('returns null for a PNG without state and rejects non-PNG input', async () => {
    expect(await extractState(PNG)).toBeNull();
    await expect(extractState(new Uint8Array(40))).rejects.toThrow('Not a PNG');
  });

  it('round-trips the audio file bytes alongside the state', async () => {
    const audio = new Uint8Array(200_000);
    for (let i = 0; i < audio.length; i++) audio[i] = (i * 2654435761) >>> 24;
    const state = { a: 1 };
    const out = await embedState(PNG, state, audio);
    expect(await extractState(out)).toEqual(state);
    expect(extractAudio(out)).toEqual(audio);
    expect(chunkCrcsValid(out)).toBe(true);
  });

  it('returns null audio when none was embedded', async () => {
    expect(extractAudio(PNG)).toBeNull();
    expect(extractAudio(await embedState(PNG, { a: 1 }))).toBeNull();
  });
});

function chunkCrcsValid(png: Uint8Array): boolean {
  const table = Array.from({ length: 256 }, (_, n) => {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    return c >>> 0;
  });
  const view = new DataView(png.buffer, png.byteOffset, png.byteLength);
  for (let o = 8; o < png.length; ) {
    const len = view.getUint32(o);
    let c = 0xffffffff;
    for (let i = o + 4; i < o + 8 + len; i++) c = table[(c ^ png[i]) & 0xff] ^ (c >>> 8);
    if (((c ^ 0xffffffff) >>> 0) !== view.getUint32(o + 8 + len)) return false;
    o += 12 + len;
  }
  return true;
}
