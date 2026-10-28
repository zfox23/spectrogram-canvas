import type { WorkerRequest, WorkerResponse } from './worker';
import type { Spectrogram } from './analysis';
import type { FilterSettings, MaskData } from './filter';
import type { WavFormat } from '../encode/wav';

type Distribute<T> = T extends unknown ? Omit<T, 'id'> : never;

/** Runs the heavy DSP and encoding off the UI thread. Inputs are copied, never detached. */
export class DspWorker {
  private worker = new Worker(new URL('./worker.ts', import.meta.url), { type: 'module' });
  private nextId = 1;
  private pending = new Map<number, { resolve: (v: any) => void; reject: (e: Error) => void; onProgress?: (f: number) => void }>();

  constructor() {
    this.worker.onmessage = (e: MessageEvent<WorkerResponse>) => {
      const msg = e.data;
      const p = this.pending.get(msg.id);
      if (!p) return;
      if (msg.type === 'progress') return p.onProgress?.(msg.fraction);
      this.pending.delete(msg.id);
      if (msg.type === 'done') p.resolve(msg.result); else p.reject(new Error(msg.message));
    };
  }

  private call<T>(req: Distribute<WorkerRequest>, onProgress?: (f: number) => void): Promise<T> {
    const id = this.nextId++;
    return new Promise<T>((resolve, reject) => {
      this.pending.set(id, { resolve, reject, onProgress });
      this.worker.postMessage({ ...req, id });
    });
  }

  analyse(channels: Float32Array[], sampleRate: number, order: number, onProgress?: (f: number) => void) {
    return this.call<Spectrogram>({ type: 'analyse', channels, sampleRate, order }, onProgress);
  }

  filter(channels: Float32Array[], sampleRate: number, order: number, mask: MaskData | null, settings: FilterSettings, onProgress?: (f: number) => void) {
    return this.call<Float32Array[]>({ type: 'filter', channels, sampleRate, order, mask, settings }, onProgress);
  }

  wav(channels: Float32Array[], sampleRate: number, format: WavFormat) {
    return this.call<Uint8Array>({ type: 'wav', channels, sampleRate, format });
  }

  mp3(channels: Float32Array[], sampleRate: number, kbps: number, onProgress?: (f: number) => void) {
    return this.call<Uint8Array>({ type: 'mp3', channels, sampleRate, kbps }, onProgress);
  }

  flac(channels: Float32Array[], sampleRate: number, onProgress?: (f: number) => void) {
    return this.call<Uint8Array>({ type: 'flac', channels, sampleRate }, onProgress);
  }
}
