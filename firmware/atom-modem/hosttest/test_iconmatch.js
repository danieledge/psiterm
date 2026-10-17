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

console.log(fails ? `test_iconmatch: ${fails} of ${checks} checks FAILED` : `test_iconmatch: all ${checks} checks passed`);
process.exit(fails ? 1 : 0);
