# Links 2 for PsiWeb: the ARM build (gcc 3.0 Psion 98r2, ESTLIB headers).
# Run inside the psion-build container, from the repository root:
#   tools/docker/psibuild "make -f web/links/epoc.mk -j8 emu"
#
#   exe   build/links/epoc/psiweb.exe: the device engine, linked with ESTLIB,
#         web/engine/pwepoc.cpp (the chunk shared with PsiWeb.app, PsiWeb.log,
#         heartbeats), ssh/psiglue.cpp (modem and Psion Internet) and the
#         updater; web/build.sh packages it into PsiWeb.sis
#   emu   build/links/epoc/psiweb-emu.pe: the same ARM objects with emu
#         stand-ins for EPOC, run by web/links/emu/run_links.py
#   objs  only compile
#
# The sources come from web/links/fetch.sh (build/links). zlib is ssh/zlib,
# TLS is ssh/tls13.c with libtomcrypt, as in the NetSurf PsiWeb build.
TOP     := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
WEB     := $(TOP)/web
LW      := $(WEB)/links
LK      ?= $(TOP)/build/links
SRC     := $(LK)/links-2.30
PNG     := $(LK)/libpng-1.6.43
JPG     := $(LK)/jpeg-9f
ZL      := $(TOP)/ssh/zlib
O       ?= $(LK)/epoc
# Links' fonts cut down by mkfont.py (Latin, 40 px masters, run-length glyphs
# instead of PNGs); FONTC=$(SRC)/font_inc.c builds with the full set
FONT_H  ?= 40
FONTC   ?= $(O)/font_psi.c
# Hinted glyphs pre-rendered by FreeType at the sizes PsiWeb draws at 100%
# zoom (mkstrike.py, DejaVu fonts from the build container; dip.c's
# PSI_STRIKES; psi_hinted_fonts switches them at run time); STRIKES=0 builds
# without them
STRIKES ?= 1
STRIKE_FONTS ?= /usr/share/fonts/truetype/dejavu
ifeq ($(STRIKES),1)
LXFLAGS += -DPSI_STRIKES
endif
# (tests: HINTED_DEFAULT=0 starts with them switched off, as psi_hinted_fonts = 0)
ifneq ($(HINTED_DEFAULT),)
LXFLAGS += -DPSI_HINTED_DEFAULT=$(HINTED_DEFAULT)
endif

SDK     ?= $(PSION_SDK)
E       := $(SDK)/epoc_cpp_sdk/epoc32
REL     := $(E)/release/marm/rel
export PATH := /usr/bin:/bin:$(SDK)/gcc-3.0-psion-98r2-9/bin:$(E)/tools
export TMP := /tmp/
CC      := arm-pe-gcc
AR      := arm-pe-ar
ARCH    := -fomit-frame-pointer -O2 -fno-strict-aliasing -nostdinc -mcpu=arm710 -mapcs-32 -mshort-load-bytes -msoft-float
DEFS    := -D__SYMBIAN32__ -D__PSISOFT32__ -D__GCC32__ -D__EPOC32__ -D__MARM__ -D__EXE__ -DNDEBUG
LIBC    := -I$(E)/include/libc -I$(E)/include
LIBGCC  := $(SDK)/gcc-3.0-psion-98r2-9/lib/gcc-lib/arm-epoc-pe/3.0-psion-98r2/libgcc.a

LINKS_SRCS := af_unix auth bfu block bookmark cache charsets compress connect \
	cookies data default dip dither dns doh drivers error file finger fn_impl ftp \
	font_inc gif html html_gr html_r html_tbl http https img imgcache jpeg jsint kbd language \
	listedit lru mailto main memory menu objreq os_dep png sched select session smb string suffix \
	terminal types url view view_gr xbm
JPG_SRCS := jaricom jcomapi jdapimin jdapistd jdarith jdatasrc jdcoefct jdcolor jddctmgr \
	jdhuff jdinput jdmainct jdmarker jdmaster jdmerge jdpostct jdsample jerror jidctflt \
	jidctfst jidctint jquant1 jquant2 jutils jmemmgr jmemnobs
