/* edtest.cpp - checks pmedit.cpp: random typing and deleting in a long text,
 * the quick relayout compared with a full one after every step.
 *   g++ -I. -o edtest edtest.cpp pmedit.cpp pmgfx.cpp pmfonts.cpp && ./edtest */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pmui.h"
#include "pmfonts.h"
void* ui_alloc(int n) { return malloc(n); }
void ui_free(void* p) { free(p); }
int main()
	{
	srand(7);
	PmEditor e, f;
	ed_init(&e, 0, 64000, &KFontR13, 300, 19);
	ed_init(&f, 0, 64000, &KFontR13, 300, 19);
	const char* words[] = { "hello ", "the ", "Psion ", "keyboard ", "is ", "lovely ", "\n", "supercalifragilisticexpialidocious ", "a ", "" };
	int bad = 0;
	for (int step = 0; step < 20000; step++)
		{
		int r = rand() % 10;
		if (r < 5) { const char* w = words[rand() % 9]; ed_insert(&e, w, strlen(w)); }
		else if (r < 7) ed_key(&e, EdBack, 1);
		else if (r < 8) ed_key(&e, EdDel, 1);
		else { static const int k[] = { EdLeft, EdRight, EdUp, EdDown, EdHome, EdEnd, EdDocStart, EdDocEnd }; ed_key(&e, k[rand() % 8], 5); }
		/* compare with a full layout */
		ed_set(&f, e.text, e.len);
		if (f.nlines != e.nlines || memcmp(f.lines, e.lines, e.nlines * sizeof(int)))
			{ if (bad++ < 3) printf("step %d: %d vs %d lines\n", step, e.nlines, f.nlines); }
		if (e.cur < 0 || e.cur > e.len) { printf("cursor out %d\n", step); return 1; }
		}
	printf("len %d lines %d mismatches %d\n", e.len, e.nlines, bad);
	return bad != 0;
	}
