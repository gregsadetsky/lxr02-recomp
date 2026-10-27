"""serves 4.web/site with the cross origin isolation headers sharedarraybuffer needs, and
stores saved grooves: a groove is the sd card sectors that differ from the page's base card
(4.web/site/groove.js), deflated. POST /api/groove stores one (<= 10 KB) and returns its id,
GET /api/groove/<id> returns it. GROOVE_DIR is where they live (a disco volume)."""
import os, re, secrets, struct, zlib
from flask import Flask, abort, jsonify, request, send_from_directory

SITE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "4.web", "site")
GROOVE_DIR = os.environ["GROOVE_DIR"]
os.makedirs(GROOVE_DIR, exist_ok=True)
MAX_BYTES = 10 * 1024                 # deflated
MAX_SECTORS = 1000                    # a groove changes ~15; this bounds what a small upload inflates to
CARD_SECTORS = 64 * 1024 * 1024 // 512
S = 512
ID_RE = re.compile(r"[A-Za-z0-9_-]{8,32}")

app = Flask(__name__, static_folder=None)
app.config["MAX_CONTENT_LENGTH"] = MAX_BYTES


def valid_groove(blob):
    """true if blob inflates to a well formed groove: LXRG, version 1, count, count x (u32 sector < card, 512 bytes)."""
    d = zlib.decompressobj()
    try:
        raw = d.decompress(blob, 7 + MAX_SECTORS * (4 + S) + 1)
    except zlib.error:
        return False
    if d.unconsumed_tail or not d.eof or d.unused_data:
        return False
    if len(raw) < 7 or raw[:5] != b"LXRG\x01":
        return False
    n = struct.unpack_from("<H", raw, 5)[0]
    if n > MAX_SECTORS or len(raw) != 7 + n * (4 + S):
        return False
    return all(struct.unpack_from("<I", raw, 7 + k * (4 + S))[0] < CARD_SECTORS for k in range(n))


@app.post("/api/groove")
def save_groove():
    blob = request.get_data(cache=False)
    if not blob or not valid_groove(blob):
        abort(400)
    gid = secrets.token_urlsafe(9)
    tmp = os.path.join(GROOVE_DIR, f".{gid}.tmp")
    with open(tmp, "wb") as f:
        f.write(blob)
    os.replace(tmp, os.path.join(GROOVE_DIR, f"{gid}.bin"))
    return jsonify(id=gid)


@app.get("/api/groove/<gid>")
def get_groove(gid):
    if not ID_RE.fullmatch(gid):
        abort(404)
    path = os.path.join(GROOVE_DIR, f"{gid}.bin")
    if not os.path.isfile(path):
        abort(404)
    return send_from_directory(GROOVE_DIR, f"{gid}.bin", mimetype="application/octet-stream", max_age=31536000)


@app.get("/")
@app.get("/<path:path>")
def site(path="index.html"):
    return send_from_directory(SITE, path)


@app.after_request
def headers(r):
    r.headers.pop("Content-Encoding", None)   # card.img.gz is unpacked by the page itself
    r.headers["Cross-Origin-Opener-Policy"] = "same-origin"
    r.headers["Cross-Origin-Embedder-Policy"] = "require-corp"
    return r
