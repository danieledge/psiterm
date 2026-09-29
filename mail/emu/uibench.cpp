/* uibench.cpp - how long PsiMail's screens take to draw on the Psion: run
 * in the emulator with --count (run_psimail.py --count uibench) */
#include "../ui/pmui.h"
extern "C" {
void* malloc(unsigned int);
void free(void*);
int emu_hc(int op, int a, int b, int c, int d);
}
void* ui_alloc(int n) { return malloc(n); }
void ui_free(void* p) { free(p); }

static unsigned char bits[320 * 240];
static const char* msg =
	"Date: Tue, 29 Sep 2026 21:06:49 +0100\nFrom: The Weekly Psion <news@psion.example>\nTo: dan@example.com\n"
	"Subject: The Weekly Psion - issue 42\n\n"
	"\x01h1The Weekly Psion\n\n\x01pHello \x11" "Dan\x12, here is what's \x13new\x14 this week. Read the \x15" "1\x16" "full story online\x17.\n\n"
	"\x01h2In this issue\n\n\x01l1\x95\x02Series 5mx battery tips\n\x01l1\x95\x02" "A \x11new\x12 email client\n"
	"\x01q1The Psion 5mx keyboard is still the best ever made on a small computer, and this line is long enough to wrap onto a second line.\n"
	"\x01pLorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut enim ad minim veniam.\n"
	"\x01u1 https://example.com/full\n";

extern "C" int main(int argc, char** argv)
	{
	PmCanvas c;
	gfx_init(&c, bits, 640, 240, 320);
	PmUiFolder f[6] = { { "Inbox", 5, 'I', 3, 0 }, { "Drafts", 6, 'D', 0, 0 }, { "Sent", 4, 'S', 0, 0 },
		{ "Archive", 7, 'A', 0, 0 }, { "Trash", 5, 'T', 0, 0 }, { "Outbox", 6, 'O', 2, 0 } };
	PmUiRow rows[8];
	for (int i = 0; i < 8; i++)
		{
		rows[i].from = "Alice Angstrom"; rows[i].flen = 14;
		rows[i].subj = "Cafe plans for tomorrow and the day after"; rows[i].slen = 41;
		rows[i].date = "21:07"; rows[i].dlen = 5;
		rows[i].flags = i & 1 ? KRowUnread : KRowAttach;
		}
	PmUiMailbox m = { "Fastmail", 8, f, 6, 0, 0, 0, "Inbox", 5, "3 unread", 8, rows, 8, 50, 0, 2,
		"none", 4, 0, 0, 0, 1, 0 };
	int which = argc > 1 ? argv[1][0] : 'm';
	emu_hc(407, (int)"mailbox x10", 0, 0, 0);
	for (int k = 0; k < 10; k++) ui_mailbox(&c, &m);
	emu_hc(407, (int)"reader layout x10", 0, 0, 0);
	PmDoc d;
	int len = 0;
	while (msg[len]) len++;
	for (int k = 0; k < 10; k++) { doc_build(&d, msg, len, 640, 0, 0, 0); if (k < 9) doc_free(&d); }
	emu_hc(407, (int)"reader draw x10", 0, 0, 0);
	PmUiReader r;
	char* z = (char*)&r;
	for (unsigned int i = 0; i < sizeof(r); i++) z[i] = 0;
	r.text = msg; r.len = len; r.doc = &d; r.folder = "Inbox"; r.flen = 5; r.position = 3; r.count = 50;
	for (int k = 0; k < 10; k++) ui_reader(&c, &r);
	emu_hc(407, (int)"done", 0, 0, 0);
	(void)which;
	return 0;
	}
