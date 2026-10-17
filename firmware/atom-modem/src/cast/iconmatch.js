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
 */
(function (root) {
  "use strict";
  const RUN = 8;

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
    const icons = [];
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
      if (addIndexable(base)) {
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
    function addIndexable(ic) {
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
            if (breaks <= 1 || changes < 3) continue;
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

    const filter = new Uint32Array(1 << 15);       // a 2^20-bit filter: 128 KB
    const byKey = new Map();
    for (const ic of icons) for (const an of ic.anchors) {
      { const f = (Math.imul(an.key, 0x9e3779b1) >>> 12); filter[f >>> 5] |= 1 << (f & 31); }
      const l = byKey.get(an.key);
      if (l) l.push(an); else byKey.set(an.key, [an]);
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
      if (got < ic.count * CLIP || got < 40) return null;
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
      for (let y = y0; y < y1; y++) {
        const row = y * W;
        let key = 0;
        for (let x = x0; x < x1; x++) {
          key = ((key << 4) | (grey[row + x] & 15)) >>> 0;
          if (x - x0 < RUN - 1) continue;
          const f = Math.imul(key, 0x9e3779b1) >>> 12;
          if (!(filter[f >>> 5] & (1 << (f & 31)))) continue;
          const l = byKey.get(key);
          if (!l) continue;
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
      }
      return prune(hits);
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
