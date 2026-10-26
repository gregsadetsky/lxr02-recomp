// page: starts the firmware worker, the audio worklet, web midi in, and turns keys and
// pads into live input lines (scenario line format, see 2.engine/scenario/README.md).
const $ = (id) => document.getElementById(id);
const log = (t) => console.log(t);
const VOICES = [["drum1", "s", 36], ["drum2", "d", 37], ["drum3", "f", 38], ["snare", "g", 39],
                ["clap/cym", "h", 40], ["cl hh", "j", 41], ["op hh", "k", 42]];
let send = () => {};
const at = (el, x, y) => { el.style.setProperty("--x", x); el.style.setProperty("--y", y); return el; };
const add = (parent, tag, cls, html = "") => { const e = document.createElement(tag); if (cls) e.className = cls; e.innerHTML = html; parent.appendChild(e); return e; };

// what the browser needs: SharedArrayBuffer (cross origin isolation, which needs https or
// localhost) and AudioWorklet.
const supported = window.isSecureContext && window.crossOriginIsolated &&
  typeof SharedArrayBuffer !== "undefined" && typeof AudioWorkletNode !== "undefined";
if (!supported) { $("msg").textContent = "desktop browser only for now"; $("start").hidden = true; $("engine").hidden = true; }

const pad = {};
for (const [name, key, note] of VOICES) {
  const b = pad[key] = add($("pads"), "button", "", `<b>${key}</b><small>${name}</small>`);
  b.onpointerdown = () => play(key);
}
function play(key) {                  // a note on channel 1; the card's settings map 36..42 to the voices
  const v = VOICES.find((x) => x[1] === key);
  send(`midi 90 ${v[2].toString(16)} 7f`);
  pad[key].classList.add("hit"); setTimeout(() => pad[key].classList.remove("hit"), 120);
}
$("intro").showModal();
$("intro").addEventListener("cancel", (e) => e.preventDefault());   // esc does not skip it

// front panel buttons and their leds: same numbering in the firmware (buttonHandler.h
// ButtonNumbers, ledHandler.h LedNumbers): shift register bit i*8+b, 40 = page down.
// layout and labels as printed on the lxr-02 panel: 16 columns (x = column), rows at
// y (centre of the button cap, in column pitches). page down (bar - 1) is the left arrow.
const STEP = [39, 38, 37, 36, 31, 30, 29, 28, 23, 22, 21, 20, 15, 14, 13, 12];
const SEL = [16, 17, 18, 19, 8, 9, 10, 11];
const STEP_LBL = ["drum1", "drum2", "drum3", "snare", "clp/cym", "cl hh", "op hh", "kit",
                  "morph", "song", "project", "<b>config</b>", "<b>reload</b>", "<b>last step</b>", "<b>fx</b>", "<b>pattern</b>"];
