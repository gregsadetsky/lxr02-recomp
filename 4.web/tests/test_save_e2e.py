"""save / load a groove through the real page and server, checked against qemu's cortex-m7.

1. page A: edit through the panel (knobs on several voices and pages, steps on two tracks),
   play, stop, press save, wait for the link.
2. the stored delta, applied to the base card with the reference tool (2.engine/tools/
   groove-delta.py), booted on qemu with the zippe test notes: the reference.
3. page B: the link with the same notes: its audio must equal qemu's, sample for sample.
4. the restored kit must differ from zippe (the edits are in it), the pattern must have steps.
5. a second save gives a different link; the plain engine saves too; an unknown link says so.
needs: the page server (app.py) at $BASE (default http://127.0.0.1:8040), qemu, the arm
build in 2.engine/build/arm.  usage: uv run --no-project --with playwright python tests/test_save_e2e.py"""
import os, sys, time, base64, array, subprocess, urllib.request, importlib.util
from playwright.sync_api import sync_playwright

HERE = os.path.dirname(os.path.abspath(__file__)); ENG = os.path.join(HERE, "..", "..", "2.engine")
BASE = os.environ.get("BASE", "http://127.0.0.1:8040")
spec = importlib.util.spec_from_file_location("gd", os.path.join(ENG, "tools", "groove-delta.py"))
gd = importlib.util.module_from_spec(spec); spec.loader.exec_module(gd)
fails = []
def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else "")); cond or fails.append(name)

def center(pg, sel):
    bb = pg.locator(sel).bounding_box(); return bb["x"] + bb["width"] / 2, bb["y"] + bb["height"] / 2

def click_button(pg, n): pg.click(f".pb[data-n='{n}']"); time.sleep(0.15)

