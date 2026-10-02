#!/usr/bin/env python3
"""gen.py - make rules for NetSurf and its libraries, for PsiWeb.

NetSurf's own build system cannot drive the EPOC toolchain, so
web/netsurf.sh first builds everything once for the PC in the usual way
(which also runs the code generators: CSS property tables, HTML entities,
fonts, images) and logs each project's compile commands. This script turns
those commands into make rules that compile the same files, with the same
include paths and defines, for either target:

    $(O)        object directory
    $(TCC)      compiler
    $(TFLAGS)   target flags (arch, optimisation, compat headers)

Usage: gen.py CMDS_DIR NS_DIR > rules.mk
"""
import os, shlex, sys

cmds_dir, ns = sys.argv[1], os.path.abspath(sys.argv[2])
PROJECTS = ["libwapcaplet", "libparserutils", "libhubbub", "libdom", "libcss",
            "libnsutils", "libnslog", "libnsgif", "libnsbmp", "libnspsl", "libnsfb", "netsurf"]
# sources we never build for PsiWeb
SKIP = {
    "libnsfb": ("src/surface/sdl.c", "src/surface/wld.c", "src/surface/x.c",
                "src/surface/vnc.c", "src/surface/ram.c"),
    "libdom": ("bindings/xml/expat_xmlparser.c", "bindings/xml/libxml_xmlparser.c"),
}
# extra per-project flags
EXTRA = {
    "netsurf": "-DPSIWEB -Dgettimeofday=pwb_gettimeofday -I$(WEB)/fb -I$(WEB)",
    "libnsutils": "-Dgettimeofday=pwb_gettimeofday",
    "libnsfb": "-I$(WEB)/fb",
}

out = sys.stdout
for proj in PROJECTS:
    cwd = os.path.join(ns, proj)
    objs = []
    for line in open(os.path.join(cmds_dir, proj + ".txt")):
        a = shlex.split(line)
        flags, src = [], None
        i = 1
        while i < len(a):
            t = a[i]
            if t == "-c":
                src = a[i + 1]; i += 2; continue
            if t in ("-o", "-MF"):
                i += 2; continue
            if t.startswith("-I"):
                v = t[2:]
                if not v:
                    i += 1; v = a[i]
                if not v.startswith("/"): v = os.path.join(cwd, v)
                v = os.path.normpath(v)
                if v.startswith("/usr"):
                    i += 1; continue
                if "/hinst/" in v or v.endswith("/hinst") or v.endswith("/hinst/include"):
                    v = "$(NSINC)"
                f = "-I" + v
                if f not in flags: flags.append(f)
            elif t.startswith("-D"):
                if t.startswith("-D_ALIGNED"):
                    flags.append("'-D_ALIGNED=__attribute__((aligned))'")
                elif t.startswith("-DNETSURF_LOG_LEVEL"):
                    flags.append("-DNETSURF_LOG_LEVEL=WARNING")
                elif "NETSURF_FB_FONTPATH" in t or "NETSURF_FB_RESPATH" in t:
                    k = t.split("=", 1)[0]
                    flags.append(shlex.quote(k + '=""'))
                else:
                    flags.append(shlex.quote(t))
            i += 1
        if not src or src in SKIP.get(proj, ()):
            continue
        s = os.path.normpath(os.path.join(cwd, src))
        o = "$(O)/" + proj + "/" + src.replace("/", "_")[:-2] + ".o"
        objs.append(o)
        out.write(f"{o}: {s}\n\t@mkdir -p $(dir $@)\n"
                  f"\t@echo '  CC  {proj}/{src}'\n"
                  f"\t@$(TCC) -c $(TFLAGS) {' '.join(flags)} {EXTRA.get(proj, '')} $< -o $@\n")
    out.write(f"OBJS_{proj} := {' '.join(objs)}\n\n")
out.write("NS_LIBS := " + " ".join(p for p in PROJECTS if p != "netsurf") + "\n")
