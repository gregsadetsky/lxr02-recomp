"""a saved groove = the sd card sectors that differ from the page's base card (2.engine/cards/
web.img). format (then deflated, zlib): "LXRG" u8 version=1, u16 count, count x (u32 sector,
512 bytes). the page does the same in js (4.web/site/groove.js); this is the reference and
the test tool.
  groove-delta.py make  base.img card.img out.bin
  groove-delta.py apply base.img in.bin out.img"""
import sys, zlib, struct

S = 512


def make(base, card):
    assert len(base) == len(card)
    ch = [i for i in range(len(base) // S) if base[i * S:(i + 1) * S] != card[i * S:(i + 1) * S]]
    raw = b"LXRG" + bytes([1]) + struct.pack("<H", len(ch)) + b"".join(struct.pack("<I", i) + card[i * S:(i + 1) * S] for i in ch)
    return zlib.compress(raw, 9)


def apply(base, blob):
    raw = zlib.decompress(blob)
    assert raw[:5] == b"LXRG\x01", "not a groove"
    n = struct.unpack_from("<H", raw, 5)[0]
    out = bytearray(base)
    for k in range(n):
        o = 7 + k * (4 + S)
        i = struct.unpack_from("<I", raw, o)[0]
        assert (i + 1) * S <= len(out), "sector out of range"
        out[i * S:(i + 1) * S] = raw[o + 4:o + 4 + S]
    assert len(raw) == 7 + n * (4 + S), "trailing bytes"
    return bytes(out)


if __name__ == "__main__":
    cmd = sys.argv[1]
    base = open(sys.argv[2], "rb").read()
    if cmd == "make":
        blob = make(base, open(sys.argv[3], "rb").read())
        open(sys.argv[4], "wb").write(blob)
        print(len(blob), "bytes")
    else:
        open(sys.argv[4], "wb").write(apply(base, open(sys.argv[3], "rb").read()))
