"""generate parameter / velocity / pitch / density sweep scenarios for a set of kits.

for every kit (.snd) given:
  params-<k>   (k = 0..3)  every sound parameter in a quarter of 0..227: sent as midi cc
                (param p = cc p+1, channel 1) or nrpn (param p >= 128: cc 99/98 = (p-128),
                cc 6 = value), values 0 1 32 64 96 126 127, each followed by all seven
                voices (notes 36..42, rotating velocities, two of them in the same ms),
                then the kit's own value is put back so each parameter is tested alone.
                cc 0 (bank change: loads kits), 6/98/99 (the nrpn mechanism) are skipped.
  vel          each voice at velocities 1..127 (step 9), overlapping.
  dense        all seven voices in the same ms, retriggered every 20 ms for 2 s.
  pitch        card with voice i on channel i+2 answering any note: notes 12..108 per voice.
cards: cards/sweep/<kitname>[-pitch].img (tools/mkcard-kit.sh). scenarios: scenario/<outdir>/.
usage: tools/param-sweep.py <outdir name> kit.snd... ; prints the scenario names.
"""
import os, sys, subprocess, re

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(HERE)
outdir = sys.argv[1]
kits = sys.argv[2:]
os.makedirs(f"scenario/{outdir}", exist_ok=True)
os.makedirs("cards/sweep", exist_ok=True)
VALUES = [0, 1, 32, 64, 96, 126, 127]
VELS = [127, 100, 64, 30, 115, 1, 80]
SKIP_CC = {0, 6, 98, 99}
BOOT = 3000            # project load is done by then (kit-zippe: notes from 6000 work, idle shows menu at ~2.6 s)
names = []


def slug(path):
    return re.sub(r"[^a-z0-9_-]", "_", os.path.basename(path).lower().replace(".snd", ""))


def card(kit, pitch=False):
    name = slug(kit) + ("-pitch" if pitch else "")
    img = f"cards/sweep/{name}.img"
    if not os.path.exists(img):
        env = dict(os.environ, GLO_PITCH="1" if pitch else "0")
        subprocess.run(["tools/mkcard-kit.sh", "cards/blank-fat32-64m.img", kit, img], check=True, env=env, stdout=subprocess.DEVNULL)
    return img


def write(name, header, lines, end):
    path = f"scenario/{outdir}/{name}.txt"
    with open(path, "w") as f:
        f.write("\n".join(header) + "\n")
        f.write("\n".join(f"{t} {l}" for t, l in lines) + "\n")
        f.write(f"{end} end\n")
    names.append(f"{outdir}/{name}")


def voices(t, k):
    """all seven voices from t: rotating velocities, voices 0 and 1 in the same ms."""
    out = []
    for v in range(7):
        vel = VELS[(k + v) % len(VELS)]
        out.append((t + (0 if v < 2 else 6 * v), f"midi 90 {36 + v:02x} {vel:02x}"))
    return out


for kit in kits:
    kb = open(kit, "rb").read()
    base = slug(kit)
    img = card(kit)
    hdr = [f"# sweep for {os.path.basename(kit)}", f"card {img}", "seed c0ffee"]
    # parameters in four chunks
    # cc n sets parameter n-1 (so up to 126), nrpn n sets 128+n: parameter 127 has no midi message
    params = [p for p in range(228) if p != 127 and not (p < 127 and (p + 1) in SKIP_CC)]
    for q in range(4):
        chunk = params[q * len(params) // 4:(q + 1) * len(params) // 4]
        t, L, k = BOOT, [], 0
        for p in chunk:
            orig = kb[8 + p] if 8 + p < len(kb) else 0
            for val in VALUES + [orig]:
                if p < 127:
                    L.append((t, f"midi b0 {p + 1:02x} {val:02x}"))
                else:
                    n = p - 128
                    L.append((t, f"midi b0 63 {n >> 7:02x}"))
                    L.append((t, f"midi b0 62 {n & 0x7f:02x}"))
                    L.append((t, f"midi b0 06 {val:02x}"))
                t += 4
                L += voices(t, k); k += 1
                t += 70
        write(f"{base}-params{q}", hdr, L, t + 500)
    # velocity sweep
    t, L = BOOT, []
    for vel in range(1, 128, 9):
        for v in range(7):
            L.append((t + 3 * v, f"midi 90 {36 + v:02x} {vel:02x}"))
        t += 90
    write(f"{base}-vel", hdr, L, t + 800)
    # dense retriggers
    t, L = BOOT, []
    for r in range(100):
        for v in range(7):
            L.append((t, f"midi 90 {36 + v:02x} {VELS[(r + v) % 7]:02x}"))
        t += 20
    write(f"{base}-dense", hdr, L, t + 800)
    # pitch: voice v on channel v+2 (status 0x91 + v), any note
    pimg = card(kit, pitch=True)
    t, L = BOOT, []
    for note in range(12, 109, 6):
        for v in range(7):
            L.append((t + 4 * v, f"midi {0x91 + v:02x} {note:02x} 64"))
        t += 110
    write(f"{base}-pitch", [f"# pitch sweep for {os.path.basename(kit)}", f"card {pimg}", "seed c0ffee"], L, t + 800)

print("\n".join(names))
