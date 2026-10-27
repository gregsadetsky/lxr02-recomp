// runs the translated firmware. it never returns, so input arrives through a shared ring
// (inCtrl/inData), never through onmessage. audio out blocks here while the ring is full,
// which paces the firmware's virtual time to the audio clock.
onmessage = async (ev) => {
  const { engine, card, ctrl, data, inCtrl, inData, maxFill, capture } = ev.data;
  // ?capture=<frames>: hand the page the first frames exactly as written to the ring (tests)
  let cap_buf = capture ? new Int16Array(capture * 2) : null, cap_n = 0;
  // "exact": the arm gnu build translated (bit identical to the cortex-m7 reference).
  // "plain": the same sources built by clang for wasm directly (fast math differs).
  importScripts(engine === "plain" ? "plain/lxr-web.js" : "lxr.js");
  const c = new Int32Array(ctrl), d = new Int16Array(data), cap = d.length / 2;
  const ic = new Int32Array(inCtrl), id = new Uint8Array(inData);
  const dec = new TextDecoder(); let lcd = "", midi = "", rest = new Uint8Array(0);
  const lines = (buf, cb) => { let i; while ((i = buf.indexOf("\n")) >= 0) { cb(buf.slice(0, i)); buf = buf.slice(i + 1); } return buf; };
  const host = {
    write(kind, b) {
      if (kind === 0) {                       // dac1.raw: int16 stereo frames (writes may split a frame)
        if (rest.length) { const t = new Uint8Array(rest.length + b.length); t.set(rest); t.set(b, rest.length); b = t; }
        const whole = b.length & ~3; rest = b.slice(whole);
        if (!whole) return;
        const s = new Int16Array(b.buffer.slice(b.byteOffset, b.byteOffset + whole));
        const frames = s.length / 2;
        const w = Atomics.load(c, 0);
        while (w + frames - Atomics.load(c, 1) > maxFill) Atomics.wait(c, 1, Atomics.load(c, 1), 20);
        for (let i = 0; i < frames; i++) { const k = ((w + i) % cap) * 2; d[k] = s[2 * i]; d[k + 1] = s[2 * i + 1]; }
        if (cap_buf) {
          const m = Math.min(frames, capture - cap_n); cap_buf.set(s.subarray(0, 2 * m), 2 * cap_n); cap_n += m;
          if (cap_n === capture) { postMessage({ capture: cap_buf }); cap_buf = null; }
        }
        Atomics.store(c, 0, w + frames);
      } else if (kind === 2) {                // lcd.txt
        lcd = lines(lcd + dec.decode(b), (l) => postMessage({ lcd: l }));
      } else if (kind === 3) {                // midiout.txt
        midi = lines(midi + dec.decode(b), (l) => postMessage({ midiout: l }));
      } else if (kind === 6) {                // snapshot.ready: the firmware's saves are on the card
        const cur = mod.FS.readFile("card.img");
        postMessage({ card: cur }, [cur.buffer]);
      }
    },
    readByte() {                              // live.in: input lines from the page
      const w = Atomics.load(ic, 0), r = Atomics.load(ic, 1);
      if (r >= w) return null;
      Atomics.store(ic, 1, r + 1);
      return id[r % id.length];
    },
  };
  let mod;
  const files = (FS) => {
    FS.chdir("/");
    FS.writeFile("scenario.txt", "live\nseed c0ffee\n");
    if (card) FS.writeFile("card.img", new Uint8Array(card));
  };
  const opts = { print: (t) => postMessage({ log: t }), printErr: (t) => postMessage({ log: t }) };
  if (engine === "plain") {
    // board files as emscripten devices (the fs calls them per byte): collect audio bytes
    // into blocks of 64 frames before handing them over.
    let pend = [];
    opts.preRun = [() => {
      const FS = opts.FS, out = (kind) => (c) => {
        if (kind === 0) { pend.push(c); if (pend.length >= 256) { host.write(0, new Uint8Array(pend)); pend = []; } }
        else host.write(kind, new Uint8Array([c]));
      };
      FS.createDevice("/", "dac1.raw", null, out(0));
      FS.createDevice("/", "dac2.raw", null, () => {});
      FS.createDevice("/", "lcd.txt", null, out(2));
      FS.createDevice("/", "midiout.txt", null, out(3));
      FS.createDevice("/", "live.in", () => host.readByte(), null);
      FS.createDevice("/", "snapshot.ready", null, out(6));
      files(FS);                              // the board's constructor runs before main here
    }];
    opts.locateFile = (f) => "plain/" + f;
    mod = await LXRPLAIN(opts);
  } else {
    opts.hostWrite = (kind, p, n) => host.write(kind, mod.HEAPU8.slice(p, p + n));
    opts.hostRead = (kind, p, n) => {
      if (kind !== 4) return 0;
      let k = 0, x;
      while (k < n && (x = host.readByte()) !== null) mod.HEAPU8[p + k++] = x;
      return k;
    };
    mod = await LXR(opts);
  }
  if (engine !== "plain") files(mod.FS);
  postMessage({ log: `firmware starting (${engine})` });
  try { mod.callMain([]); } catch (e) { postMessage({ log: "stopped: " + e }); }
};
