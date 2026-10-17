// test_iconmatch.js: src/cast/iconmatch.js against real 5mx screens.
//
//   node hosttest/test_iconmatch.js [PACK] [--screens DIR] [--scan] [--preview OUTDIR]
//
// PACK is the icon pack (default src/cast/s7icons.json, or $S7ICONS). The
// screens are the emulator's screenshots (640x240 greyscale PNGs, white =
// 153: grey = round(v / 153 * 15)), default ../../build/emu. Without a pack
// or the screens the test is skipped, not failed.
//
// The checks: each screen in EXPECT shows the icons listed at the places
// listed, and screens with no icons on them give no hits. --scan prints
// every hit on every screen found (to grow EXPECT); --preview writes
// before/after PNGs with the "Series 7" theme.
"use strict";
const fs = require("fs"), path = require("path"), zlib = require("zlib");
const IconMatch = require("../src/cast/iconmatch.js");

const here = __dirname;
const args = process.argv.slice(2);
const opt = (name) => { const i = args.indexOf(name); return i >= 0 ? args.splice(i, 2)[1] : null; };
const flag = (name) => { const i = args.indexOf(name); if (i >= 0) { args.splice(i, 1); return true; } return false; };
const screensDir = opt("--screens") || path.join(here, "..", "..", "..", "build", "emu");
const previewDir = opt("--preview");
const scan = flag("--scan");
const allShots = flag("--all");          // every s-NNN.png too, not only end.png
const packPath = args[0] || process.env.S7ICONS || path.join(here, "..", "src", "cast", "s7icons.json");

// ---- the expected hits: screen, then [icon name, x, y] ----
// (from --scan, each checked by eye on the screen and in --preview)
const EXPECT = {
  // the System screen: file icons, the Extras bar (48-pixel icons, bottoms drawn over by their captions), the toolbar
  "kern1/end.png": [
    ["System/Apps/Agenda/Agenda.aif#48x48", 209, 66],
    ["System/Apps/Bombs/Bombs.aif#48x48", 421, 188],
    ["System/Apps/Data/Data.aif#48x48", 189, 188],
    ["System/Apps/Data/Data.aif#48x48", 209, 8],
    ["System/Apps/Paint/Paint.aif#48x48", 498, 188],
    ["System/Apps/Record/Record.aif#48x48", 575, 188],
    ["System/Apps/Shell/Shell.mbm#26", 30, 8],
    ["System/Apps/Shell/Shell.mbm#4", 575, 105],
    ["System/Apps/Shell/Shell.mbm#6", 575, 68],
    ["System/Apps/Shell/Shell.mbm#8", 575, 31],
    ["System/Apps/Spell/Spell.aif#48x48", 33, 188],
    ["System/Apps/TextEd/TextEd.aif#48x48", 111, 188],
    ["System/Apps/TimeW/TimeW.aif#48x48", 344, 188],
    ["System/Apps/Word/Word.aif#48x48", 30, 66],
  ],
  // Data: its toolbar
  "cast5/end.png": [
    ["System/Apps/Data/Data.mbm#3", 576, 32],
    ["System/Apps/Data/Data.mbm#5", 575, 68],
    ["System/Apps/Data/Data.mbm#7", 575, 105],
    ["System/Apps/Data/Data.mbm#9", 575, 142],
  ],
  // installing a program
  "inst1/end.png": [
    ["System/Apps/InstApp/InstApp.mbm#0", 182, 55],
  ],
  // a file dialog: folder pictures
  "fbword2/end.png": [
    ["System/Data/Eikon.mbm#3", 29, 53],
    ["System/Data/Eikon.mbm#3", 41, 74],
  ],
};
// screens with none of the pictures on them: PsiTerm, PsiMail, PsiWeb,
// PsiLinux, test patterns, menus and dialogs
const NONE = ["pterm/end.png", "ptmenu/end.png", "pmail/end.png", "pmreader/end.png", "pweb/end.png",
  "lxv1/end.png", "greys/end.png", "fillscan_00/end.png", "fillscan_ff/end.png", "rv-menus/end.png", "dm-about/end.png"];

