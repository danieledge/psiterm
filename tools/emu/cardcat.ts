// tools/emu/cardcat.ts - lists a CF card image's root, or prints one file
// from it (8.3 name, root only), e.g. a log written on the emulated Psion.
//   node --experimental-strip-types tools/emu/cardcat.ts CARD.img [NAME]
// The harness saves the card at the end of a run with --save-card FILE.
import { readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
const REPO = resolve(dirname(fileURLToPath(import.meta.url)), '../..') + '/';
const EMU = process.env.PSION_EMU || resolve(REPO, '../psionEmulators');
const { listRoot, readFileBytes } = await import(EMU + '/frontend/src/lib/fat16.ts');
const [card, name] = process.argv.slice(2);
const img = new Uint8Array(readFileSync(card));
const root = listRoot(img);
if (!name) { for (const e of root) console.log(e.name ?? e, e.size ?? ''); process.exit(0); }
const e = root.find((x: any) => String(x.name).toUpperCase() === name.toUpperCase());
if (!e) { console.error(name + ': not on the card'); process.exit(1); }
process.stdout.write(Buffer.from(readFileBytes(img, e)));
