"""server: groove storage and the site headers."""
import os, struct, zlib, importlib, sys
import pytest


@pytest.fixture()
def client(tmp_path, monkeypatch):
    monkeypatch.setenv("GROOVE_DIR", str(tmp_path / "grooves"))
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    import app as appmod
    appmod = importlib.reload(appmod)
    appmod.app.config["TESTING"] = True
    return appmod.app.test_client(), tmp_path / "grooves"


def groove(sectors):
    raw = b"LXRG\x01" + struct.pack("<H", len(sectors)) + b"".join(struct.pack("<I", s) + bytes([s & 255]) * 512 for s in sectors)
    return zlib.compress(raw, 9)


def test_roundtrip(client):
    c, d = client
    blob = groove([1, 32, 2048, 2074])
    r = c.post("/api/groove", data=blob)
    assert r.status_code == 200
    gid = r.get_json()["id"]
    g = c.get(f"/api/groove/{gid}")
    assert g.status_code == 200 and g.data == blob
    assert g.headers["Content-Type"] == "application/octet-stream"
    assert open(d / f"{gid}.bin", "rb").read() == blob


def test_ids_are_distinct(client):
    c, _ = client
    ids = {c.post("/api/groove", data=groove([5])).get_json()["id"] for _ in range(50)}
    assert len(ids) == 50


def test_empty_groove_is_valid(client):
    c, _ = client
    assert c.post("/api/groove", data=groove([])).status_code == 200


@pytest.mark.parametrize("blob", [
    b"",                                                       # nothing
    b"not zlib at all",                                        # not deflate
    zlib.compress(b"XXXX\x01\x00\x00"),                        # bad magic
    zlib.compress(b"LXRG\x02\x00\x00"),                        # bad version
    zlib.compress(b"LXRG\x01\x01\x00" + b"\x00" * 10),         # truncated sector
    zlib.compress(b"LXRG\x01\x00\x00" + b"x"),                 # trailing byte
    zlib.compress(b"LXRG\x01\x01\x00" + struct.pack("<I", 131072) + b"\x00" * 512),   # sector past the card
    zlib.compress(b"LXRG\x01" + struct.pack("<H", 1001) + (struct.pack("<I", 0) + b"\x00" * 512) * 1001),  # too many sectors
    groove([1])[:-3],                                          # cut deflate stream
    groove([1]) + b"junk",                                     # data after the stream
], ids=["empty", "not-zlib", "magic", "version", "truncated", "trailing", "range", "too-many", "cut", "after-stream"])
def test_rejects_malformed(client, blob):
    c, d = client
    assert c.post("/api/groove", data=blob).status_code == 400
    assert not any(d.iterdir())


def test_rejects_over_10kb(client):
    c, d = client
    big = os.urandom(10 * 1024 + 1)
    assert c.post("/api/groove", data=big).status_code == 413
    assert not any(d.iterdir())


def test_bomb_is_refused(client):
    c, d = client
    bomb = zlib.compress(b"LXRG\x01" + struct.pack("<H", 15000) + b"\x00" * (15000 * 516), 9)   # ~7.7 MB inflated
    assert len(bomb) < 10 * 1024
    assert c.post("/api/groove", data=bomb).status_code == 400
    assert not any(d.iterdir())


@pytest.mark.parametrize("gid", ["../app.py", "short", "a" * 40, "bad id!!", "%2e%2e%2fapp"])
def test_bad_ids(client, gid):
    c, _ = client
    assert c.get(f"/api/groove/{gid}").status_code == 404


def test_unknown_id(client):
    c, _ = client
    assert c.get("/api/groove/AAAAAAAAAAAA").status_code == 404


def test_site_headers(client):
    c, _ = client
    r = c.get("/")
    assert r.status_code == 200
    assert r.headers["Cross-Origin-Opener-Policy"] == "same-origin"
    assert r.headers["Cross-Origin-Embedder-Policy"] == "require-corp"
    g = c.get("/card.img.gz")
    assert "Content-Encoding" not in g.headers


def test_requires_groove_dir(monkeypatch):
    monkeypatch.delenv("GROOVE_DIR", raising=False)
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    sys.modules.pop("app", None)
    with pytest.raises(KeyError):
        import app  # noqa: F401