const PANEL = [            // dom order: voice, select, steps, mode rows
  ["voice", [[40, "◀", 8], [0, "▶", 9], ...[1, 2, 3, 4, 5, 6, 7].map((v) =>
    [v, ["drum1", "drum2", "drum3", "snare", "clp/cym", "cl hh", "op hh"][v - 1], 9 + v])], 4.83],
  ["sel", SEL.map((b, k) => [b, ["osc", "aeg", "mod", "fm", "click", "filter", "lfo", "mix"][k], 9 + k]), 5.83],
  ["step", STEP.map((b, k) => [b, STEP_LBL[k], 1 + k]), 6.84],
  ["mode", [[32, "<b>shift</b>", 1], [33, "play", 2], [34, "rec", 3], [35, "copy <b>clr</b>", 4],
            [24, "voice", 5], [25, "perf", 6], [26, "load", 7], [27, "save", 8]], 5.83],
];
const ledEl = {};
for (const [x0, x1] of [[1, 4], [5, 8], [9, 16]]) {     // the bracket lines around the three groups
  const g = $("dev").insertBefore(document.createElement("div"), $("dev").firstChild); g.className = "grp";
  Object.assign(g.style, { left: `calc(var(--u) * ${x0 - 0.45})`, width: `calc(var(--u) * ${x1 - x0 + 0.9})`,
                           top: "calc(var(--u) * 5.47)", height: "calc(var(--u) * .8)" });
}
at(add($("dev"), "span", "txt", "bar"), 8.5, 5.21);
for (const [kind, btns, y] of PANEL) {
  const row = add($("panel"), "div", "row");
  for (const [n, label, x] of btns) {
    const b = at(add(row, "button", `pb ${kind}`), x, y);
    b.dataset.n = n;
    b.innerHTML = (kind === "step" ? `<span class="bz"><i></i></span>` : "<i></i>") + `<span>${label}</span>`;
    if (kind === "step" && x % 4 === 1) b.classList.add("beat");
    ledEl[n] = b.querySelector("i");
    // the firmware scans the buttons from its main loop: a press and release arriving in the
    // same poll would never be seen, so a press is held for at least 40 ms.
    let t0 = 0, down = false;
    const up = () => { if (!down) return; down = false; setTimeout(() => send(`button ${n} 0`), Math.max(0, 40 - (performance.now() - t0))); };
    b.onpointerdown = (e) => { try { b.setPointerCapture(e.pointerId); } catch {} down = true; t0 = performance.now(); send(`button ${n} 1`); };
    b.onpointerup = up; b.onpointercancel = up;
  }
}
function showLeds(bytes) {          // 5 shift register bytes + page down
  for (const n in ledEl) {
    const on = n == 40 ? bytes[5] : (bytes[n >> 3] >> (n & 7)) & 1;
    ledEl[n].classList.toggle("on", !!on);
  }
}

// six faders: adc_buf[8..13], read by adcPots.c adc_checkFaders as (raw>>4)/4096, squared,
// as the volume of voices 1..6. the board starts them at 0xfff0 (fully up). (the panel's
// seventh fader, master volume, is analog after the dac and not modelled.)
["drum1", "drum2", "drum3", "snare", "clp/cym", "hihats"].forEach((name, k) => {
  const f = add($("faders"), "label", "fader", `${name}<input type="range" min="0" max="4095" value="4095">`);
  f.style.setProperty("--x", 10 + k);
  f.querySelector("input").oninput = (e) => send(`adc ${8 + k} ${(+e.target.value) << 4}`);
});

// knob drawing: 12 grip ticks that turn with the knob (endless, so no pointer line).
const ticks = (r0, r1, w, col) => Array.from({ length: 12 }, (_, k) =>
  `<line x1="0" y1="${-r0}" x2="0" y2="${-r1}" transform="rotate(${k * 30})" stroke="${col}" stroke-width="${w}" stroke-linecap="round"/>`).join("");

