import { describe, expect, it } from 'vitest';
import { base64ToBytes, bytesToBase64, embedState, extractState } from '../src/io/statePng';

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
});
