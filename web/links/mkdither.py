#!/usr/bin/env python3
"""mkdither.py - Links' colour tables for PsiWeb's screen, made at build time.

At every start Links' dither.c builds its tables for the screen's pixel
format: for each of red, green and blue, which 565 level a light value
rounds to, and back (make_16_table, compress_tables, make_round_tables).
That takes about 1,000 soft-float pow() calls and three 256 KB scratch
tables: about 30 million ARM instructions (2 seconds on a 5mx) and 768 KB
of heap, every time PsiWeb starts. PsiWeb's settings never change them:
RGB565 (depth 130), display gamma 2.2, user gamma 1.0 and 8-bit gamma
tables (-gamma-correction 0). So this writes the finished tables, computed
the same way in double precision, and dither.c (PSI_EPOC) uses them when
the settings match.

It also writes dip.c's gamma table for pictures in sRGB (every JPEG and
GIF, and PNGs without their own gamma): 768 more pow() calls, about 20
million instructions, at the first picture of a session.

The C file includes it with PSI_WANT_DITHER or PSI_WANT_IMG_GAMMA defined.

    mkdither.py OUT.inc
"""
import struct, sys

GAMMA = 2.2                 # display_red/green/blue_gamma (default.c)
USER_GAMMA = 1.0            # user_gamma
SRGB_GAMMA = 0.45455        # sRGB_gamma (links.h)
INV_65535 = 1 / 65535.
INV_255 = 1 / 255.

def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]

def make_16_table(values, mult):
    """make_16_table() with gamma_bits 0 (a step every 256), then compressed
    to 256 entries as compress_tables() does"""
    grades = values - 1
    rev_gamma = 1 / GAMMA
    out = []
    last_grade, last = -1, 0
    for j in range(0, 65536, 256):
        voltage = (j * INV_65535) ** rev_gamma
        grade = int(voltage * grades + 0.5)
        if grade != last_grade:
            last_grade = grade
            voltage = grade / grades
            light = int(voltage ** GAMMA * 65535 + 0.5)
            light = max(0, min(65535, light))
            last = light | ((grade * mult) << 16)
        out.append(last)
    return out

def ags_8_to_16(a, gamma):
    v = int((a * INV_255) ** gamma * 65535 + 0.5)
    return min(v, 0xffff)

red = make_16_table(1 << 5, 1 << 11)
green = make_16_table(1 << 6, 1 << 5)
blue = make_16_table(1 << 5, 1 << 0)
g = f32(USER_GAMMA / SRGB_GAMMA)
rnd = [ags_8_to_16(a, g) for a in range(256)]
round_r = [red[v >> 8] & 0xffff for v in rnd]
round_g = [green[v >> 8] & 0xffff for v in rnd]
round_b = [blue[v >> 8] & 0xffff for v in rnd]

# make_gamma_table() for 8-bit pictures with cimg->*_gamma = (float)sRGB_gamma
rg = USER_GAMMA / f32(SRGB_GAMMA)
img = [min(65535, int(65535 * (a * INV_255) ** rg + 0.5)) for a in range(256)]

def arr(ctype, name, vals):
    s = "static const %s %s[256] = {" % (ctype, name)
    for i, v in enumerate(vals):
        s += ("\n\t" if i % 8 == 0 else " ") + "0x%x," % v
    return s + "\n};\n"

with open(sys.argv[1], "w") as f:
    f.write("/* written by web/links/mkdither.py: dither.c's tables for RGB565,\n"
            " * display gamma %g, user gamma %g, 8-bit gamma tables */\n" % (GAMMA, USER_GAMMA))
    f.write("#define PSI_BAKED_GAMMA %r\n#define PSI_BAKED_USER_GAMMA %r\n" % (GAMMA, USER_GAMMA))
    f.write("#ifdef PSI_WANT_DITHER\n")
    f.write(arr("int", "psi_red_table", red) + arr("int", "psi_green_table", green) +
            arr("int", "psi_blue_table", blue))
    f.write(arr("unsigned short", "psi_round_red", round_r) + arr("unsigned short", "psi_round_green", round_g) +
            arr("unsigned short", "psi_round_blue", round_b))
    f.write("#endif\n#ifdef PSI_WANT_IMG_GAMMA\n")
    f.write(arr("unsigned short", "psi_img_gamma", img))
    f.write("#endif\n")
