"""groove.js (page/worker) against 2.engine/tools/groove-delta.py (reference), both ways,
on the real web card + the card groove-edit1 produced, plus random edits and malformed input.
usage: uv run --no-project python tests/groove_cross.py <base.img> <edited.img>"""
import sys, os, subprocess, zlib, random, json, importlib.util, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("gd", os.path.join(HERE, "..", "..", "2.engine", "tools", "groove-delta.py"))
gd = importlib.util.module_from_spec(spec); spec.loader.exec_module(gd)
base = open(sys.argv[1], "rb").read(); edited = open(sys.argv[2], "rb").read()
JS = os.path.join(HERE, "..", "site", "groove.js")
def node(op, *files):
    code = f"""const g = require({json.dumps(JS)}); const fs = require('fs');
      const a = fs.readFileSync(process.argv[1]), b = fs.readFileSync(process.argv[2]);
      try {{ const r = '{op}' === 'diff' ? g.diff(a, b) : g.apply(a, b); fs.writeFileSync(process.argv[3], r); }}
      catch (e) {{ console.log('ERR ' + e.message); process.exit(3); }}"""
    p = subprocess.run(["node", "-e", code, *files], capture_output=True, text=True)
    return p.returncode, p.stdout.strip()
ok = True
def check(name, cond):
    global ok; print(("ok  " if cond else "FAIL") + " " + name); ok &= bool(cond)
with tempfile.TemporaryDirectory() as d:
    B, E, R, O = (os.path.join(d, x) for x in ("base", "edited", "raw", "out"))
    open(B, "wb").write(base)
    cases = [("groove-edit1 card", edited)]
    rnd = random.Random(1)
    for t in range(5):
        c = bytearray(base)
        for _ in range(rnd.randint(1, 40)):
            s = rnd.randrange(len(base) // 512); c[s * 512 + rnd.randrange(512)] ^= 1 + rnd.randrange(255)
        cases.append((f"random edit {t}", bytes(c)))
    cases.append(("no change", base))
    for name, card in cases:
        open(E, "wb").write(card)
        rc, _ = node("diff", B, E, R); raw_js = open(R, "rb").read()
        raw_py = zlib.decompress(gd.make(base, card))
        check(f"{name}: js diff == python make ({len(raw_js)} bytes)", rc == 0 and raw_js == raw_py)
        open(R, "wb").write(raw_py)
        rc, _ = node("apply", B, R, O)
        check(f"{name}: js apply(python delta) == card", rc == 0 and open(O, "rb").read() == card)
        check(f"{name}: python apply(js delta) == card", gd.apply(base, zlib.compress(raw_js)) == card)
    good = zlib.decompress(gd.make(base, edited))
    bad = {"bad magic": b"XXXX" + good[4:], "bad version": good[:4] + b"\x02" + good[5:],
           "truncated": good[:-1], "trailing byte": good + b"\x00",
           "sector out of range": good[:7] + (131072).to_bytes(4, "little") + good[11:]}
    for name, raw in bad.items():
        open(R, "wb").write(raw); rc, out = node("apply", B, R, O)
        check(f"js apply rejects {name} ({out})", rc == 3)
print("ALL OK" if ok else "FAILURES"); sys.exit(0 if ok else 1)
