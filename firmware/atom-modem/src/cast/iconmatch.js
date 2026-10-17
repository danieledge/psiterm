/* iconmatch.js: find the 5mx's own pictures (application icons, toolbar
 * pictures) in a PsiCast screen and draw the Series 7's colour versions of
 * them in their place, for the viewer's "Series 7" theme. Plain ES2017, no
 * dependencies; also loads in node (module.exports) for the host test.
 *
 * The pack is s7icons.json, made by tools/s7icons.py from a 5mx ROM and a
 * Series 7 ROM (format "psicast-iconpack", version 2): a 256-colour palette
 * and per picture its name, size, 5mx screen greys (0..15, 4 bits each),
 * the pixels the 5mx draws (m) and the pixels to draw in colour (a), and
 * their palette indices (c).
 *
 *   const m = IconMatch.create(pack);          // pack: the parsed JSON
 *   const hits = m.find(grey, W, H, rect);     // rect {x, y, w, h}, optional
 *   m.draw(ctx, hits);                         // onto a 2D context, Psion pixels
 *   m.paint(px, W, hits, little);              // or into a Uint32Array (ImageData)
 *
 * find() returns [{icon, x, y, r0, r1, n}] for the pictures that overlap
 * rect (it looks in rect grown by the largest picture's size; without rect,
 * the whole screen). A picture is matched only on the pixels it draws and
 * exactly: the 5mx draws them with BitBltMasked, a binary mask, and its
 * 4-grey pictures reach the 16-grey screen as 0, 5, 10 and 15 unchanged, so
 * a real one matches pixel for pixel and nothing else does. One allowance:
 * EIKON draws over the bottom of the Extras bar's 48-pixel icons (their
 * captions), so a band of whole rows that matches exactly and holds most of
 * the picture's pixels counts, and only that band (rows r0..r1) is drawn.
 * Not matched: pictures in a faded window (behind a dialog, EIKON greys and
 * dithers them), which come back when the dialog goes.
 *
 * The index: each picture is indexed by up to three runs of 8 drawn pixels
 * along a row (one in each third of its height, each the run with the most
 * greys and changes, never a dither or a plain run, so it is rare elsewhere).
 * Eight greys of 4 bits are one 32-bit number, so each row of the screen is
 * scanned with a rolling 32-bit key; a 2^20-bit filter turns away nearly
 * every position with one array read, and the few left are looked up and
 * checked row by row. About 1 ms for a whole 640x240 screen in desktop
 * Chromium (5 ms at a 4x CPU slowdown), 0.2 ms for a small update.
 * Inverted pictures (opts.inverted) can be indexed too, but no 5mx screen
 * was seen to draw one: the System screen highlights the caption instead.
 *
 * A picture with no such run but one of 8 with three greys and two changes
 * is indexed by that. One with neither (plain black shapes, or black
 * outlines round white: many of our toolbar pictures) is indexed by its
 * shape: the screen's black pixels, a bit each, 12 along a row, in two
 * rows d apart (-8..8; a 24-bit key). Off the picture, a pixel beside a
 * black one it draws must not be black (else its edge would not show), so
 * those count as known bits too (not black), and a run may take them in.
 * Each such picture has an anchor whose first row is an even row of it and
 * one whose first row is odd, so the scan rolls the 12 bits along with the
 * greys only on every other row of the screen (every row if some picture
 * has only one sort), noting with one table read where they are some
 * anchor's first row; only those few places have their second row read.
 * Such a picture is taken only whole (no clipped band), and only if those
 * edge pixels are indeed not black: a black shape is then found where it
 * stands clear of other black, and only there. A pack without them costs
 * nothing more to scan; with them, a whole screen takes about 0.3 ms more.
 */
