/// <reference lib="webworker" />
import { analyse, mixToMono } from './analysis';
import { applyMask, type FilterSettings, type MaskData } from './filter';
import { encodeWav, type WavFormat } from '../encode/wav';
import { encodeMp3 } from '../encode/mp3';

export type WorkerRequest =
  | { id: number; type: 'analyse'; channels: Float32Array[]; sampleRate: number; order: number }
  | { id: number; type: 'filter'; channels: Float32Array[]; sampleRate: number; order: number; mask: MaskData | null; settings: FilterSettings }
  | { id: number; type: 'wav'; channels: Float32Array[]; sampleRate: number; format: WavFormat }
  | { id: number; type: 'mp3'; channels: Float32Array[]; sampleRate: number; kbps: number };

export type WorkerResponse =
  | { id: number; type: 'progress'; fraction: number }
  | { id: number; type: 'done'; result: unknown }
  | { id: number; type: 'error'; message: string };

const post = (msg: WorkerResponse, transfer: Transferable[] = []) => (self as DedicatedWorkerGlobalScope).postMessage(msg, transfer);

self.onmessage = (e: MessageEvent<WorkerRequest>) => {
  const req = e.data;
  const progress = (fraction: number) => post({ id: req.id, type: 'progress', fraction });
  try {
    switch (req.type) {
      case 'analyse': {
        const spec = analyse(mixToMono(req.channels), req.sampleRate, req.order, progress);
        post({ id: req.id, type: 'done', result: spec }, [spec.data.buffer]);
        break;
      }
      case 'filter': {
        const out = applyMask(req.channels, req.sampleRate, req.order, req.mask, req.settings, progress);
        post({ id: req.id, type: 'done', result: out }, out.map((c) => c.buffer));
        break;
      }
      case 'wav': {
        const bytes = encodeWav(req.channels, req.sampleRate, req.format);
        post({ id: req.id, type: 'done', result: bytes }, [bytes.buffer]);
        break;
      }
      case 'mp3': {
        const bytes = encodeMp3(req.channels, req.sampleRate, req.kbps, progress);
        post({ id: req.id, type: 'done', result: bytes }, [bytes.buffer]);
        break;
      }
    }
  } catch (err) {
    post({ id: req.id, type: 'error', message: err instanceof Error ? err.message : String(err) });
  }
};
