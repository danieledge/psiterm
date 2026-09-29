/* Checks PsiTerm's paint protocol (immediate damage painting + CopyRect
 * scroll blits under VTERM_DAMAGE_SCROLL) against libvterm's own screen. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vterm.h"
#define R 30
#define C 106
static VTerm *vt; static VTermScreen *scr;
static uint32_t fb[R][C];
static int painting, damaged, d0r, d0c, d1r, d1c;
static long blits, cells_drawn;
static uint32_t cellch(int r, int c) { VTermScreenCell cell; VTermPos p = { r, c }; vterm_screen_get_cell(scr, p, &cell); return cell.chars[0] ? cell.chars[0] : ' '; }
static void draw(int r0, int c0, int r1, int c1) { for (int r = r0; r < r1 && r < R; r++) for (int c = c0; c < c1 && c < C; c++) { fb[r][c] = cellch(r, c); cells_drawn++; } }
static int cb_damage(VTermRect a, void *u) {
	if (painting) draw(a.start_row, a.start_col, a.end_row, a.end_col);
	else if (!damaged) { d0r = a.start_row; d0c = a.start_col; d1r = a.end_row; d1c = a.end_col; damaged = 1; }
	else { if (a.start_row < d0r) d0r = a.start_row; if (a.start_col < d0c) d0c = a.start_col; if (a.end_row > d1r) d1r = a.end_row; if (a.end_col > d1c) d1c = a.end_col; }
	return 1; }
static int cb_move(VTermRect d, VTermRect s, void *u) {
	if (!painting) return 0;
	static uint32_t tmp[R][C]; memcpy(tmp, fb, sizeof(fb));
	for (int r = s.start_row; r < s.end_row; r++) for (int c = s.start_col; c < s.end_col; c++)
		fb[r + d.start_row - s.start_row][c + d.start_col - s.start_col] = tmp[r][c];
	blits++; return 1; }
static void begin(void) { painting = 1; if (damaged) { damaged = 0; draw(d0r, d0c, d1r, d1c); } }
static void end(void) { vterm_screen_flush_damage(scr); painting = 0; }
static int check(const char *what) { for (int r = 0; r < R; r++) for (int c = 0; c < C; c++) if (fb[r][c] != cellch(r, c)) { printf("MISMATCH %s at %d,%d fb=%x screen=%x\n", what, r, c, fb[r][c], cellch(r, c)); return 1; } return 0; }
int main(int argc, char **argv) {
	static VTermScreenCallbacks cb; FILE *f; static char buf[1 << 22]; size_t n; int chunk = argc > 2 ? atoi(argv[2]) : 97; int bad = 0;
	vt = vterm_new(R, C); vterm_set_utf8(vt, 1); scr = vterm_obtain_screen(vt);
	cb.damage = cb_damage; if (!getenv("NOBLIT")) cb.moverect = cb_move; vterm_screen_set_callbacks(scr, &cb, 0);
	vterm_screen_enable_altscreen(scr, 1); if (!getenv("NOBLIT")) vterm_screen_set_damage_merge(scr, VTERM_DAMAGE_SCROLL); vterm_screen_reset(scr, 1);
	vterm_screen_flush_damage(scr); draw(0, 0, R, C); damaged = 0;
	f = fopen(argv[1], "rb"); n = fread(buf, 1, sizeof(buf), f); fclose(f);
	unsigned seed = 1;
	for (size_t i = 0; i < n && !bad; ) {
		size_t k = chunk > 0 ? chunk : (seed = seed * 1103515245 + 12345, 1 + (seed >> 16) % 3000);
		if (k > n - i) k = n - i;
		begin(); vterm_input_write(vt, buf + i, k); end(); i += k;
		bad = check(argv[1]);
	}
	printf("%s chunk=%d: %s  (%ld blits, %ld cells painted, %zu bytes)\n", argv[1], chunk, bad ? "FAIL" : "ok", blits, cells_drawn, n);
	return bad;
}
