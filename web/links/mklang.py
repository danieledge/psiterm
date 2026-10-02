#!/usr/bin/env python3
"""mklang.py - Links' language.inc with English only, for the Psion.

Links builds in every translation of its interface (33 languages, 380 KB
of ARM data). PsiWeb shows none of Links' own interface, and the few texts
that reach the screen (status and error messages) are English, as is the
rest of PsiWeb. This keeps the English table and points every language at
it, so language.c and language.h work unchanged.

    mklang.py SRC/language.inc OUT/language.inc
"""
import re, sys

src, out = sys.argv[1:3]
text = open(src, encoding="latin-1").read()
eng = re.search(r"static_const struct translation translation_english \[\] = \{.*?\n\};\n", text, re.S).group(0)
names = re.findall(r"\{ (translation_\w+) \},", text.split("translations [] = {")[1])
with open(out, "w", encoding="latin-1") as f:
    f.write("/* English only: written by web/links/mklang.py from Links' language.inc */\n\n")
    f.write(eng)
    f.write("\nstatic_const struct translation_desc translations [] = {\n")
    for n in names:
        f.write("  { translation_english },\t/* (%s) */\n" % n[len("translation_"):])
    f.write("};\n")
print("%s: English only, %d languages mapped to it" % (out, len(names)))
