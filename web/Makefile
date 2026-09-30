# PsiWeb - NetSurf for the Psion Series 5mx.  See README.md; normally run
# through ./build.sh, which prepares NetSurf ($(NS)) and the rules.
#   make TARGET=host   PC test build (psiweb-host), same code as the Psion
#   make TARGET=epoc   psiweb.exe for the Psion
WEB     := $(abspath .)
TOP     := $(abspath ..)
NS      ?= $(TOP)/build/netsurf
NSINC   := $(NS)/hinst/include
TARGET  ?= epoc
O       := $(TOP)/build/web-$(TARGET)

.DEFAULT_GOAL := all
include $(O)/rules.mk

ifeq ($(TARGET),host)
TCC     := gcc
TFLAGS  := -O1 -g -w -std=gnu99 -D_DEFAULT_SOURCE -I$(TOP)/ssh
PLAT_OBJS := $(O)/web/pwhost.o $(O)/web/pgnet_host.o
NETDEFS := -DPW_NO_TLS
OUT     := $(O)/psiweb-host
else
SDK     ?= $(PSION_SDK)
E       := $(SDK)/epoc_cpp_sdk/epoc32
REL     := $(E)/release/marm/rel
export PATH := /usr/bin:/bin:$(SDK)/gcc-3.0-psion-98r2-9/bin:$(E)/tools
export WINEDEBUG := -all
export TMP := /tmp
TCC     := arm-pe-gcc
ARCH    := -s -fomit-frame-pointer -O2 -fno-strict-aliasing -nostdinc -mcpu=arm710 -mapcs-32 -mshort-load-bytes -msoft-float
DEFS    := -D__SYMBIAN32__ -D__PSISOFT32__ -D__GCC32__ -D__EPOC32__ -D__MARM__ -D__EXE__ -DNDEBUG
TFLAGS  := $(ARCH) $(DEFS) -std=gnu99 -fno-builtin -w -I$(TOP)/ssh -I$(WEB)/compat -I$(E)/include/libc -I$(E)/include -I$(TOP)/ssh/zlib -include $(WEB)/compat/nscompat.h
OUT     := $(REL)/psiweb.exe
CXXFLAGS := $(ARCH) $(DEFS) -Wno-ctor-dtor-privacy -fcheck-new -fvtable-thunks -w -I$(WEB) -I$(TOP)/ssh -I$(E)/include/libc -I$(E)/include
LTC     := $(TOP)/ssh/db/libtomcrypt/src
LTCFLAGS := $(ARCH) $(DEFS) -std=gnu99 -fno-builtin -w -I$(WEB)/tls -I$(WEB)/compat -I$(LTC)/headers -I$(E)/include/libc -I$(E)/include -DLTC_SOURCE
LTC_SRCS := stream/chacha/chacha_crypt stream/chacha/chacha_ivctr32 stream/chacha/chacha_keystream \
	stream/chacha/chacha_setup stream/chacha/chacha_done mac/poly1305/poly1305 hashes/sha2/sha256 hashes/sha2/sha512 \
	misc/zeromem misc/burn_stack misc/crypt/crypt_argchk misc/compare_testvector
Z_SRCS  := adler32 crc32 inflate inftrees inffast zutil
PLAT_OBJS := $(O)/web/pwepoc.o $(O)/web/psiglue.o $(O)/web/nscompat.o $(O)/web/nsprintf.o \
	$(O)/web/tls13.o $(O)/web/x25519.o $(O)/web/pwrandom.o $(O)/web/pwupdate.o \
	$(addprefix $(O)/ltc/,$(subst /,__,$(LTC_SRCS:=.o))) $(addprefix $(O)/zlib/,$(Z_SRCS:=.o))
LIBGCC  := $(SDK)/gcc-3.0-psion-98r2-9/lib/gcc-lib/arm-epoc-pe/3.0-psion-98r2/libgcc.a
SYSLIBS := $(REL)/estlib.lib $(REL)/euser.lib $(REL)/c32.lib $(REL)/efsrv.lib $(REL)/esock.lib $(REL)/insock.lib $(LIBGCC)
endif

NSFLAGS := -I$(NS)/netsurf -I$(NS)/netsurf/include -I$(NS)/netsurf/build/Linux-framebuffer \
	-I$(NS)/netsurf/frontends -I$(NS)/netsurf/content/handlers -I$(NSINC) -DPSIWEB -I$(WEB)/fb -I$(WEB)

WEB_OBJS := $(O)/web/nsfb_epoc.o $(O)/web/pwgrey.o $(O)/web/utf8proc.o $(O)/web/pwres.o \
	$(O)/web/fetch_psi.o $(O)/web/pwnet.o $(PLAT_OBJS)
ALL_OBJS := $(foreach p,$(NS_LIBS),$(OBJS_$(p))) $(OBJS_netsurf) $(WEB_OBJS)

all: $(OUT)

# psiweb.h embeds ssh/psishared.h: if either changes, every object that may
# touch the shared chunk must be rebuilt, or app and engine disagree on its
# layout and the engine crashes at start
$(WEB_OBJS): psiweb.h $(TOP)/ssh/psishared.h

$(O)/web/nsfb_epoc.o: fb/nsfb_epoc.c fb/pwback.h
	@mkdir -p $(dir $@)
	$(TCC) -c $(TFLAGS) -I$(NS)/libnsfb/include -I$(NS)/libnsfb/src $< -o $@
$(O)/web/%.o: fb/%.c fb/pwback.h
	@mkdir -p $(dir $@)
	$(TCC) -c $(TFLAGS) $< -o $@
