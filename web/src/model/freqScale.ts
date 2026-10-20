export type FreqScaleType = 'log' | 'linear' | 'mel';

const hzToMel = (hz: number) => 2595 * Math.log10(1 + hz / 700);
const melToHz = (mel: number) => 700 * (Math.pow(10, mel / 2595) - 1);

/** Maps frequencies in Hz to a normalised display position in [0, 1] (0 = bottom, 1 = top). */
export class FreqScale {
  constructor(
    public type: FreqScaleType = 'log',
    public minHz = 20,
    public maxHz = 22050,
  ) {}

  hzToNorm(hz: number): number {
    switch (this.type) {
      case 'linear':
        return (hz - this.minHz) / (this.maxHz - this.minHz);
      case 'mel':
        return (hzToMel(hz) - hzToMel(this.minHz)) / (hzToMel(this.maxHz) - hzToMel(this.minHz));
      default:
        return Math.log(Math.max(hz, 1e-3) / this.minHz) / Math.log(this.maxHz / this.minHz);
    }
  }

  normToHz(n: number): number {
    switch (this.type) {
      case 'linear':
        return this.minHz + n * (this.maxHz - this.minHz);
      case 'mel':
        return melToHz(hzToMel(this.minHz) + n * (hzToMel(this.maxHz) - hzToMel(this.minHz)));
      default:
        return this.minHz * Math.pow(this.maxHz / this.minHz, n);
    }
  }
}
