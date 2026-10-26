// drains the int16 stereo ring the firmware worker fills (dac1 = master out).
// ring: ctrl Int32[0] = frames written, ctrl[1] = frames read, ctrl[2] = underrun frames.
class Ring extends AudioWorkletProcessor {
  constructor(o) {
    super();
    const { ctrl, data } = o.processorOptions;
    this.ctrl = new Int32Array(ctrl); this.data = new Int16Array(data);
    this.cap = this.data.length / 2;
  }
  process(_, outputs) {
    const [l, r] = outputs[0];
    const w = Atomics.load(this.ctrl, 0); let rd = Atomics.load(this.ctrl, 1), under = 0;
    for (let i = 0; i < l.length; i++) {
      if (rd < w) { const k = (rd % this.cap) * 2; l[i] = this.data[k] / 32768; r[i] = this.data[k + 1] / 32768; rd++; }
      else { l[i] = 0; r[i] = 0; under++; }
    }
    Atomics.store(this.ctrl, 1, rd);
    if (under) Atomics.add(this.ctrl, 2, under);
    Atomics.notify(this.ctrl, 1);
    return true;
  }
}
registerProcessor("ring", Ring);