$(O)/web/fetch_psi.o: engine/fetch_psi.c engine/pwnet.h fb/pwback.h
	@mkdir -p $(dir $@)
	$(TCC) -c $(TFLAGS) $(NSFLAGS) -I$(TOP)/ssh/zlib $< -o $@
$(O)/web/pwnet.o: engine/pwnet.c engine/pwnet.h psiweb.h
	@mkdir -p $(dir $@)
	$(TCC) -c $(TFLAGS) $(NETDEFS) $< -o $@
$(O)/pwres.c: mkres.py res/welcome.html
	python3 mkres.py $(NS)/netsurf $(WEB) > $@
$(O)/web/pwres.o: $(O)/pwres.c
	@mkdir -p $(dir $@)
	$(TCC) -c $(TFLAGS) $< -o $@
$(O)/web/utf8proc.o: $(NS)/libutf8proc/utf8proc.c
	@mkdir -p $(dir $@)
	$(TCC) -c $(TFLAGS) -DUTF8PROC_STATIC $< -o $@

ifeq ($(TARGET),host)
$(OUT): $(ALL_OBJS)
	gcc -o $@ $^ -lz -lm
else
$(O)/web/pwepoc.o: engine/pwepoc.cpp psiweb.h fb/pwback.h
	@mkdir -p $(dir $@)
	$(TCC) -c $(CXXFLAGS) $< -o $@
$(O)/web/psiglue.o: $(TOP)/ssh/psiglue.cpp $(TOP)/ssh/psishared.h
	@mkdir -p $(dir $@)
	$(TCC) -c $(CXXFLAGS) '-DPSI_SHARED_NAME="PsiWebShared"' $< -o $@
$(O)/web/%.o: compat/%.c
	@mkdir -p $(dir $@)
	$(TCC) -c $(TFLAGS) $< -o $@
$(O)/web/tls13.o: $(TOP)/ssh/tls13.c tls/includes.h
	@mkdir -p $(dir $@)
	$(TCC) -c $(LTCFLAGS) -I$(TOP)/ssh/db/src $< -o $@
$(O)/web/pwupdate.o: engine/pwupdate.c engine/pwnet.h psiweb.h psiweb_cmds.h tls/includes.h
	@mkdir -p $(dir $@)
	$(TCC) -c $(LTCFLAGS) -I$(TOP)/ssh $< -o $@
$(O)/emu/pwupdate_test.o: engine/pwupdate.c engine/pwnet.h psiweb.h psiweb_cmds.h tls/includes.h
	@mkdir -p $(dir $@)
	$(TCC) -c $(LTCFLAGS) -I$(TOP)/ssh -DPW_TEST_KEY $< -o $@
$(O)/web/%.o: tls/%.c tls/includes.h tls/options.h
	@mkdir -p $(dir $@)
	$(TCC) -c $(LTCFLAGS) -I$(TOP)/ssh/db/src $< -o $@
$(O)/ltc/%.o:
	@mkdir -p $(dir $@)
	$(TCC) -c $(LTCFLAGS) $(LTC)/$(subst __,/,$*).c -o $@
$(O)/zlib/%.o: $(TOP)/ssh/zlib/%.c
	@mkdir -p $(dir $@)
	$(TCC) -c $(ARCH) $(DEFS) -fno-builtin -w -I$(WEB)/compat -I$(E)/include/libc -I$(E)/include $< -o $@

LINKOBJS = $(REL)/eexe.o $(ALL_OBJS) $(REL)/ecrt0.o $(SYSLIBS)

# psiweb-emu.pe: the same ARM objects with emu/ standing in for EPOC, to run
# in an ARM emulator on a PC (emu/run_psiweb.py)
EMU_OBJS := $(filter-out $(O)/web/pwepoc.o $(O)/web/psiglue.o $(O)/web/pwupdate.o,$(ALL_OBJS)) \
	$(O)/emu/emu_rt.o $(O)/emu/emu_hc.o $(O)/emu/emu_back.o $(O)/emu/pwupdate_test.o
$(O)/emu/%.o: emu/%.c
	@mkdir -p $(dir $@)
	$(TCC) -c $(ARCH) -O2 -fno-builtin -w -D__EPOC32__ -I$(WEB) -I$(TOP)/ssh -I$(E)/include/libc $< -o $@
$(O)/psiweb-emu.pe: $(EMU_OBJS)
	arm-pe-ld -o $@ -e main -Map $(O)/psiweb-emu.map $(EMU_OBJS) $(LIBGCC)
emu: $(O)/psiweb-emu.pe
$(OUT): $(ALL_OBJS)
	arm-pe-ld -s -e _E32Startup --base-file $(O)/psiweb.bas -o $(O)/psiweb.tmp.exe $(LINKOBJS)
	arm-pe-dlltool --as=arm-pe-as --output-exp $(O)/psiweb.exp --base-file $(O)/psiweb.bas $(LINKOBJS)
	arm-pe-ld -s -e _E32Startup -Map $(O)/psiweb.map -o $(O)/psiweb.tmp.exe $(O)/psiweb.exp $(LINKOBJS)
	wine $(E)/tools/petran.exe $(O)/psiweb.tmp.exe $@ -nocall -uid1 0x1000007a -uid2 0x00000000 -uid3 0x01000A7B -heap 0x40000 0xa00000 -stack 0x40000
	@ls -la $@
endif

objs: $(ALL_OBJS)
.PHONY: all objs
