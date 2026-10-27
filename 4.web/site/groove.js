// a saved groove: the sd card sectors that differ from the page's base card (card.img.gz).
// raw format (sent deflated, zlib): "LXRG", version 1, u16 count, count x (u32 sector, 512
// bytes). same as 2.engine/tools/groove-delta.py, which the tests check this against.
// used by the worker (diff), the page (apply) and node (tests).
(function (root) {
  const S = 512, MAGIC = [76, 88, 82, 71, 1];      // "LXRG" 1
  function diff(base, card) {
    if (base.length !== card.length) throw new Error("card size changed");
    const changed = [];
    for (let i = 0, n = base.length / S; i < n; i++) {
      const o = i * S;
      for (let k = 0; k < S; k++) if (base[o + k] !== card[o + k]) { changed.push(i); break; }
    }
    const raw = new Uint8Array(7 + changed.length * (4 + S));
    raw.set(MAGIC, 0);
    raw[5] = changed.length & 255; raw[6] = changed.length >> 8;
    changed.forEach((sec, j) => {
      const o = 7 + j * (4 + S);
      raw[o] = sec & 255; raw[o + 1] = (sec >> 8) & 255; raw[o + 2] = (sec >> 16) & 255; raw[o + 3] = sec >>> 24;
      raw.set(card.subarray(sec * S, sec * S + S), o + 4);
    });
    return raw;
  }
  function apply(base, raw) {                     // returns a new card; throws on anything malformed
    for (let k = 0; k < 5; k++) if (raw[k] !== MAGIC[k]) throw new Error("not a groove");
    const n = raw[5] | (raw[6] << 8);
    if (raw.length !== 7 + n * (4 + S)) throw new Error("groove length");
    const card = new Uint8Array(base);
    for (let j = 0; j < n; j++) {
      const o = 7 + j * (4 + S);
      const sec = (raw[o] | (raw[o + 1] << 8) | (raw[o + 2] << 16) | (raw[o + 3] << 24)) >>> 0;
      if ((sec + 1) * S > card.length) throw new Error("sector out of range");
      card.set(raw.subarray(o + 4, o + 4 + S), sec * S);
    }
    return card;
  }
  const api = { diff, apply };
  if (typeof module !== "undefined") module.exports = api; else root.LXRGroove = api;
})(typeof self !== "undefined" ? self : this);