// ---- a small PNG reader: 8-bit greyscale, as the emulator writes ----
function readPng(file) {
  const b = fs.readFileSync(file);
  let p = 8, w = 0, h = 0, depth = 0, type = 0;
  const idat = [];
  while (p < b.length) {
    const len = b.readUInt32BE(p), t = b.toString("ascii", p + 4, p + 8);
    const data = b.subarray(p + 8, p + 8 + len);
    if (t === "IHDR") { w = data.readUInt32BE(0); h = data.readUInt32BE(4); depth = data[8]; type = data[9]; }
    else if (t === "IDAT") idat.push(data);
    else if (t === "IEND") break;
    p += 12 + len;
  }
  const chans = { 0: 1, 2: 3, 4: 2, 6: 4 }[type];
  if (depth !== 8 || !chans) throw new Error(file + ": not an 8-bit PNG");
  const raw = zlib.inflateSync(Buffer.concat(idat)), stride = w * chans;
  const out = new Uint8Array(w * h * chans);
  for (let y = 0; y < h; y++) {
    const f = raw[y * (stride + 1)], src = y * (stride + 1) + 1, o = y * stride;
    for (let x = 0; x < stride; x++) {
      const a = x >= chans ? out[o + x - chans] : 0, up = y ? out[o - stride + x] : 0, c = x >= chans && y ? out[o - stride + x - chans] : 0;
      let v = raw[src + x];
      if (f === 1) v += a; else if (f === 2) v += up; else if (f === 3) v += (a + up) >> 1;
      else if (f === 4) { const pa = Math.abs(up - c), pb = Math.abs(a - c), pc = Math.abs(a + up - 2 * c); v += pa <= pb && pa <= pc ? a : pb <= pc ? up : c; }
      out[o + x] = v & 255;
    }
  }
  const grey = new Uint8Array(w * h);
  for (let i = 0; i < w * h; i++) grey[i] = Math.min(15, Math.round(out[i * chans] / 153 * 15));
  return { w, h, grey };
}

function writePng(file, w, h, rgb) {
  const raw = Buffer.alloc((w * 3 + 1) * h);
  for (let y = 0; y < h; y++) { raw[y * (w * 3 + 1)] = 0; Buffer.from(rgb.buffer, rgb.byteOffset + y * w * 3, w * 3).copy(raw, y * (w * 3 + 1) + 1); }
  const crcT = []; for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; crcT[n] = c >>> 0; }
  const crc = (buf) => { let c = 0xffffffff; for (const x of buf) c = crcT[(c ^ x) & 255] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; };
  const chunk = (t, d) => { const l = Buffer.alloc(4); l.writeUInt32BE(d.length); const td = Buffer.concat([Buffer.from(t), d]); const c = Buffer.alloc(4); c.writeUInt32BE(crc(td)); return Buffer.concat([l, td, c]); };
  const ih = Buffer.alloc(13); ih.writeUInt32BE(w, 0); ih.writeUInt32BE(h, 4); ih[8] = 8; ih[9] = 2;
  fs.writeFileSync(file, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", ih), chunk("IDAT", zlib.deflateSync(raw)), chunk("IEND", Buffer.alloc(0))]));
}

// the viewer's "Series 7" theme (viewer.html THEMES, id "s7")
const S7 = [[0, [0, 0, 0]], [5, [85, 85, 85]], [10, [150, 150, 180]], [12, [150, 150, 180]], [15, [255, 255, 255]]];
function s7rgb(g) {
  let a = S7[0], b = S7[S7.length - 1];
  for (let k = 0; k + 1 < S7.length; k++) if (g >= S7[k][0] && g <= S7[k + 1][0]) { a = S7[k]; b = S7[k + 1]; break; }
  const f = b[0] === a[0] ? 0 : (g - a[0]) / (b[0] - a[0]);
  return [0, 1, 2].map(i => Math.round(a[1][i] + (b[1][i] - a[1][i]) * f));
}