PNG_SRCS := png pngerror pngget pngmem pngpread pngread pngrio pngrtran pngrutil pngset pngtrans
Z_SRCS   := adler32 crc32 inflate inftrees inffast zutil
LTC     := $(TOP)/ssh/db/libtomcrypt/src
LTC_SRCS := stream/chacha/chacha_crypt stream/chacha/chacha_ivctr32 stream/chacha/chacha_keystream \
	stream/chacha/chacha_setup stream/chacha/chacha_done mac/poly1305/poly1305 hashes/sha2/sha256 hashes/sha2/sha512 \
	misc/zeromem misc/burn_stack misc/crypt/crypt_argchk misc/compare_testvector

LCFLAGS := $(ARCH) $(DEFS) $(LXFLAGS) -w -I$(LW)/epoc -I$(SRC) -I$(O)/inc -I$(PNG) -I$(JPG) -I$(ZL) \
	$(LIBC) -include $(LW)/epoc/psicompat.h
CFLAGS_C := $(ARCH) $(DEFS) -fno-builtin -w $(LIBC)
# LTC_NO_ASM: libtomcrypt's portable byte-wise loads and stores. Otherwise,
# as a little-endian 32-bit target, every LOAD32L/STORE32L in ChaCha20,
# Poly1305 and SHA-256 is a 4-byte memcpy call (-fno-builtin): over a third
# of the cost of decrypting a page.
LTCFLAGS := $(ARCH) $(DEFS) -std=gnu99 -fno-builtin -w -I$(WEB)/tls -I$(WEB)/compat -I$(LTC)/headers $(LIBC) -DLTC_SOURCE -DLTC_NO_ASM

LINKS_OBJS := $(addprefix $(O)/links/,$(LINKS_SRCS:=.o))
ifeq ($(STRIKES),1)
LINKS_OBJS += $(O)/links/strike_psi.o
endif
PSI_OBJS   := $(O)/psi/psi_drv.o $(O)/psi/psi_os.o $(O)/psi/pwgrey.o $(O)/psi/psi_str.o $(O)/psi/psi_digest.o $(O)/psi/pwnet.o \
	$(O)/psi/nsprintf.o $(O)/psi/tls13.o $(O)/psi/x25519.o $(O)/psi/pwrandom.o \
	$(addprefix $(O)/ltc/,$(subst /,__,$(LTC_SRCS:=.o)))
JPG_LIB    := $(O)/libjpeg.a
PNG_LIB    := $(O)/libpng.a
Z_LIB      := $(O)/libz.a
EMU_OBJS   := $(O)/emu/links_rt.o $(O)/emu/emu_hc.o $(O)/emu/emu_back.o $(O)/emu/setjmp.o

all: exe emu
objs: $(LINKS_OBJS) $(PSI_OBJS) $(JPG_LIB) $(PNG_LIB) $(Z_LIB)
emu: $(O)/psiweb-emu.pe

$(O)/inc/pnglibconf.h: $(LW)/epoc/pnglibconf.h
	@mkdir -p $(dir $@)
	cp $< $@
$(O)/inc/jconfig.h: $(JPG)/jconfig.txt
	@mkdir -p $(dir $@)
	cp $< $@
# dither.c's colour tables for PsiWeb's screen, made here instead of at every start
$(O)/inc/dither_psi.inc: $(LW)/mkdither.py
	@mkdir -p $(dir $@)
	python3 $< $@
# the start page (about:welcome, psi_os.c), as a C string
$(O)/inc/welcome.inc: $(LW)/welcome.html
	@mkdir -p $(dir $@)
	python3 -c 'import sys,json; d=open(sys.argv[1]).read(); print("\n".join(json.dumps(l + "\n") for l in d.splitlines()))' $< > $@
