#!/usr/bin/env python3
"""mksuffix.py - Links' public-suffix list (suffix.inc), cut down for the Psion.

Links keeps the whole public-suffix list (9,800 names, 188 KB on the ARM)
to stop sites setting cookies for a whole registry such as "co.uk". This
keeps the names of one or two labels ("uk", "co.uk", "github.io"), which
covers the registries people meet, and drops the long ones (mostly
hosting providers' zones and Japanese city domains).

    mksuffix.py SRC/suffix.inc OUT/suffix.inc
"""
import re, sys

src, out = sys.argv[1:3]
text = open(src, encoding="latin-1").read()
names = re.findall(r'^\t"([^"]*)",$', text, re.M)
keep = [n for n in names if n.lstrip("*!.").count(".") <= 1]
with open(out, "w", encoding="latin-1") as f:
    f.write("/* written by web/links/mksuffix.py from Links' suffix.inc: names of one or two labels */\n\n")
    f.write("static_const const_char_ptr domain_suffix[] = {\n")
    for n in keep:
        f.write('\t"%s",\n' % n)
    f.write("};\n")
print("%s: %d of %d public suffixes" % (out, len(keep), len(names)))