function skip(why) { console.log("test_iconmatch: SKIP (" + why + ")"); process.exit(0); }

if (!fs.existsSync(packPath)) skip("no icon pack at " + packPath);
if (!fs.existsSync(screensDir)) skip("no emulator screens at " + screensDir);
const pack = JSON.parse(fs.readFileSync(packPath, "utf8"));
let t0 = process.hrtime.bigint();
const m = IconMatch.create(pack);
const tLoad = Number(process.hrtime.bigint() - t0) / 1e6;
console.log(`pack: ${m.size} indexed pictures, largest ${m.maxW}x${m.maxH}, index built in ${tLoad.toFixed(1)} ms`);

let fails = 0, checks = 0;
const fail = (s) => { fails++; console.log("FAIL " + s); };

function hitsOn(rel) {
  const s = readPng(path.join(screensDir, rel));
  return { s, hits: m.find(s.grey, s.w, s.h) };
}

for (const rel of Object.keys(EXPECT)) {
  if (!fs.existsSync(path.join(screensDir, rel))) { console.log("(no " + rel + ")"); continue; }
  const { hits } = hitsOn(rel);
  for (const [name, x, y] of EXPECT[rel]) {
    checks++;
    if (!hits.some(h => h.icon.name === name && h.x === x && h.y === y && !h.icon.inverted)) fail(`${rel}: ${name} not found at ${x},${y}`);
  }
  for (const h of hits) {
    checks++;
    if (!EXPECT[rel].some(([n, x, y]) => n === h.icon.name && x === h.x && y === h.y)) fail(`${rel}: unexpected ${h.icon.inverted ? "inverted " : ""}${h.icon.name} at ${h.x},${h.y}`);
  }
}
for (const rel of NONE) {
  if (!fs.existsSync(path.join(screensDir, rel))) continue;
  checks++;
  const { hits } = hitsOn(rel);
  if (hits.length) fail(`${rel}: ${hits.length} hits where none were expected (${hits[0].icon.name})`);
}

// timing: a full-screen scan, the worst case per update
{
  const rel = Object.keys(EXPECT)[0];
  if (fs.existsSync(path.join(screensDir, rel))) {
    const s = readPng(path.join(screensDir, rel));
    for (let i = 0; i < 20; i++) m.find(s.grey, s.w, s.h);
    const n = 200; t0 = process.hrtime.bigint();
    for (let i = 0; i < n; i++) m.find(s.grey, s.w, s.h);
    const full = Number(process.hrtime.bigint() - t0) / 1e6 / n;
    t0 = process.hrtime.bigint();
    for (let i = 0; i < n; i++) m.find(s.grey, s.w, s.h, { x: 300, y: 100, w: 40, h: 20 });
    const small = Number(process.hrtime.bigint() - t0) / 1e6 / n;
    console.log(`timing: full 640x240 scan ${full.toFixed(2)} ms, 40x20 dirty rect ${small.toFixed(3)} ms`);
  }
}