(function (root) {
  "use strict";
  const RUN = 8;
  const BRUN = 12, BMASK = (1 << BRUN) - 1;        // black-bit runs (shape keys)

  function b64(s) {
    if (typeof atob === "function") {
      const t = atob(s), out = new Uint8Array(t.length);
      for (let i = 0; i < t.length; i++) out[i] = t.charCodeAt(i);
      return out;
    }
    return new Uint8Array(Buffer.from(s, "base64"));
  }

  function create(pack, opts) {
    opts = opts || {};
    const withInverted = !!opts.inverted;          // (not seen on a 5mx: off unless asked for)
    const list = (pack && pack.icons) || [];
    const icons = [], shapeDs = [];
    let shapeEveryRow = false;
    let maxW = 0, maxH = 0;

    const pal = b64((pack && pack.palette) || "");
    if (!pack || pack.format !== "psicast-iconpack" || pack.version !== 2 || pal.length !== 768) return emptyMatcher();
    for (const e of list) {
      const w = e.w | 0, h = e.h | 0, n = w * h;
      if (!w || !h || w > 256 || h > 256) continue;
      const g4 = b64(e.g), m1 = b64(e.m), c8 = b64(e.c), a1 = b64(e.a);
      if (g4.length !== (n + 1) >> 1 || m1.length !== (n + 7) >> 3 || a1.length !== (n + 7) >> 3) continue;
      const grey = new Uint8Array(n), mask = new Uint8Array(n), rgba = new Uint8Array(4 * n);
      let ci = 0;
      for (let i = 0; i < n; i++) {
        grey[i] = (g4[i >> 1] >> ((i & 1) * 4)) & 15;
        mask[i] = (m1[i >> 3] >> (i & 7)) & 1;
        if ((a1[i >> 3] >> (i & 7)) & 1 && ci < c8.length) {
          const c = c8[ci++] * 3;
          rgba[4 * i] = pal[c]; rgba[4 * i + 1] = pal[c + 1]; rgba[4 * i + 2] = pal[c + 2]; rgba[4 * i + 3] = 255;
        }
      }
      const base = { name: e.n, w, h, grey, mask, rgba, inverted: false, canvas: null };
      base.id = icons.length;
      if (addIndexable(base) || addIndexable(base, true) || addShape(base)) {
        icons.push(base);
        if (withInverted) {
          const ig = new Uint8Array(w * h), ir = new Uint8Array(rgba.length);
          for (let i = 0; i < w * h; i++) {
            ig[i] = 15 - grey[i];
            ir[4 * i] = 255 - rgba[4 * i]; ir[4 * i + 1] = 255 - rgba[4 * i + 1];
            ir[4 * i + 2] = 255 - rgba[4 * i + 2]; ir[4 * i + 3] = rgba[4 * i + 3];
          }
          const inv = { name: e.n, w, h, grey: ig, mask, rgba: ir, inverted: true, canvas: null, id: icons.length + 0.5 };
          if (addIndexable(inv)) icons.push(inv);
        }
        if (w > maxW) maxW = w;
        if (h > maxH) maxH = h;
      }
    }

    // the anchor runs (up to one per third of the icon's height, so an icon
    // with its bottom or top drawn over is still found) and the drawn pixels
    // row by row; false if it has no run of RUN drawn pixels at all
    function addIndexable(ic, loose) {
      const { w, h, grey, mask } = ic;
      const bands = [[0, Math.ceil(h / 3)], [Math.ceil(h / 3), Math.ceil(2 * h / 3)], [Math.ceil(2 * h / 3), h]];
      ic.anchors = [];
      for (const [r0, r1] of bands) {
        let best = -1, bx = 0, by = 0;
        for (let y = r0; y < r1; y++) {
          for (let x = 0; x + RUN <= w; x++) {
            let ok = true, changes = 0, distinct = 0;
            for (let k = 0; k < RUN; k++) {
              if (!mask[y * w + x + k]) { ok = false; break; }
              if (k && grey[y * w + x + k] !== grey[y * w + x + k - 1]) changes++;
              distinct |= 1 << grey[y * w + x + k];
            }
            if (!ok) continue;
            // a run that repeats every two pixels, or does but for one pixel (a
            // dither, a hatched border), or has under three changes of grey (a
            // white run with one black pixel) turns up all over the screen:
            // never an anchor
            let breaks = 0;
            for (let k = 2; k < RUN; k++) if (grey[y * w + x + k] !== grey[y * w + x + k - 2]) breaks++;
            if (breaks <= 1 || (changes < 3 && !(loose && changes >= 2 && popcount(distinct) >= 3))) continue;
            const score = popcount(distinct) * 8 + changes;
            if (score > best) { best = score; bx = x; by = y; }
          }
        }
        if (best < 17) continue;                 // (two greys at least)
        let key = 0;
        for (let k = 0; k < RUN; k++) key = ((key << 4) | grey[by * w + bx + k]) >>> 0;
        ic.anchors.push({ ic, ax: bx, ay: by, key });
      }
      if (!ic.anchors.length) return false;
      // drawn pixels by row: rowAt[y]..rowAt[y+1] index px/pg
      const xs = [], gs = [], rowAt = new Int32Array(h + 1);
      let n = 0;
      for (let y = 0; y < h; y++) {
        rowAt[y] = xs.length;
        for (let x = 0; x < w; x++) if (mask[y * w + x]) { xs.push(x); gs.push(grey[y * w + x]); n++; }
      }
      rowAt[h] = xs.length;
      ic.count = n; ic.px = new Int16Array(xs); ic.pg = new Uint8Array(gs); ic.rowAt = rowAt;
      return true;
    }

    // a picture with no rich run: index it by its shape (see the header). Known
    // bits: drawn pixels (1 if black), and undrawn ones beside a drawn black
    // pixel (0: not black). An anchor is a pair of rows d apart with runs of
    // BRUN known bits at the same place, the first changing at least once
    // and at least as often as the second (two rows alike must change three
    // times: an edge or a stroke is everywhere in text); the best of those
    // whose first row is even, and of those whose first row is odd.
    function addShape(ic) {
      const { w, h, grey, mask } = ic;
      const known = new Int8Array(w * h).fill(-1), edge = [];
      for (let i = 0; i < w * h; i++) if (mask[i]) known[i] = grey[i] === 0 ? 1 : 0;
      for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
        const i = y * w + x;
        if (mask[i]) continue;
        if ((x > 0 && mask[i - 1] && !grey[i - 1]) || (x + 1 < w && mask[i + 1] && !grey[i + 1]) ||
            (y > 0 && mask[i - w] && !grey[i - w]) || (y + 1 < h && mask[i + w] && !grey[i + w])) {
          known[i] = 0; edge.push(i);
        }
      }
      // every known run of BRUN bits along each row: its key and its changes
      const runs = new Map();
      for (let y = 0; y < h; y++) for (let x = 0; x + BRUN <= w; x++) {
        let k = 0, ch = 0, ok = true;
        for (let j = 0; j < BRUN; j++) {
          const v = known[y * w + x + j];
          if (v < 0) { ok = false; break; }
          if (j && v !== known[y * w + x + j - 1]) ch++;
          k = (k << 1) | v;
        }
        if (ok) runs.set(y * w + x, { k, ch });
      }
      // (the first row, the one every place is tested on, is the one with
      // more changes: the second may be above it, d < 0)
      const bestOf = [null, null];
      for (const [at, a] of runs) {
        const x = at % w, y = (at / w) | 0;
        for (let d = -8; d <= 8; d++) {
          const b = d && y + d >= 0 && y + d < h ? runs.get((y + d) * w + x) : null;
          if (!b || !a.ch || a.ch < b.ch || a.k === b.k && a.ch < 3) continue;
          // (a d already scanned for is worth a little: fewer to scan)
          // (both rows changing often is what makes a key rare: an edge or a
          // stroke, one or two changes, is everywhere in text)
          const score = 4 * Math.min(a.ch, b.ch) + 2 * a.ch + b.ch + (shapeDs.includes(d) ? 0.5 : 0) - Math.abs(d) * 0.01;
          const par = y & 1, best = bestOf[par];
          if (!best || score > best.score) bestOf[par] = { score, ax: x, ay: y, d, key: ((a.k << BRUN) | b.k) >>> 0 };
        }
      }
      // one anchor with its first row on an even row of the picture and one
      // on an odd row, so the picture is found on whichever rows it stands
      // (one that has only one sort makes the scan look at every row)
      const ans = bestOf.filter(Boolean);
      if (!ans.length) return false;
      if (ans.length < 2) shapeEveryRow = true;
      for (const an of ans) if (!shapeDs.includes(an.d)) shapeDs.push(an.d);
      ic.shape = { anchors: ans, edge };
      ic.anchors = [];
      const xs = [], gs = [], rowAt = new Int32Array(h + 1);
      for (let y = 0; y < h; y++) {
        rowAt[y] = xs.length;
        for (let x = 0; x < w; x++) if (mask[y * w + x]) { xs.push(x); gs.push(grey[y * w + x]); }
      }
      rowAt[h] = xs.length;
      ic.count = xs.length; ic.px = new Int16Array(xs); ic.pg = new Uint8Array(gs); ic.rowAt = rowAt;
      let r = 0;
      while (r < h && rowAt[r + 1] === rowAt[r]) r++;   // (a row it draws in, for tryAt)
      ic.shape.row = r < h ? r : 0;
      return ic.count >= 12;
    }

    const filter = new Uint32Array(1 << 15);       // a 2^20-bit filter: 128 KB
    const byKey = new Map();
    for (const ic of icons) for (const an of ic.anchors) {
      { const f = (Math.imul(an.key, 0x9e3779b1) >>> 12); filter[f >>> 5] |= 1 << (f & 31); }
      const l = byKey.get(an.key);
      if (l) l.push(an); else byKey.set(an.key, [an]);
    }
    // the shape keys: a table of first rows (12 bits: which d each is an
    // anchor's first row for, a bit each), then the whole key looked up
    const firstRow = new Uint32Array(1 << BRUN), byShape = new Map();   // (17 ds at most: -8..8)
    for (const ic of icons) if (ic.shape) for (const an of ic.shape.anchors) {
      firstRow[an.key >>> BRUN] |= 1 << shapeDs.indexOf(an.d);
      const id = (an.d + 16) * 0x1000000 + an.key;
      const l = byShape.get(id);
      if (l) l.push({ ic, an }); else byShape.set(id, [{ ic, an }]);
    }

    // does row r of the icon match the picture with the icon at (ox, oy)?
    function rowOk(ic, grey, W, H, ox, oy, r) {
      const sy = oy + r;
      if (sy < 0 || sy >= H) return false;
      const base = sy * W + ox, px = ic.px, pg = ic.pg;
      for (let i = ic.rowAt[r], e = ic.rowAt[r + 1]; i < e; i++) if ((grey[base + px[i]] & 15) !== pg[i]) return false;
      return true;
    }

    // The icon at (ox, oy) is taken if a band of whole rows round the anchor
    // matches exactly and holds at least CLIP of its drawn pixels: EIKON draws
    // over the bottom of the System screen's Extras bar icons (48-pixel icons
    // in a shorter bar, their captions on top), so the rows below the band are
    // another control's and stay as they are. Only the band is drawn.
    const CLIP = opts.clip || 0.55;
    function tryAt(ic, grey, W, H, ox, oy, ay) {
      if (!rowOk(ic, grey, W, H, ox, oy, ay)) return null;
      let r0 = ay, r1 = ay;
      while (r0 > 0 && rowOk(ic, grey, W, H, ox, oy, r0 - 1)) r0--;
      while (r1 + 1 < ic.h && rowOk(ic, grey, W, H, ox, oy, r1 + 1)) r1++;
      const got = ic.rowAt[r1 + 1] - ic.rowAt[r0];
      if (ic.shape) {
        // a shape: whole, and standing clear of other black
        if (got !== ic.count) return null;
        for (const i of ic.shape.edge) if (!(grey[(oy + ((i / ic.w) | 0)) * W + ox + i % ic.w] & 15)) return null;
      } else if (got < ic.count * CLIP || got < 40) return null;
      // trim blank rows off the band, so the box is the drawn part
      while (r0 < r1 && ic.rowAt[r0 + 1] === ic.rowAt[r0]) r0++;
      while (r1 > r0 && ic.rowAt[r1 + 1] === ic.rowAt[r1]) r1--;
      return { icon: ic, x: ox, y: oy, r0, r1: r1 + 1, n: got };
    }

    function find(grey, W, H, rect) {
      let x0 = 0, y0 = 0, x1 = W, y1 = H;
      if (rect) {
        x0 = Math.max(0, rect.x - maxW); y0 = Math.max(0, rect.y - maxH);
        x1 = Math.min(W, rect.x + rect.w + maxW); y1 = Math.min(H, rect.y + rect.h + maxH);
      }
      const hits = [], seenAt = new Set();
      if (!icons.length || x1 - x0 < RUN) return hits;
      const shapes = byShape.size > 0;
      np = 0;
      for (let y = y0; y < y1; y++) {
        const row = y * W;
        // (the shape index is looked at on every other row: each shape has
        // an anchor on an even row and one on an odd row)
        const sh = shapes && (shapeEveryRow || !(y & 1));
        let key = 0;
        if (sh) {
          // with the shape index: the black bits too, and where a first row is
          let bk = 0;
          for (let x = x0; x < x1; x++) {
            const g = grey[row + x] & 15;
            key = ((key << 4) | g) >>> 0;
            bk = ((bk << 1) | ((g - 1) >>> 31)) & BMASK;
            if (firstRow[bk] !== 0 && x - x0 >= BRUN - 1) note(row + x, bk);
            if (x - x0 < RUN - 1) continue;
            const f = Math.imul(key, 0x9e3779b1) >>> 12;
            if (filter[f >>> 5] & (1 << (f & 31))) lookAt(key, grey, W, H, x, y, x0, y0, x1, y1, hits, seenAt);
          }
        } else {
          for (let x = x0; x < x1; x++) {
            key = ((key << 4) | (grey[row + x] & 15)) >>> 0;
            if (x - x0 < RUN - 1) continue;
            const f = Math.imul(key, 0x9e3779b1) >>> 12;
            if (filter[f >>> 5] & (1 << (f & 31))) lookAt(key, grey, W, H, x, y, x0, y0, x1, y1, hits, seenAt);
          }
        }
      }
      if (np) findShapes(grey, W, H, x0, y0, x1, y1, np, hits, seenAt);
      return prune(hits);
    }

    // the places where a shape's first row is: look at its second row, d
    // rows down, and check the shape there
    let pending = new Int32Array(4096), np = 0;   // (places to look at, reused)
    function note(at, bk) {
      if (np + 2 > pending.length) { const p = new Int32Array(pending.length * 2); p.set(pending); pending = p; }
      pending[np++] = at; pending[np++] = bk;
    }
    // a place where the last 8 greys are some anchor's
    function lookAt(key, grey, W, H, x, y, x0, y0, x1, y1, hits, seenAt) {
      const l = byKey.get(key);
      if (!l) return;
      for (let k = 0; k < l.length; k++) {
        const an = l[k], ic = an.ic, ox = x - (RUN - 1) - an.ax, oy = y - an.ay;
        if (ox < x0 || ox + ic.w > x1) continue;
        const id = ic.id + ":" + ox + ":" + oy;
        if (seenAt.has(id)) continue;
        seenAt.add(id);
        const hit = tryAt(ic, grey, W, H, ox, oy, an.ay);
        if (hit && hit.y + hit.r0 >= y0 && hit.y + hit.r1 <= y1) hits.push(hit);
      }
    }
    function findShapes(grey, W, H, x0, y0, x1, y1, np, hits, seenAt) {
      for (let p = 0; p < np; p += 2) {
        const at = pending[p], first = pending[p + 1], ds = firstRow[first], y = (at / W) | 0, x = at - y * W;
        for (let di = 0; di < shapeDs.length; di++) {
          const d = shapeDs[di];
          if (!(ds & (1 << di)) || y + d >= y1 || y + d < y0) continue;
          let second = 0;
          for (let j = at + d * W - BRUN + 1, e = at + d * W; j <= e; j++) second = (second << 1) | ((grey[j] & 15) === 0);
          const l = byShape.get((d + 16) * 0x1000000 + (((first << BRUN) | second) >>> 0));
          if (!l) continue;
          for (const { ic, an } of l) {
            if (an.d !== d) continue;
            const ox = x - (BRUN - 1) - an.ax, oy = y - an.ay;
            if (ox < x0 || ox + ic.w > x1 || oy < y0 || oy + ic.h > y1) continue;
            const id = (ic.id * 4096 + oy) * 4096 + ox;
            if (seenAt.has(id)) continue;
            seenAt.add(id);
            const hit = tryAt(ic, grey, W, H, ox, oy, ic.shape.row);
            if (hit) hits.push(hit);
          }
        }
      }
    }

    // one picture inside another's box (a small picture that is also part of
    // a bigger one): keep the one with more drawn pixels
    function prune(hits) {
      if (hits.length < 2) return hits;
      hits.sort((a, b) => b.n - a.n);
      const keep = [];
      for (const h of hits) {
        const hx1 = h.x + h.icon.w, hy0 = h.y + h.r0, hy1 = h.y + h.r1;
        let inside = false;
        for (const k of keep) {
          if (h.x >= k.x && hy0 >= k.y + k.r0 && hx1 <= k.x + k.icon.w && hy1 <= k.y + k.r1) { inside = true; break; }
        }
        if (!inside) keep.push(h);
      }
      return keep;
    }

    function canvasFor(ic) {
      if (ic.canvas) return ic.canvas;
      let c;
      if (typeof OffscreenCanvas !== "undefined") c = new OffscreenCanvas(ic.w, ic.h);
      else { c = document.createElement("canvas"); c.width = ic.w; c.height = ic.h; }
      const x = c.getContext("2d"), im = x.createImageData(ic.w, ic.h);
      im.data.set(ic.rgba);
      x.putImageData(im, 0, 0);
      return (ic.canvas = c);
    }

    // onto a canvas: alpha 0 off the mask, so the background stays
    function draw(ctx, hits) {
      for (const h of hits) {
        const n = h.r1 - h.r0;
        ctx.drawImage(canvasFor(h.icon), 0, h.r0, h.icon.w, n, h.x, h.y + h.r0, h.icon.w, n);
      }
    }

    // into the viewer's pixel buffer (Uint32 per pixel, RGBA bytes in memory
    // order, as ImageData; `little` as the viewer works it out)
    function paint(px, W, hits, little) {
      if (little === undefined) little = true;
      for (const h of hits) {
        const ic = h.icon, rgba = ic.rgba;
        for (let y = h.r0; y < h.r1; y++) for (let x = 0; x < ic.w; x++) {
          const i = y * ic.w + x;
          if (!rgba[4 * i + 3]) continue;
          const r = rgba[4 * i], g = rgba[4 * i + 1], b = rgba[4 * i + 2];
          px[(h.y + y) * W + h.x + x] = little ? (255 << 24 | b << 16 | g << 8 | r) >>> 0 : (r << 24 | g << 16 | b << 8 | 255) >>> 0;
        }
      }
    }

    return { find, draw, paint, icons, maxW, maxH, size: icons.length };
  }

  function emptyMatcher() {
    return { find: () => [], draw() {}, paint() {}, icons: [], maxW: 0, maxH: 0, size: 0 };
  }

  function popcount(v) { let n = 0; while (v) { n += v & 1; v >>>= 1; } return n; }

  const api = { create };
  if (typeof module === "object" && module.exports) module.exports = api;
  else root.IconMatch = api;
})(typeof self !== "undefined" ? self : this);
