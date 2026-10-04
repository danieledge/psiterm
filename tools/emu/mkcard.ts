// tools/emu/mkcard.ts - builds a CF card image for the emulator from this
// checkout's build output.
//   node --experimental-strip-types tools/emu/mkcard.ts psimail [--seed] [out.img]
//   node --experimental-strip-types tools/emu/mkcard.ts psiterm|psiweb [out.img]
// Options: --file CARDPATH=HOSTFILE adds a file, 8.3 names only (e.g. --file
// PSIKEY=id_ed25519 for PsiTerm's key Import); EMU_PKG=DIR takes the app's
// files from DIR instead of pkg/, build/mail-pkg/ or build/web-pkg/.
// Default output: build/emu/card.img. Needs $PSION_EMU (the psionEmulators
// checkout; default ../psionEmulators next to this repo) for its FAT16 writer.
// --seed adds a mailbox and calendar for PsiMail's TESTSEED (tools/emu/seed.py).
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
const HERE = dirname(fileURLToPath(import.meta.url)) + '/';
const REPO = resolve(HERE, '../..') + '/';
const EMU = process.env.PSION_EMU || resolve(REPO, '../psionEmulators');
const { createBlankImage, writeFileAtPath } = await import(EMU + '/frontend/src/lib/fat16.ts');
const args = process.argv.slice(2);
const app = args[0];
const seedOn = args.includes('--seed');
const extra: [string, string][] = [];
for (let i = 1; i < args.length; i++)
	if (args[i] === '--file') { const [d, ...r] = args[i + 1].split('='); extra.push([d, r.join('=')]); args.splice(i, 2); i--; }
