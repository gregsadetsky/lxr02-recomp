"""drive the page in headless chromium: start it with ?test=zippe&capture=<s>, wait for the
captured audio, compare it with the qemu cortex-m7 run of the same scenario (2.engine
out/qemu/webcard-zippe/dac1.raw), report lcd text, buffer state, timing.
usage: uv run --no-project --with playwright python test_page.py <engine> [seconds]"""
import sys, time, base64, subprocess, os, array
from playwright.sync_api import sync_playwright
HERE = os.path.dirname(os.path.abspath(__file__))
engine = sys.argv[1]; secs = float(sys.argv[2]) if len(sys.argv) > 2 else 11.8
# the page server must already run: uv run --no-project python serve.py 8023
PORT = int(os.environ.get("PORT", "8023"))
try:
    with sync_playwright() as p:
        b = p.chromium.launch(args=["--autoplay-policy=no-user-gesture-required"])
        pg = b.new_page()
        pg.on("console", lambda m: print("console:", m.text))
        pg.goto(f"http://127.0.0.1:{PORT}/?engine={engine}&test=zippe&capture={secs}")
        pg.click("#start")
        t0 = time.time()
        pg.wait_for_function("window.__capture !== undefined", timeout=int(secs * 4000) + 60000)
        dt = time.time() - t0
        b64 = pg.evaluate("""() => { const u = new Uint8Array(window.__capture.buffer); let s = '';
            for (let i = 0; i < u.length; i += 32768) s += String.fromCharCode.apply(null, u.subarray(i, i + 32768)); return btoa(s); }""")
        print("lcd:", repr(pg.inner_text("#lcd"))); print("status:", pg.inner_text("#status"))
        print("log:", pg.inner_text("#log").strip().split("\n")[-3:])
        pg.screenshot(path=os.path.join(HERE, "build", f"page-{engine}.png"))
        b.close()
finally:
    pass
got = base64.b64decode(b64)
ref = open(os.environ.get("REF") or os.path.join(HERE, "..", "2.engine", "out", "qemu", "webcard-zippe", "dac1.raw"), "rb").read()[:len(got)]
a = array.array("h"); a.frombytes(got); r = array.array("h"); r.frombytes(ref)
diff = sum(1 for x, y in zip(a, r) if x != y)
print(f"{engine}: captured {len(a)//2} frames in {dt:.1f} s wall ({len(a)/2/44140/dt:.2f}x the 44.1 kHz audio clock incl. start)")
print(f"{engine}: vs qemu cortex-m7: {diff} of {len(a)} samples differ, max |d| {max((abs(x-y) for x, y in zip(a, r)), default=0)}")