if (scan || previewDir) {
  const all = [];
  for (const d of fs.readdirSync(screensDir)) {
    const dir = path.join(screensDir, d);
    if (!fs.statSync(dir).isDirectory()) continue;
    for (const f of fs.readdirSync(dir)) if (allShots ? /^(end|s-\d+)\.png$/.test(f) : f === "end.png") all.push(d + "/" + f);
  }
  const counts = {};
  for (const rel of all) {
    let r;
    try { r = hitsOn(rel); } catch (e) { continue; }
    if (r.s.w !== 640 || r.s.h !== 240) continue;
    if (scan && r.hits.length) console.log(rel + ": " + r.hits.map(h => `${h.icon.inverted ? "~" : ""}${h.icon.name}@${h.x},${h.y}`).join(" "));
    for (const h of r.hits) counts[h.icon.name] = (counts[h.icon.name] || 0) + 1;
    if (previewDir && r.hits.length) {
      fs.mkdirSync(previewDir, { recursive: true });
      const { w, h, grey } = r.s, before = new Uint8Array(w * h * 3);
      for (let i = 0; i < w * h; i++) { const c = s7rgb(grey[i]); before[3 * i] = c[0]; before[3 * i + 1] = c[1]; before[3 * i + 2] = c[2]; }
      const px = new Uint32Array(w * h);
      m.paint(px, w, r.hits, true);
      const after = new Uint8Array(before);
      for (let i = 0; i < w * h; i++) if (px[i]) { after[3 * i] = px[i] & 255; after[3 * i + 1] = (px[i] >> 8) & 255; after[3 * i + 2] = (px[i] >> 16) & 255; }
      const both = new Uint8Array(w * (2 * h + 4) * 3).fill(255);
      both.set(before, 0); both.set(after, w * (h + 4) * 3);
      writePng(path.join(previewDir, rel.replace(/[\/]/g, "_")), w, 2 * h + 4, both);
    }
  }
  if (scan) console.log("seen: " + JSON.stringify(counts));
}