HDRS := $(O)/inc/pnglibconf.h $(O)/inc/jconfig.h $(O)/inc/dither_psi.inc $(LW)/epoc/config.h $(LW)/epoc/psicompat.h

$(O)/links/%.o: $(SRC)/%.c $(HDRS) $(SRC)/links.h $(SRC)/cfg.h
	@mkdir -p $(dir $@)
	$(CC) -c $(LCFLAGS) $< -o $@
$(O)/links/main.o: $(SRC)/main.c $(HDRS) $(SRC)/links.h
	@mkdir -p $(dir $@)
	$(CC) -c $(LCFLAGS) -Dmain=links_main $< -o $@
$(O)/font_psi.c: $(SRC)/font_inc.c $(LW)/mkfont.py
	@mkdir -p $(dir $@)
	python3 $(LW)/mkfont.py $< $@ --height $(FONT_H) --rle
# English only (mklang.py): language.c is compiled from a copy next to the
# cut-down language.inc, as it includes "language.inc" from its own folder
$(O)/lang/language.inc: $(SRC)/language.inc $(LW)/mklang.py
	@mkdir -p $(dir $@)
	python3 $(LW)/mklang.py $< $@
$(O)/lang/language.c: $(SRC)/language.c
	@mkdir -p $(dir $@)
	cp $< $@
$(O)/links/language.o: $(O)/lang/language.c $(O)/lang/language.inc $(HDRS) $(SRC)/links.h
	@mkdir -p $(dir $@)
	$(CC) -c $(LCFLAGS) $< -o $@
# a shorter public-suffix list (mksuffix.py), the same way
$(O)/sfx/suffix.inc: $(SRC)/suffix.inc $(LW)/mksuffix.py
	@mkdir -p $(dir $@)
	python3 $(LW)/mksuffix.py $< $@
$(O)/sfx/suffix.c: $(SRC)/suffix.c $(SRC)/suffix_x.inc
	@mkdir -p $(dir $@)
	cp $(SRC)/suffix.c $(SRC)/suffix_x.inc $(dir $@)
$(O)/links/suffix.o: $(O)/sfx/suffix.c $(O)/sfx/suffix.inc $(HDRS) $(SRC)/links.h
	@mkdir -p $(dir $@)
	$(CC) -c $(LCFLAGS) $< -o $@
$(O)/links/font_inc.o: $(FONTC) $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) -c $(LCFLAGS) -O0 $< -o $@
$(O)/strike_psi.c: $(LW)/mkstrike.py
	@mkdir -p $(dir $@)
	python3 $< $@ --fontdir $(STRIKE_FONTS)
$(O)/links/strike_psi.o: $(O)/strike_psi.c $(HDRS) $(SRC)/links.h
	@mkdir -p $(dir $@)
	$(CC) -c $(LCFLAGS) -O0 $< -o $@

$(O)/psi/psi_drv.o: $(LW)/psi_drv.c $(HDRS) $(WEB)/fb/pwback.h $(SRC)/links.h $(WEB)/psiweb.h $(TOP)/ssh/psishared.h
	@mkdir -p $(dir $@)
	$(CC) -c $(LCFLAGS) -I$(WEB)/fb -I$(WEB) -I$(TOP)/ssh $< -o $@
$(O)/psi/psi_os.o: $(LW)/epoc/psi_os.c $(HDRS) $(O)/inc/welcome.inc $(WEB)/engine/pwnet.h $(SRC)/links.h $(WEB)/psiweb.h $(TOP)/ssh/psishared.h
	@mkdir -p $(dir $@)
	$(CC) -c $(LCFLAGS) -I$(WEB)/fb -I$(WEB) -I$(WEB)/engine -I$(TOP)/ssh $< -o $@