const out = args.find((x, i) => i > 0 && x !== '--seed') || REPO + 'build/emu/card.img';
const pkg = (dflt: string) => process.env.EMU_PKG ? resolve(process.env.EMU_PKG) + '/' : dflt;
const img = createBlankImage(16*1024*1024);
const w = (p: string, d: Uint8Array) => { const r = writeFileAtPath(img, p, d); if (!r.ok) throw new Error(p + ': ' + r.reason); };
const t = (s: string) => new Uint8Array(Buffer.from(s, 'latin1'));
const f = (p: string) => new Uint8Array(readFileSync(p));
w('PAD.BIN', new Uint8Array(Number(process.env.EMU_PAD || 9000)));   // (shifts the layout: the harness's CF emulation can wedge on some)
w('SYSTEM/LIBS/ESTLIB.DLL', f(HERE + 'ESTLIB.DLL'));   // the EPOC C library (from the SDK's redistributable stdlib.sis)
if (app === 'psimail') {
	const P = pkg(REPO + 'build/mail-pkg/');
	for (const n of ['psimail.app','psimail.rsc','psimail.exe','psimail.aif','psimail.mbm','pmbutton.exe'])
		w('SYSTEM/APPS/PSIMAIL/' + n.toUpperCase(), f(P + n));
} else if (app === 'psiterm') {
	const P = pkg(REPO + 'pkg/');
	for (const n of ['psiterm.app','psiterm.rsc','psissh.exe','psiterm.aif','psiterm.gdr','psiterm.mbm'])
		w('SYSTEM/APPS/PSITERM/' + n.toUpperCase(), f(P + n));
} else if (app === 'psiweb') {
	const P = pkg(REPO + 'build/web-pkg/');
	for (const n of ['psiweb.app','psiweb.rsc','psiweb.exe','psiweb.aif','psiweb.mbm'])
		w('SYSTEM/APPS/PSIWEB/' + n.toUpperCase(), f(P + n));
} else if (app === 'psikern') {
	// experimental/kernel: the test harness and the driver (never released)
	const P = pkg(REPO + 'build/kernel-pkg/');
	w('SYSTEM/APPS/PSIKT/PSIKT.APP', f(P + 'psikt.app'));
	w('SYSTEM/APPS/PSIKT/PSIKT.RSC', f(P + 'psikt.rsc'));
	w('SYSTEM/LIBS/PSIKERN6.LDD', f(P + 'psikern.ldd'));
} else {
	console.error('usage: mkcard.ts psimail|psiterm|psiweb|psikern [--seed] [out.img]'); process.exit(1);
}
if (seedOn) {
	const A = 'SEEDA0/';    // (TESTSEED copies it to C:\PsiMail\A0)
	w(A+'ACCOUNT.TXT', t('dan@example.com@imap.example.com\n'));
	w(A+'FOLDERS.TXT', t('#PSIMAIL1\nI\t3\t9\tINBOX\tInbox\t/\nA\t0\t120\tArchive\tArchive\t/\nD\t0\t1\tDrafts\tDrafts\t/\nS\t0\t40\tSent\tSent\t/\nJ\t0\t2\tJunk\tJunk\t/\nT\t0\t5\tTrash\tTrash\t/\n-\t1\t4\tWork\tWork\t/\n-\t0\t2\tWork/Projects\tWork/Projects\t/\n'));
	const now = Math.floor(Date.UTC(2026, 8, 30, 12, 0, 0) / 1000);
	// (the index file is oldest first: the app reverses it)
	const rows = [
	 [101,'SB',now-86400*20,2400,'Psion Ltd <news@psion.example>','The Series 5mx is here'],
	 [102,'SAB',now-86400*9,5100,'Alice Smith <alice@example.com>','Lunch on Friday?'],
	 [103,'STB',now-86400*5,88000,'Bob Jones <bob@example.com>','Holiday photos'],
	 [104,'SFB',now-86400*3,3100,'Carol <carol@example.com>','Contract to sign'],
	 [105,'SB',now-86400*2,1200,'Dave <dave@example.com>','Re: the WiRSa'],
	 [106,'B',now-86400,4200,'Eve Adams <eve@example.com>','Minutes from the meeting'],
	 [107,'',now-7200,2200,'Provider <support@example.net>','Your app password'],
	 [108,'T',now-3600,65000,'Grace <grace@example.com>','Quarterly figures'],
	 [109,'S',now-600,900,'Heidi <heidi@example.com>','Tea?'],
	 [110,'SB',now-300,120000,'Psion News <news@psion.example>','New PsiMail shows pictures'],
	 [111,'STB',now-120,60000,'Grace <grace@example.com>','Diagram and a GIF, attached'],
	 [112,'STB',now-60,2300000,'Frank <frank@example.com>','The big scan (and a web picture)'],
	];
	let ix = '#PSIMAIL1\t1\t113\t12\n';
	for (const r of rows) ix += `${r[0]}\t${r[1]}\t${r[2]}\t${r[3]}\t${r[4]}\t${r[5]}\tdan@example.com\t<m${r[0]}@x>\t\n`;
	w('SEED/INDEX.TXT', t(ix));
	// (no 102.TXT: the card writer's directories hold 16 entries)

	// pictures: 110 is HTML with two inline (cid:) pictures, a JPEG photo and a
	// PNG logo, both left as the parts came (.img) for the engine to decode on
	// the Psion; 111 is plain text with a PNG diagram (decoded already: .pmi)
	// and a GIF attached; 112 has a picture too big to fetch unasked, a cid the
	// message hasn't got, and a web picture
	const S = HERE + 'seed/';
	w('SEED/110.TXT', t('#PSIMAIL1\t0\t1\nFrom: Psion News <news@psion.example>\nTo: dan@example.com\nDate: Wed, 30 Sep 2026 11:55\nSubject: New PsiMail shows pictures\n\n\x01h1PsiMail now shows pictures\n\x01pThe logo below is a PNG with transparency, embedded in the HTML with a cid: address:\n\x01iPsiMail logo\x02cid:logo@psi\n\x01pAnd this is a photo, a baseline JPEG of 800 by 600, shrunk to fit and dithered to sixteen greys on the Psion itself:\n\x01iA Series 5mx\x02cid:photo@psi\n\x01pText carries on after the pictures, laid out round them as Word does. Both pictures were decoded by psimail.exe from the parts as they came.\n\x01pRegards,\n\x01pThe PsiMail team\n'));
	w('SEED/110.PIC', t('#PSIMAIL1\n2\t124500\timage/jpeg\t1\tphoto@psi\tpsion.jpg\n3\t4900\timage/png\t1\tlogo@psi\tlogo.png\n'));
	w('SEED/110.ATT', t('2\t124500\tpsion.jpg\timage/jpeg\t1\n3\t4900\tlogo.png\timage/png\t1\n'));
	w('SEED/110_2.IMG', new Uint8Array(readFileSync(S+'photo.jpg')));
	w('SEED/110_3.IMG', new Uint8Array(readFileSync(S+'logo.png')));
	w('SEED/111.TXT', t('#PSIMAIL1\t0\t0\nFrom: Grace <grace@example.com>\nTo: dan@example.com\nDate: Wed, 30 Sep 2026 11:58\nSubject: Diagram and a GIF, attached\n\nHi Dan,\n\nHere is the diagram of how the pieces fit, and a GIF of the machine. They come after the text, with their names.\n\nGrace\n'));
	w('SEED/111.PIC', t('#PSIMAIL1\n2\t3200\timage/png\t1\t\tdiagram.png\n3\t38600\timage/gif\t1\t\tpsion.gif\n'));
	w('SEED/111.ATT', t('2\t3200\tdiagram.png\timage/png\t1\n3\t38600\tpsion.gif\timage/gif\t1\n'));
	w('SEED/111_2.PMI', new Uint8Array(readFileSync(S+'diagram.pmi')));
	w('SEED/111_3.IMG', new Uint8Array(readFileSync(S+'photo.gif')));
	w('SEED/112.TXT', t('#PSIMAIL1\t0\t1\nFrom: Frank <frank@example.com>\nTo: dan@example.com\nDate: Wed, 30 Sep 2026 11:59\nSubject: The big scan (and a web picture)\n\n\x01pThe scan is attached - it is a big one. This picture is on the web, so PsiMail does not fetch it:\n\x01iOur logo\x02http://www.example.com/logo.png\n\x01pAnd this one points at a part the message has not got:\n\x01iMissing\x02cid:nothere@x\n\x01pFrank\n'));
	w('SEED/112.PIC', t('#PSIMAIL1\n2\t2300000\timage/jpeg\t1\t\tscan.jpg\n'));
	w('SEED/112.ATT', t('2\t2300000\tscan.jpg\timage/jpeg\t1\n'));

	// calendar seed: this week, with today, a weekend and a busy day
	const CAL = 'PSIMAIL/CAL/';
	w(CAL+'CALENDARS.TXT', t('#PSICAL1\nwork\t1\tctag1\t/dav/work/\tD\tWork\nhome\t1\tctag2\t/dav/home/\t\tHome\n'));
	const ev = (cal: string, id: string, flags: string, s: string, e: string, alarm: string, sum: string, loc: string) => `${cal}\t/dav/${cal}/${id}.ics\tetag${id}\t\t${flags}\t${s}\t${e}\t${alarm}\t${sum}\t${loc}\n`;
	let evs = '#PSICAL1\n';
	evs += ev('work','a1','','202609281000','202609281100','15','Weekly planning','Room 2');
	evs += ev('home','a2','A','202609290000','202609300000','-1','Bin day','');
	evs += ev('work','a3','','202609300900','202609300930','5','Stand-up','');
	evs += ev('work','a4','R','202609301100','202609301200','15','Design review','Meeting room B');
	evs += ev('home','a5','','202609301230','202609301330','-1','Lunch with Alice','The Kings Arms');
	evs += ev('work','a6','','202609301500','202609301630','10','Contract call with Carol','');
	evs += ev('home','a7','','202609301900','202609302100','30','Band practice','Village hall');
	evs += ev('work','a8','','202610011400','202610011500','15','Dentist','High Street');
	evs += ev('home','a9','A','202610030000','202610050000','-1','Weekend away','Cornwall');
	evs += ev('home','b1','','202610031000','202610031200','60','Coast walk','Padstow');
	evs += ev('home','b2','','202610041800','202610042000','-1','Sunday roast','Mum & Dad');
	evs += ev('work','b3','R','202610071000','202610071100','15','Weekly planning','Room 2');
	evs += ev('work','b4','','202610150900','202610151700','-1','Conference','Birmingham');
	w(CAL+'EVENTS.TXT', t(evs));
}
for (const [d, src] of extra) w(d.toUpperCase(), f(src));
mkdirSync(dirname(out), { recursive: true });
writeFileSync(out, img); console.log(out);
