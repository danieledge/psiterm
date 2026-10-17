# psicast.exe, PsiCast's engine, for the Psion (gcc 3.0 Psion 98r2, ESTLIB).
# Normally run by cast/build.sh, inside the psion-build container:
#   make -f cast/engine/engine.mk PSION_SDK=...
#
# csmain.c (link A of cast/docs/PROTOCOL.md), cskeys.c (typed characters as
# keys), csepoc.cpp (the chunk, the screen, input, power, main), the codec
# (cast/codec/pccodec.c, encoder only), zlib's deflate side and crc32
# (ssh/zlib), PsiWeb's single-connection layer (web/engine/pwnet.c, no TLS)
# and PsiTerm's psiglue.cpp (the modem and Psion Internet).
#
#   CSEPOC_DEFS=...   extra defines for csepoc.cpp
TOP     := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
CAST    := $(TOP)/cast
O       := $(TOP)/build/cast-epoc
SDK     ?= $(PSION_SDK)
E       := $(SDK)/epoc_cpp_sdk/epoc32
REL     := $(E)/release/marm/rel
export PATH := /usr/bin:/bin:$(SDK)/gcc-3.0-psion-98r2-9/bin:$(E)/tools
export WINEDEBUG := -all
# (trailing /: dlltool makes TMP\name, so /tmp\x.s would land in / and need root)
export TMP := /tmp/
CC      := arm-pe-gcc
ARCH    := -s -fomit-frame-pointer -O2 -fno-strict-aliasing -nostdinc -mcpu=arm710 -mapcs-32 -mshort-load-bytes -msoft-float
DEFS    := -D__SYMBIAN32__ -D__PSISOFT32__ -D__GCC32__ -D__EPOC32__ -D__MARM__ -D__EXE__ -DNDEBUG
LIBC    := -I$(TOP)/web/compat -I$(E)/include/libc -I$(E)/include
CFLAGS  := $(ARCH) $(DEFS) -std=gnu99 -fno-builtin -w $(LIBC) -I$(TOP)/ssh -I$(TOP)/ssh/zlib -I$(CAST)/engine -I$(CAST)/codec -include $(TOP)/mail/compat/pmcompat.h
ZFLAGS  := $(ARCH) $(DEFS) -DNO_GZIP -fno-builtin -w -I$(TOP)/web/compat $(LIBC)
CXXFLAGS := $(ARCH) $(DEFS) -Wno-ctor-dtor-privacy -fcheck-new -fvtable-thunks -w -I$(CAST)/engine -I$(TOP)/ssh -I$(E)/include/libc -I$(E)/include
LIBGCC  := $(SDK)/gcc-3.0-psion-98r2-9/lib/gcc-lib/arm-epoc-pe/3.0-psion-98r2/libgcc.a
SYSLIBS := $(REL)/estlib.lib $(REL)/euser.lib $(REL)/c32.lib $(REL)/efsrv.lib $(REL)/esock.lib $(REL)/insock.lib \
	$(REL)/nifman.lib $(REL)/ws32.lib $(REL)/fbscli.lib $(REL)/gdi.lib $(LIBGCC)
OUT     := $(O)/psicast.exe
HDRS    := $(CAST)/engine/psicast.h $(CAST)/engine/cs.h $(TOP)/ssh/psishared.h

Z_SRCS  := adler32 crc32 deflate trees zutil
OBJS    := $(O)/csmain.o $(O)/cskeys.o $(O)/pccodec.o $(O)/pwnet.o $(O)/csepoc.o $(O)/psiglue.o $(O)/nsprintf.o \
	$(addprefix $(O)/zlib/,$(Z_SRCS:=.o))

all: $(OUT)

$(O)/csmain.o: $(CAST)/engine/csmain.c $(HDRS) $(CAST)/codec/pccodec.h $(TOP)/web/engine/pwnet.h
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) $< -o $@
$(O)/cskeys.o: $(CAST)/engine/cskeys.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) $< -o $@
$(O)/pccodec.o: $(CAST)/codec/pccodec.c $(CAST)/codec/pccodec.h
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) -DPC_NO_DECODER -DNO_GZIP $< -o $@
$(O)/pwnet.o: $(TOP)/web/engine/pwnet.c $(TOP)/web/engine/pwnet.h $(TOP)/ssh/psishared.h
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) -DPW_NO_TLS $< -o $@
$(O)/csepoc.o: $(CAST)/engine/csepoc.cpp $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) -c $(CXXFLAGS) $(CSEPOC_DEFS) $< -o $@
$(O)/psiglue.o: $(TOP)/ssh/psiglue.cpp $(TOP)/ssh/psishared.h $(TOP)/ssh/psibell.h $(TOP)/ssh/psilink.h
	@mkdir -p $(dir $@)
	$(CC) -c $(CXXFLAGS) '-DPSI_SHARED_NAME="PsiCastShared"' $< -o $@
$(O)/nsprintf.o: $(TOP)/web/compat/nsprintf.c
	@mkdir -p $(dir $@)
	$(CC) -c $(ARCH) $(DEFS) -fno-builtin -w $(LIBC) $< -o $@
$(O)/zlib/%.o: $(TOP)/ssh/zlib/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $(ZFLAGS) $< -o $@

LINKOBJS = $(REL)/eexe.o $(OBJS) $(REL)/ecrt0.o $(SYSLIBS)
# petran: heap 64 KB to 2 MB (the encoder's copy of the screen and deflate's
# window are on it), 64 KB stack as psimail.exe (psiglue's
# dialling needs more than 32 KB); the big buffers are static.
# uid3 0x01000A93, PsiCast.app's own being 0x01000A92
$(OUT): $(OBJS)
	arm-pe-ld -s -e _E32Startup --base-file $(O)/psicast.bas -o $(O)/psicast.tmp.exe $(LINKOBJS)
	arm-pe-dlltool --as=arm-pe-as --output-exp $(O)/psicast.exp --base-file $(O)/psicast.bas $(LINKOBJS)
	arm-pe-ld -s -e _E32Startup -Map $(O)/psicast.map -o $(O)/psicast.tmp.exe $(O)/psicast.exp $(LINKOBJS)
	wine $(E)/tools/petran.exe $(O)/psicast.tmp.exe $@ -nocall -uid1 0x1000007a -uid2 0x00000000 -uid3 0x01000A93 -heap 0x10000 0x200000 -stack 0x10000
	@ls -la $@

.PHONY: all