$(O)/psi/pwgrey.o: $(LW)/psi_grey.c $(WEB)/fb/pwback.h $(TOP)/ssh/psigrey.h
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS_C) -std=gnu99 -I$(WEB)/fb $< -o $@
$(O)/psi/psi_str.o: $(LW)/psi_str.c
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS_C) $< -o $@
$(O)/psi/psi_digest.o: $(LW)/psi_digest.c
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS_C) $< -o $@
$(O)/psi/nsprintf.o: $(WEB)/compat/nsprintf.c
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS_C) -std=gnu99 -I$(WEB)/compat $< -o $@
$(O)/psi/pwnet.o: $(WEB)/engine/pwnet.c $(WEB)/psiweb.h $(TOP)/ssh/psishared.h $(WEB)/engine/pwnet.h $(WEB)/psiweb.h
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS_C) -std=gnu99 -I$(TOP)/ssh -I$(WEB)/compat -include $(WEB)/compat/nscompat.h $< -o $@
$(O)/psi/tls13.o: $(TOP)/ssh/tls13.c $(WEB)/tls/includes.h
	@mkdir -p $(dir $@)
	$(CC) -c $(LTCFLAGS) -I$(TOP)/ssh/db/src $< -o $@
$(O)/psi/%.o: $(WEB)/tls/%.c $(WEB)/tls/includes.h $(WEB)/tls/options.h
	@mkdir -p $(dir $@)
	$(CC) -c $(LTCFLAGS) -I$(TOP)/ssh/db/src $< -o $@
$(O)/ltc/%.o:
	@mkdir -p $(dir $@)
	$(CC) -c $(LTCFLAGS) $(LTC)/$(subst __,/,$*).c -o $@

$(O)/jpeg/%.o: $(JPG)/%.c $(O)/inc/jconfig.h
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS_C) -I$(O)/inc -I$(JPG) $< -o $@
$(O)/png/%.o: $(PNG)/%.c $(O)/inc/pnglibconf.h
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS_C) -DPNG_ARM_NEON_OPT=0 -I$(LW)/epoc -I$(O)/inc -I$(PNG) -I$(ZL) $< -o $@
$(O)/zlib/%.o: $(ZL)/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS_C) -I$(WEB)/compat -I$(ZL) $< -o $@

$(JPG_LIB): $(addprefix $(O)/jpeg/,$(JPG_SRCS:=.o))
	rm -f $@; $(AR) rcs $@ $^
$(PNG_LIB): $(addprefix $(O)/png/,$(PNG_SRCS:=.o))
	rm -f $@; $(AR) rcs $@ $^
$(Z_LIB): $(addprefix $(O)/zlib/,$(Z_SRCS:=.o))
	rm -f $@; $(AR) rcs $@ $^

# emulator stand-ins for EPOC (web/emu: the NetSurf harness's own files,
# used as they are where possible)
$(O)/emu/emu_hc.o: $(WEB)/emu/emu_hc.c
	@mkdir -p $(dir $@)
	$(CC) -c $(ARCH) -fno-builtin -w -D__EPOC32__ $< -o $@
$(O)/emu/emu_back.o: $(LW)/emu/emu_back.c $(WEB)/psiweb.h $(TOP)/ssh/psishared.h $(WEB)/fb/pwback.h
	@mkdir -p $(dir $@)
	$(CC) -c $(ARCH) -fno-builtin -w -D__EPOC32__ -I$(WEB) -I$(TOP)/ssh -I$(E)/include/libc $< -o $@
$(O)/emu/links_rt.o: $(LW)/emu/links_rt.c
	@mkdir -p $(dir $@)
	$(CC) -c $(ARCH) -fno-builtin -w -D__EPOC32__ -I$(E)/include/libc $< -o $@
$(O)/emu/setjmp.o: $(LW)/emu/setjmp.s
	@mkdir -p $(dir $@)
	arm-pe-as $< -o $@

$(O)/psiweb-emu.pe: $(LINKS_OBJS) $(PSI_OBJS) $(EMU_OBJS) $(JPG_LIB) $(PNG_LIB) $(Z_LIB)
	arm-pe-ld -o $@ -e main -Map $(O)/psiweb-emu.map $(LINKS_OBJS) $(PSI_OBJS) $(EMU_OBJS) \
		$(PNG_LIB) $(JPG_LIB) $(Z_LIB) $(LIBGCC)
	arm-pe-nm -n $@ > $(O)/psiweb-emu.syms
	@ls -la $@

