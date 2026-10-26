"""serves 4.web/site with the cross origin isolation headers sharedarraybuffer needs."""
import os
from flask import Flask, send_from_directory

SITE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "4.web", "site")
app = Flask(__name__, static_folder=None)


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
