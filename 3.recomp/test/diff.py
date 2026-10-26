"""compare two tagged fpvec.bin files: per source line, how many results differ, with examples."""
import sys, struct, collections
def recs(path):
    w = struct.unpack("<%dI" % (len(open(path, "rb").read()) // 4), open(path, "rb").read())
    i, inp = 0, ()
    while i < len(w):
        t = w[i]; hi = t >> 28
        if hi == 0xb:
            n = (t >> 24) & 0xf; inp = w[i + 1:i + 1 + n]; i += 1 + n
        elif (t >> 24) == 0xa5: yield t & 0xffffff, inp, (w[i + 1],); i += 2
        elif (t >> 24) == 0xa6: yield t & 0xffffff, inp, (w[i + 1], w[i + 2]); i += 3
        else: raise SystemExit(f"bad tag {t:#x} at word {i}")
src = open(sys.argv[3]).read().split("\n") if len(sys.argv) > 3 else None
bad = collections.defaultdict(list); n = 0
for (la, ia, va), (lb, ib, vb) in zip(recs(sys.argv[1]), recs(sys.argv[2])):
    assert la == lb and ia == ib
    n += 1
    if va != vb: bad[la].append((ia, va, vb))
print(n, "results compared,", sum(len(v) for v in bad.values()), "differ")
for line, v in sorted(bad.items()):
    print(f"line {line}: {len(v)} differ  | {src[line-1].strip()[:90] if src else ''}")
    for ia, va, vb in v[:4]:
        print("   in", " ".join(f"{x:08x}" for x in ia), " qemu", " ".join(f"{x:08x}" for x in va), " recomp", " ".join(f"{x:08x}" for x in vb))