// the four knobs under the lcd: endless pots with two wipers (triangle tracks 90 degrees
// apart, adc_buf[2i] and [2i+1]); the firmware's adcPots.c turns the pair into an angle and
// every 2 degrees into one step of the parameter shown above the knob. the board starts
// both wipers at mid scale, which reads as 90 degrees.
function wipers(deg) {
  const t = ((deg % 360) + 360) % 360;
  const b = t <= 180 ? t : 360 - t;
  const a = t <= 90 ? 90 + t : t <= 270 ? 270 - t : t - 270;
  const raw = (d) => Math.round((d / 180) * 0xfff) << 4;
  return [raw(a), raw(b)];
}
[1.68, 3.24, 4.85, 6.41].forEach((x, i) => {
  let deg = 90;
  const k = at(add($("knobs"), "div", "knob abs", `<svg viewBox="-50 -50 100 100">
    <circle r="47" fill="#121212" stroke="#2e2e2e" stroke-width="2"/><g>${ticks(37, 44, 3, "#7a7a7a")}</g>
    <circle r="31" fill="url(#capg)"/></svg>`), x, 4.31);
  const g = k.querySelector("g");
  const turn = (d) => {
    if (!d) return;
    deg += Math.max(-30, Math.min(30, d));          // the firmware ignores jumps over ~80 degrees
    g.setAttribute("transform", `rotate(${deg - 90})`);
    const [a, b] = wipers(deg);
    send(`adc ${2 * i} ${a}`); send(`adc ${2 * i + 1} ${b}`);
  };
  let y = null;
  k.onpointerdown = (e) => { y = e.clientY; k.setPointerCapture(e.pointerId); };
  k.onpointermove = (e) => { if (y === null) return; const d = y - e.clientY; if (Math.abs(d) >= 2) { turn(d); y = e.clientY; } };
  k.onpointerup = k.onpointercancel = () => { y = null; };
  k.onwheel = (e) => { e.preventDefault(); turn(e.deltaY < 0 ? 4 : -4); };
});
document.body.insertAdjacentHTML("beforeend", `<svg width="0" height="0" style="position:absolute"><defs>
  <radialGradient id="capg" cx=".4" cy=".35" r=".7"><stop offset="0" stop-color="#d0d0d0"/><stop offset="1" stop-color="#8a8a8a"/></radialGradient></defs></svg>`);

// the data encoder: one detent ("encoder +-1") per 15 degrees of drag around it or per wheel
// notch; the centre cap is the push button (encbutton 1 while held).
{
  const e = at(add($("dev"), "div", "enc abs", `<svg viewBox="-50 -50 100 100">
    <g>${ticks(41, 45, 10, "#2c2c2c")}<circle r="42" fill="#181818"/>${ticks(41, 45, 7, "#181818")}${ticks(31, 36, 2.5, "#6a6a6a")}</g>
    <circle class="cap" r="24" fill="#222" stroke="#3a3a3a"/></svg>`), 8, 2.28);
  at(add($("dev"), "span", "txt", "data"), 8, 3.3);
  const g = e.querySelector("g"), cap = e.querySelector(".cap");
  let rot = 0, acc = 0, a0 = null, wacc = 0;
  const detent = (d) => { send(`encoder ${d}`); rot += 15 * d; g.setAttribute("transform", `rotate(${rot})`); };
  const angle = (ev) => { const r = e.getBoundingClientRect(); return Math.atan2(ev.clientY - r.top - r.height / 2, ev.clientX - r.left - r.width / 2) * 180 / Math.PI; };
  let t0 = 0, pressed = false;
  const release = () => { if (!pressed) return; pressed = false; cap.classList.remove("down");
    setTimeout(() => send("encbutton 0"), Math.max(0, 40 - (performance.now() - t0))); };
  e.onpointerdown = (ev) => {
    e.setPointerCapture(ev.pointerId);
    if (ev.target === cap) { pressed = true; t0 = performance.now(); cap.classList.add("down"); send("encbutton 1"); return; }
    a0 = angle(ev); acc = 0;
  };
  e.onpointermove = (ev) => {
    if (a0 === null) return;
    const a = angle(ev); let d = a - a0; a0 = a;
    if (d > 180) d -= 360; if (d < -180) d += 360;
    acc += d;
    while (acc >= 15) { acc -= 15; detent(1); }
    while (acc <= -15) { acc += 15; detent(-1); }
  };
  e.onpointerup = e.onpointercancel = () => { a0 = null; release(); };
  e.onwheel = (ev) => {
    ev.preventDefault();
    wacc -= ev.deltaY * (ev.deltaMode ? 40 : 1);
    while (wacc >= 50) { wacc -= 50; detent(1); }
    while (wacc <= -50) { wacc += 50; detent(-1); }
  };
}