# ---------- the device EXE (as web/Makefile links NetSurf's) ----------
CXXFLAGS := $(ARCH) $(DEFS) -Wno-ctor-dtor-privacy -fcheck-new -fvtable-thunks -w -I$(WEB) -I$(TOP)/ssh $(LIBC)
DEV_OBJS := $(O)/dev/pwepoc.o $(O)/dev/psiglue.o $(O)/dev/pwupdate.o $(O)/dev/psi_heap.o $(O)/dev/psi_stubs.o
SYSLIBS := $(REL)/estlib.lib $(REL)/euser.lib $(REL)/c32.lib $(REL)/efsrv.lib $(REL)/esock.lib $(REL)/insock.lib $(REL)/nifman.lib $(LIBGCC)
EXE_OBJS = $(REL)/eexe.o $(LINKS_OBJS) $(PSI_OBJS) $(DEV_OBJS) $(PNG_LIB) $(JPG_LIB) $(Z_LIB) $(REL)/ecrt0.o $(SYSLIBS)

$(O)/dev/pwepoc.o: $(WEB)/engine/pwepoc.cpp $(WEB)/psiweb.h $(WEB)/fb/pwback.h $(TOP)/ssh/psishared.h
	@mkdir -p $(dir $@)
	$(CC) -c $(CXXFLAGS) $(PWEPOC_DEFS) $< -o $@
$(O)/dev/psiglue.o: $(TOP)/ssh/psiglue.cpp $(TOP)/ssh/psishared.h $(TOP)/ssh/psibell.h
	@mkdir -p $(dir $@)
	$(CC) -c $(CXXFLAGS) '-DPSI_SHARED_NAME="PsiWebShared"' $< -o $@
$(O)/dev/psi_heap.o: $(LW)/epoc/psi_heap.cpp $(WEB)/psiweb.h $(TOP)/ssh/psishared.h
	@mkdir -p $(dir $@)
	$(CC) -c $(CXXFLAGS) $< -o $@
$(O)/dev/psi_stubs.o: $(LW)/epoc/psi_stubs.c
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS_C) $< -o $@
$(O)/dev/pwupdate.o: $(WEB)/engine/pwupdate.c $(WEB)/engine/pwnet.h $(WEB)/psiweb.h $(WEB)/psiweb_cmds.h $(TOP)/ssh/psishared.h
	@mkdir -p $(dir $@)
	$(CC) -c $(LTCFLAGS) -I$(TOP)/ssh $< -o $@

# petran: heap 256 KB to 10 MB (as NetSurf's), 256 KB stack (Links' table
# layout recurses); UIDs as NetSurf's psiweb.exe, so PsiWeb.app starts it
$(O)/psiweb.exe: $(LINKS_OBJS) $(PSI_OBJS) $(DEV_OBJS) $(JPG_LIB) $(PNG_LIB) $(Z_LIB)
	arm-pe-ld -s -e _E32Startup --base-file $(O)/psiweb.bas -o $(O)/psiweb.tmp.exe $(EXE_OBJS)
	arm-pe-dlltool --as=arm-pe-as --output-exp $(O)/psiweb.exp --base-file $(O)/psiweb.bas $(EXE_OBJS)
	arm-pe-ld -s -e _E32Startup -Map $(O)/psiweb.map -o $(O)/psiweb.tmp.exe $(O)/psiweb.exp $(EXE_OBJS)
	WINEDEBUG=-all wine $(E)/tools/petran.exe $(O)/psiweb.tmp.exe $@ -nocall -uid1 0x1000007a -uid2 0x00000000 -uid3 0x01000A7B -heap 0x40000 0xa00000 -stack 0x40000
	@ls -la $@
exe: $(O)/psiweb.exe

.PHONY: all objs emu exe
