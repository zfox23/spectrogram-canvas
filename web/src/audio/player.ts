/** Plays a decoded buffer from an arbitrary position, reporting a position for the playhead. */
export class Player {
  private ctx: AudioContext | null = null;
  private buffer: AudioBuffer | null = null;
  private source: AudioBufferSourceNode | null = null;
  private startedAt = 0;
  private offset = 0;
  playing = false;
  /** When set, playback that starts before `end` wraps from `end` back to `start`. */
  private loop: { start: number; end: number } | null = null;
  private looping = false;
  onEnded: (() => void) | null = null;

  setAudio(channels: Float32Array[], sampleRate: number) {
    const wasPlaying = this.playing;
    const pos = this.position;
    this.stop();

    if (this.ctx && this.ctx.sampleRate !== sampleRate) {
      void this.ctx.close();
      this.ctx = null;
    }
    if (!this.ctx) {
      try { this.ctx = new AudioContext({ sampleRate }); } catch { this.ctx = new AudioContext(); }
    }
    this.buffer = this.ctx.createBuffer(channels.length, channels[0].length, sampleRate);
    channels.forEach((c, i) => this.buffer!.copyToChannel(c as Float32Array<ArrayBuffer>, i));
    this.offset = Math.min(pos, this.buffer.duration);
    if (wasPlaying) this.play(this.offset);
  }

  get duration() {
    return this.buffer?.duration ?? 0;
  }

  /** Current position in seconds (where playback would resume when stopped). */
  get position() {
    if (!this.playing || !this.ctx) return this.offset;
    const raw = this.offset + (this.ctx.currentTime - this.startedAt);
    if (this.looping && this.loop && raw >= this.loop.end) {
      const len = this.loop.end - this.loop.start;
      return this.loop.start + ((raw - this.loop.start) % len);
    }
    return Math.min(this.duration, raw);
  }

  setLoop(loop: { start: number; end: number } | null) {
    this.loop = loop && loop.end - loop.start > 0.01 ? loop : null;
    if (this.playing) this.play(this.position);
  }

  play(from = this.offset) {
    if (!this.ctx || !this.buffer) return;
    this.stopSource();
    void this.ctx.resume();
    if (from >= this.buffer.duration - 0.01) from = 0;
    const src = this.ctx.createBufferSource();
    src.buffer = this.buffer;
    src.connect(this.ctx.destination);
    this.looping = !!this.loop && from < this.loop.end;
    if (this.looping) {
      src.loop = true;
      src.loopStart = this.loop!.start;
      src.loopEnd = this.loop!.end;
    }
    src.onended = () => {
      if (this.source !== src) return;
      this.source = null;
      this.playing = false;
      this.offset = this.buffer ? this.buffer.duration : 0;
      this.onEnded?.();
    };
    this.offset = from;
    this.startedAt = this.ctx.currentTime;
    src.start(0, from);
    this.source = src;
    this.playing = true;
  }

  stop() {
    if (this.playing) this.offset = this.position;
    this.stopSource();
    this.playing = false;
  }

  seek(seconds: number) {
    if (this.playing) this.play(seconds); else this.offset = seconds;
  }

  private stopSource() {
    if (this.source) {
      const s = this.source;
      this.source = null;
      s.onended = null;
      try { s.stop(); } catch { /* already stopped */ }
      s.disconnect();
    }
  }
}