// ---- the Modern theme's packs (tools/modicons.py) ----
// src/cast/modicons.json, and a local pack (src/cast/modicons.local.json,
// not in the repository) if there is one. Each must load, carry each
// picture's 5mx side exactly as s7icons.json does, or as the spec's sides
// file (tools/modicons/<spec>.sides.json, decoded from the built .aif and
// .mbm files) does for pictures not in it, so it matches the same
// pictures; paint something for each; and have every picture indexed. The
// public pack must also find on the System screen the pictures the
// Series 7 pack finds there, for those it has, and nothing else, and on
// our programs' screens their toolbar pictures.
const MOD_EXPECT = {
  // (more than the Series 7 pack: Comms, which the Series 7 has no icon for)
  "kern1/end.png": [
    ["System/Apps/Comms/Comms.aif#48x48", 33, 138],
  ],
  "pmail/end.png": [
    ["System/Apps/PsiMail/PsiMail.mbm#42", 575, 33],
    ["System/Apps/PsiMail/PsiMail.mbm#46", 575, 107],
    ["System/Apps/PsiMail/PsiMail.mbm#48", 575, 144],
  ],
  "cast10/end.png": [
    ["System/Apps/PsiCast/PsiCast.mbm#0", 575, 33],
    ["System/Apps/PsiCast/PsiCast.mbm#2", 575, 70],          // (a plain black shape)
    ["System/Apps/PsiCast/PsiCast.mbm#4", 576, 108],
    ["System/Apps/PsiCast/PsiCast.mbm#6", 575, 144],         // (black outlines round white)
  ],
  "dm-info/end.png": [
    ["System/Apps/PsiMail/PsiMail.mbm#42", 575, 33],
    ["System/Apps/PsiMail/PsiMail.mbm#44", 575, 70],         // (black outlines round white)
    ["System/Apps/PsiMail/PsiMail.mbm#46", 575, 107],
    ["System/Apps/PsiMail/PsiMail.mbm#48", 575, 144],
  ],
  "ds-info/end.png": [
    ["System/Apps/PsiMail/PsiMail.mbm#0", 18, 135],          // (a folder in the tree)
  ],
};
// every picture of a pack, drawn on a toolbar's dithered grey (15 and 10)
// in rows across synthetic screens, must be found where it is drawn, and
// nothing else (a picture drawn twice in a pack under two names may be
// found as either)
function syntheticCheck(label, mod, mm) {
  const W = 640, H = 240, sides = new Map();
  for (const e of mod.icons) sides.set(e.n, e.w + "x" + e.h + e.g + e.m);
  let todo = mm.icons.filter(ic => !ic.inverted), screens = 0;
  while (todo.length) {
    const grey = new Uint8Array(W * H);
    for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) grey[y * W + x] = (x + y) & 1 ? 10 : 15;
    const placed = [];
    let x = 4, y = 4, rowH = 0;
    while (todo.length) {
      const ic = todo[0];
      if (x + ic.w + 4 > W) { x = 4; y += rowH + 4; rowH = 0; }
      if (y + ic.h + 4 > H) break;
      for (let j = 0; j < ic.w * ic.h; j++) if (ic.mask[j]) grey[(y + ((j / ic.w) | 0)) * W + x + j % ic.w] = ic.grey[j];
      placed.push({ ic, x, y });
      x += ic.w + 4; rowH = Math.max(rowH, ic.h); todo.shift();
    }
    screens++;
    const hits = mm.find(grey, W, H);
    for (const p of placed) {
      checks++;
      const same = sides.get(p.ic.name);
      if (!hits.some(h => h.x === p.x && h.y === p.y && sides.get(h.icon.name) === same)) fail(`${label}: ${p.ic.name} (drawn alone) not found`);
    }
    for (const h of hits) {
      checks++;
      if (!placed.some(p => p.x === h.x && p.y === h.y && sides.get(p.ic.name) === sides.get(h.icon.name))) fail(`${label}: ${h.icon.name} found at ${h.x},${h.y}, where it was not drawn`);
    }
  }
  return screens;
}
function checkModPack(label, modPath, sidesPath, screens) {
  const mod = JSON.parse(fs.readFileSync(modPath, "utf8"));
  const s7 = JSON.parse(fs.readFileSync(path.join(here, "..", "src", "cast", "s7icons.json"), "utf8"));
  const sides = new Map(s7.icons.map(e => [e.n, e]));
  if (fs.existsSync(sidesPath)) for (const [n, e] of Object.entries(JSON.parse(fs.readFileSync(sidesPath, "utf8")))) if (!sides.has(n)) sides.set(n, e);
  checks++;
  if (mod.format !== "psicast-iconpack" || mod.version !== 2 || Buffer.from(mod.palette, "base64").length !== 768) fail(`${label}: not a version 2 icon pack`);
  for (const e of mod.icons) {
    checks++;
    const o = sides.get(e.n);
    if (!o) { fail(`${label}: ${e.n} is in neither s7icons.json nor ${path.basename(sidesPath)}`); continue; }
    if (o.w !== e.w || o.h !== e.h || o.g !== e.g || o.m !== e.m) fail(`${label}: ${e.n}: its 5mx side differs from its source's`);
    const a = Buffer.from(e.a, "base64"), c = Buffer.from(e.c, "base64");
    let na = 0; for (let i = 0; i < e.w * e.h; i++) na += (a[i >> 3] >> (i & 7)) & 1;
    if (!na || na !== c.length) fail(`${label}: ${e.n}: ${na} painted pixels, ${c.length} colours`);
  }
  const mm = IconMatch.create(mod);
  checks++;
  if (mm.size !== mod.icons.length) fail(`${label}: ${mm.size} of ${mod.icons.length} pictures indexed`);
  syntheticCheck(label, mod, mm);
  const have = new Set(mod.icons.map(e => e.n));
  for (const [rel, list, exact] of screens) {
    if (!fs.existsSync(path.join(screensDir, rel))) continue;
    const s = readPng(path.join(screensDir, rel));
    const hits = mm.find(s.grey, s.w, s.h);
    const want = list.filter(([n]) => have.has(n));
    for (const [name, x, y] of want) {
      checks++;
      if (!hits.some(h => h.icon.name === name && h.x === x && h.y === y)) fail(`${label}: ${rel}: ${name} not found at ${x},${y}`);
    }
    checks++;
    if (exact && hits.length !== want.length) fail(`${label}: ${rel}: ${hits.length} hits, ${want.length} expected`);
    // painting: colour inside the pictures' own boxes only
    const px = new Uint32Array(s.w * s.h);
    mm.paint(px, s.w, hits, true);
    let painted = 0, outside = 0;
    for (let i = 0; i < px.length; i++) if (px[i]) {
      painted++;
      const x = i % s.w, y = (i / s.w) | 0;
      if (!hits.some(h => x >= h.x && x < h.x + h.icon.w && y >= h.y + h.r0 && y < h.y + h.r1)) outside++;
    }
    checks++;
    if (want.length && (!painted || outside)) fail(`${label}: ${rel}: ${painted} pixels painted, ${outside} outside the pictures`);
  }
  return { mod, have };
}
{
  const cast = path.join(here, "..", "src", "cast"), specs = path.join(here, "..", "tools", "modicons");
  const modPath = process.env.MODICONS || path.join(cast, "modicons.json");
  if (!fs.existsSync(modPath)) console.log("(no " + modPath + ")");
  else {
    const screens = Object.keys(EXPECT).map(r => [r, EXPECT[r].concat(MOD_EXPECT[r] || []), true])
      .concat(Object.keys(MOD_EXPECT).filter(r => !EXPECT[r]).map(r => [r, MOD_EXPECT[r], false]));
    const { mod, have } = checkModPack("modicons.json", modPath, path.join(specs, "specs.sides.json"), screens);
    const kern = (EXPECT["kern1/end.png"] || []).filter(([n]) => have.has(n)).length;
    console.log(`modicons: ${mod.icons.length} pictures, ${kern} of them on the System screen`);
    checks++;
    if (fs.existsSync(path.join(screensDir, "kern1/end.png")) && kern < 10) fail(`modicons: only ${kern} of the System screen's pictures drawn`);
  }
  // the firmware's copy with the local pictures merged in (tools/mkcasticons.py
  // writes it to casticons_local.h when there is a local pack): every
  // picture of both packs, each with the colours it has in its own pack
  const localH = path.join(cast, "casticons_local.h");
  if (fs.existsSync(localH) && fs.existsSync(path.join(cast, "modicons.local.json"))) {
    const hex = fs.readFileSync(localH, "utf8").match(/kCastModIconsGz\[\] = \{([^}]*)\}/)[1].match(/0x[0-9a-f]{2}/g);
    const merged = JSON.parse(zlib.gunzipSync(Buffer.from(hex.map(h => parseInt(h, 16)))).toString());
    const mm = IconMatch.create(merged), byName = new Map(mm.icons.map(ic => [ic.name, ic]));
    let n = 0;
    for (const f of ["modicons.json", "modicons.local.json"]) {
      const one = IconMatch.create(JSON.parse(fs.readFileSync(path.join(cast, f), "utf8")));
      for (const ic of one.icons) {
        checks++; n++;
        const m2 = byName.get(ic.name);
        if (!m2 || Buffer.compare(Buffer.from(m2.rgba), Buffer.from(ic.rgba)) || Buffer.compare(Buffer.from(m2.grey), Buffer.from(ic.grey))) fail(`casticons_local.h: ${ic.name} not as in ${f}`);
      }
    }
    checks++;
    if (mm.size !== n) fail(`casticons_local.h: ${mm.size} pictures, ${n} expected`);
    console.log(`casticons_local.h: ${mm.size} pictures, as in their packs`);
  }
  const localPath = path.join(cast, "modicons.local.json");
  if (!fs.existsSync(localPath)) console.log("(no local pack: skipped)");
  else {
    const { mod } = checkModPack("modicons.local.json", localPath, path.join(specs, "specs_local.sides.json"), []);
    console.log(`modicons.local: ${mod.icons.length} pictures`);
  }
}

console.log(fails ? `test_iconmatch: ${fails} of ${checks} checks FAILED` : `test_iconmatch: all ${checks} checks passed`);
process.exit(fails ? 1 : 0);