def drag_knob(pg, k, dy):
    x, y = center(pg, f".knob >> nth={k}"); pg.mouse.move(x, y); pg.mouse.down()
    for s in range(abs(dy) // 2): pg.mouse.move(x, y - (2 if dy > 0 else -2) * (s + 1)); time.sleep(0.02)
    pg.mouse.up(); time.sleep(0.1)

def edit_and_save(pg, engine):
    pg.goto(f"{BASE}/?engine={engine}"); pg.click("#start"); time.sleep(4)
    for voice, page in ((2, 16), (2, 9), (5, 17)):            # drum2 osc, drum2 filter, clap aeg
        click_button(pg, voice); click_button(pg, page)
        for k, dy in ((0, 30), (2, -24), (3, 40)): drag_knob(pg, k, dy)
    click_button(pg, 1)
    for st in (39, 31, 23, 15): click_button(pg, st)          # drum1 steps 1 5 9 13
    click_button(pg, 4)
    for st in (31, 15): click_button(pg, st)                  # snare steps 5 13
    click_button(pg, 33); time.sleep(2); click_button(pg, 33)  # play, stop
    t0 = time.time(); pg.click("#save")
    pg.wait_for_function("document.getElementById('url').textContent.includes('?g=')", timeout=30000)
    return pg.inner_text("#url"), time.time() - t0, pg.evaluate("window.__groove")

with sync_playwright() as p:
    b = p.chromium.launch(args=["--autoplay-policy=no-user-gesture-required"])
    pg = b.new_page(viewport={"width": 1280, "height": 900})
    errs = []; pg.on("pageerror", lambda e: errs.append(str(e)))
    url, dt, g = edit_and_save(pg, "exact")
    check("page A: save gives a link", "?g=" in url, f"{url}, {dt:.1f} s, {g['bytes']} bytes")
    check("groove is under 10 KB", g["bytes"] <= 10 * 1024, f"{g['bytes']} bytes")
    gid = url.split("?g=")[1]
    pg.click("#save"); pg.wait_for_function(f"!document.getElementById('url').textContent.endsWith('{gid}')", timeout=30000)
    check("a second save gives another link", pg.inner_text("#url") != url)
    check("page A: no errors", not errs, "; ".join(errs))
    b.close()

    blob = urllib.request.urlopen(f"{BASE}/api/groove/{gid}").read()
    base = open(os.path.join(ENG, "cards", "web.img"), "rb").read()
    card = gd.apply(base, blob)
    os.makedirs(os.path.join(ENG, "cards", "sweep"), exist_ok=True)
    open(os.path.join(ENG, "cards", "sweep", "groove-e2e.img"), "wb").write(card)
    notes = [l for l in open(os.path.join(ENG, "scenario", "kit-zippe.txt")).read().splitlines() if l[:1].isdigit()]
    open(os.path.join(ENG, "scenario", "groove-e2e.txt"), "w").write(
        "# written by 4.web/tests/test_save_e2e.py: a groove saved from the page + zippe notes\n"
        "card cards/sweep/groove-e2e.img\nseed c0ffee\n" + "\n".join(notes) + "\n")
    subprocess.run(["./run.sh", "qemu", "groove-e2e"], cwd=ENG, check=True, env=dict(os.environ, NOBUILD="1"), stdout=subprocess.DEVNULL)
    out = os.path.join(ENG, "out", "qemu", "groove-e2e")
    p_ = bytes.fromhex(open(os.path.join(out, "params.txt")).read().replace("\n", " "))
    z = open(os.path.join(ENG, "cards", "src", "web", "00-zippe.snd"), "rb").read()
    changed = sum(1 for i in range(228) if p_[i] != z[8 + i])
    check("restored kit carries the edits", changed > 0, f"{changed} of 228 parameters differ from zippe")
    pat = open(os.path.join(out, "pattern.bin"), "rb").read()
    boot = open(os.path.join(ENG, "out", "qemu", "webcard-zippe", "pattern.bin"), "rb").read()
    check("restored pattern carries the edits", pat != boot, f"{sum(1 for x, y in zip(pat, boot) if x != y)} bytes differ from the base card's pattern")

    b = p.chromium.launch(args=["--autoplay-policy=no-user-gesture-required"])
    pg = b.new_page(); errs = []; pg.on("pageerror", lambda e: errs.append(str(e)))
    pg.goto(f"{url}&engine=exact&test=zippe&capture=11.8"); pg.click("#start")
    pg.wait_for_function("window.__capture !== undefined", timeout=120000)
    got = base64.b64decode(pg.evaluate("""() => { const u = new Uint8Array(window.__capture.buffer); let s = '';
        for (let i = 0; i < u.length; i += 32768) s += String.fromCharCode.apply(null, u.subarray(i, i + 32768)); return btoa(s); }"""))
    ref = open(os.path.join(out, "dac1.raw"), "rb").read()[:len(got)]
    a = array.array("h"); a.frombytes(got); r = array.array("h"); r.frombytes(ref)
    diff = sum(1 for x, y in zip(a, r) if x != y)
    check("page B (the link) plays like qemu booting that card", len(a) == len(r) and diff == 0, f"{diff} of {len(a)} samples differ")
    check("page B: no errors", not errs, "; ".join(errs))
    pg.goto(f"{BASE}/?g=doesnotexist123&engine=exact"); pg.click("#start"); time.sleep(2)
    check("an unknown link says so", "no groove" in pg.inner_text("#msg"), pg.inner_text("#msg"))
    b.close()

    b = p.chromium.launch(args=["--autoplay-policy=no-user-gesture-required"])
    pg = b.new_page(viewport={"width": 1280, "height": 900}); errs = []; pg.on("pageerror", lambda e: errs.append(str(e)))
    url2, dt2, g2 = edit_and_save(pg, "plain")
    check("plain engine: save gives a link", "?g=" in url2, f"{dt2:.1f} s, {g2['bytes']} bytes")
    b.close()
print("ALL OK" if not fails else f"{len(fails)} FAILED"); sys.exit(1 if fails else 0)