$("start").onclick = async () => {
  $("start").disabled = true;
  if (!supported) return;
  $("intro").close();
  try {
    const ctx = new AudioContext({ sampleRate: 44100 });   // device: 44140.625 hz, 0.09% slow
    await ctx.audioWorklet.addModule("audio.js");
    const CAP = 1 << 15;
    const ctrl = new SharedArrayBuffer(16), data = new SharedArrayBuffer(CAP * 4);
    const inCtrl = new SharedArrayBuffer(8), inData = new SharedArrayBuffer(1 << 16);
    const node = new AudioWorkletNode(ctx, "ring", { outputChannelCount: [2], processorOptions: { ctrl, data } });
    node.connect(ctx.destination);
    const ic = new Int32Array(inCtrl), id = new Uint8Array(inData), enc = new TextEncoder();
    send = (line) => {
      const b = enc.encode(line + "\n"); let w = Atomics.load(ic, 0);
      for (const x of b) id[w++ % id.length] = x;
      Atomics.store(ic, 0, w);
    };
    const card = await new Response((await fetch("card.img.gz")).body.pipeThrough(new DecompressionStream("gzip"))).arrayBuffer();
    const w = new Worker("worker.js");
    w.onmessage = (e) => {
      const m = e.data;
      if (m.log !== undefined) log(m.log);
      if (m.capture !== undefined) window.__capture = m.capture;
      if (m.lcd !== undefined) {
        const p = m.lcd.split("|");            // "   1133 ms |line 1          |line 2          |"
        if (p.length >= 3) $("lcd").textContent = p[1] + "\n" + p[2];
        const l = m.lcd.trim().split(/\s+/);   // "   6800 ms leds 04 00 01 01 82 00"
        if (l[2] === "leds") showLeds(l.slice(3).map((x) => parseInt(x, 16)));
      }
    };
    $("engine").disabled = true;
    const q = new URLSearchParams(location.search);
    if (q.get("engine")) $("engine").value = q.get("engine");
    const capture = q.get("capture") ? Math.round(+q.get("capture") * 44140) : 0;
    if (q.get("test")) {                     // timed input lines (board queues them by virtual ms)
      const r = await fetch(`test-${q.get("test")}.in`);
    if (!r.ok) throw new Error(`test-${q.get("test")}.in: ${r.status}`);
    for (const l of (await r.text()).split("\n")) if (l.trim()) send(l);
    }
    w.postMessage({ engine: $("engine").value, card, ctrl, data, inCtrl, inData, maxFill: 3072, capture }, [card]);
    const c = new Int32Array(ctrl);
    let under = 0;
    setInterval(() => {
      const u = Atomics.load(c, 2);
      if (u !== under) { under = u; log(`underruns ${u}, buffer ${Atomics.load(c, 0) - Atomics.load(c, 1)} frames`); }
    }, 500);
  } catch (e) {
    $("msg").textContent = "could not start audio here: " + e.message;
    if (!$("intro").open) $("intro").showModal();
    log(e);
  }
};

/* midi in (todo):
$("midi").onclick = async () => {        // asked only on this click
  if (!navigator.requestMIDIAccess) { log("this browser has no web midi"); return; }
  try {
    const midi = await navigator.requestMIDIAccess();
    const hook = (inp) => { inp.onmidimessage = (e) => { if (e.data[0] < 0xf8) send("midi " + [...e.data].map((x) => x.toString(16)).join(" ")); }; };
    midi.inputs.forEach(hook);
    midi.onstatechange = (e) => { if (e.port.type === "input" && e.port.state === "connected") hook(e.port); };
    log(`midi in: ${midi.inputs.size} input(s)`);
    $("midi").disabled = true;
  } catch (e) { log("no web midi: " + e); }
};
*/

document.addEventListener("keydown", (e) => {
  if (e.repeat) return;
  if (pad[e.key]) play(e.key);
  if (e.key === "ArrowLeft") send("encoder -1");
  if (e.key === "ArrowRight") send("encoder 1");
  if (e.key === "Enter") send("encbutton 1");
  if (e.key === "Shift") send("button 32 1");
});
document.addEventListener("keyup", (e) => { if (e.key === "Enter") send("encbutton 0"); if (e.key === "Shift") send("button 32 0"); });
