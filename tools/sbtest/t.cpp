#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
extern "C" {
#include "vterm.h"
}
#include "termsb.h"
static VTerm* vt; static VTermScreen* scr; static TTermSb sb; static long pushed;
static int R = 30, C = 106;
static int cb_push(int cols, const VTermScreenCell* cells, void*) {
	TSbCell line[160]; if (cols > 160) cols = 160;
	for (int i = 0; i < cols; i++) { line[i].iCh = cells[i].chars[0] == (uint32_t)-1 ? 0xFFFF : cells[i].chars[0]; line[i].iGrey = 0x0F; line[i].iFlags = 0; }
	SbPush(&sb, line, cols); pushed++; return 1; }
// absolute line numbering exactly as PsiTerm: screen row r = pushed + r
static int linefn(void*, int line, unsigned int* out, int max) {
	if (line >= pushed) { int r = line - pushed; if (r >= R) return 0;
		for (int c = 0; c < C && c < max; c++) { VTermPos p = { r, c }; VTermScreenCell cell; vterm_screen_get_cell(scr, p, &cell); out[c] = cell.chars[0]; } return C; }
	int cols; const TSbCell* l = SbLine(&sb, pushed - line, &cols); if (!l) return 0;
	for (int c = 0; c < cols; c++) out[c] = l[c].iCh == 0xFFFF ? (unsigned)-1 : l[c].iCh; return cols; }
static int map(unsigned ch) { return ch < 128 ? (int)ch : '?'; }
int main(int argc, char** argv) {
	static TSbCell cells[400 * 160]; static short cols[400];
	SbInit(&sb, cells, cols, 400, 160);
	vt = vterm_new(R, C); vterm_set_utf8(vt, 1); scr = vterm_obtain_screen(vt);
	static VTermScreenCallbacks cb; cb.sb_pushline = cb_push; vterm_screen_set_callbacks(scr, &cb, 0);
	vterm_screen_enable_altscreen(scr, 1); vterm_screen_set_damage_merge(scr, VTERM_DAMAGE_SCROLL); vterm_screen_reset(scr, 1);
	FILE* f = fopen(argv[1], "rb"); static char buf[1 << 22]; size_t n = fread(buf, 1, sizeof buf, f); fclose(f);
	for (size_t i = 0; i < n; i += 97) { vterm_input_write(vt, buf + i, (n - i) < 97 ? n - i : 97); vterm_screen_flush_damage(scr); }
	printf("pushed %ld stored %d\n", pushed, sb.iCount);
	// whole history (stored scrollback + screen) as text
	static unsigned char out[1 << 20];
	int first = pushed - sb.iCount;
	int len = SbSelectionText(linefn, 0, map, first, 0, pushed + R - 1, C - 1, out, sizeof out, '\n');
	out[len] = 0;
	if (argc > 2) {   // seq check: consecutive numbers
		const char* p = (const char*)out; long expect = -1, bad = 0, seen = 0;
		while (*p) { const char* e = strchr(p, '\n'); std::string ln(p, e ? e - p : strlen(p)); char* end; long v = strtol(ln.c_str(), &end, 10);
			if (*end == 0 && ln.size()) { if (expect >= 0 && v != expect) bad++; expect = v + 1; seen++; } if (!e) break; p = e + 1; }
		printf("numbers seen %ld, gaps %ld, last %ld\n", seen, bad, expect - 1);
	}
	// reversed-order selection across the scrollback/screen boundary
	int a = SbSelectionText(linefn, 0, map, pushed + 1, 3, pushed - 2, 1, out, sizeof out, '|'); out[a] = 0;
	printf("boundary selection: [%s]\n", out);
	return 0;
}
