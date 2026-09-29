# PsiMail - an email client for the Psion Series 5mx.  See README.md.
#   make TARGET=host    psimail-host: the engine on a PC, for tests
#   make TARGET=epoc    psimail.exe for the Psion (normally via ./build.sh)
MAIL    := $(abspath .)
TOP     := $(abspath ..)
TARGET  ?= epoc
O       := $(TOP)/build/mail-$(TARGET)
LTC     := $(TOP)/ssh/db/libtomcrypt/src
LTM     := $(TOP)/ssh/db/libtommath

ENGINE  := pmmain imap imapparse smtp compose mime charset html store pmnet certcheck pmrsa
LTC_SRCS := stream/chacha/chacha_crypt stream/chacha/chacha_ivctr32 stream/chacha/chacha_keystream \
	stream/chacha/chacha_setup stream/chacha/chacha_done mac/poly1305/poly1305 hashes/sha2/sha256 \
	hashes/sha2/sha384 hashes/sha2/sha512 misc/zeromem misc/burn_stack misc/crypt/crypt_argchk misc/compare_testvector
LTM_SRCS := $(notdir $(basename $(shell sed -n 's|^libtommath/||p' $(TOP)/ssh/ltm_sources.txt)))

ifeq ($(TARGET),host)
CC      := cc
ARCH    := -O1 -g
DEFS    := -DTLS_VERIFY
TFLAGS  := $(ARCH) -w -std=gnu99 -D_DEFAULT_SOURCE $(DEFS)
LTCFLAGS := $(TFLAGS) -I$(MAIL)/tls -I$(LTC)/headers -DLTC_SOURCE
LTMFLAGS := $(TFLAGS) -I$(MAIL)/tls -I$(LTM)
OUT     := $(O)/psimail-host
PLAT    := $(O)/pmhost.o $(O)/pgnet_host.o
LINK    = $(CC) -o $@ $^
else
SDK     ?= $(PSION_SDK)
E       := $(SDK)/epoc_cpp_sdk/epoc32
REL     := $(E)/release/marm/rel
export PATH := /usr/bin:/bin:$(SDK)/gcc-3.0-psion-98r2-9/bin:$(E)/tools
export WINEDEBUG := -all
export TMP := /tmp
CC      := arm-pe-gcc
ARCH    := -s -fomit-frame-pointer -O2 -fno-strict-aliasing -nostdinc -mcpu=arm710 -mapcs-32 -mshort-load-bytes -msoft-float
DEFS    := -D__SYMBIAN32__ -D__PSISOFT32__ -D__GCC32__ -D__EPOC32__ -D__MARM__ -D__EXE__ -DNDEBUG -DTLS_VERIFY
LIBC    := -I$(TOP)/web/compat -I$(E)/include/libc -I$(E)/include
TFLAGS  := $(ARCH) $(DEFS) -std=gnu99 -fno-builtin -w $(LIBC) -include $(MAIL)/compat/pmcompat.h
LTCFLAGS := $(TFLAGS) -I$(MAIL)/tls -I$(LTC)/headers -DLTC_SOURCE
LTMFLAGS := $(TFLAGS) -I$(MAIL)/tls -I$(LTM)
CXXFLAGS := $(ARCH) $(DEFS) -Wno-ctor-dtor-privacy -fcheck-new -fvtable-thunks -w -I$(MAIL) -I$(TOP)/ssh -I$(E)/include/libc -I$(E)/include
OUT     := $(REL)/psimail.exe
PLAT    := $(O)/pmepoc.o $(O)/psiglue.o $(O)/nsprintf.o
LIBGCC  := $(SDK)/gcc-3.0-psion-98r2-9/lib/gcc-lib/arm-epoc-pe/3.0-psion-98r2/libgcc.a
SYSLIBS := $(REL)/estlib.lib $(REL)/euser.lib $(REL)/c32.lib $(REL)/efsrv.lib $(REL)/esock.lib $(REL)/insock.lib $(LIBGCC)
endif

ENG_OBJS := $(addprefix $(O)/,$(ENGINE:=.o))
TLS_OBJS := $(O)/tls13.o $(O)/x25519.o $(O)/pmrandom.o
LTC_OBJS := $(addprefix $(O)/ltc/,$(subst /,__,$(LTC_SRCS:=.o)))
LTM_OBJS := $(addprefix $(O)/ltm/,$(LTM_SRCS:=.o))
HDRS    := $(MAIL)/psimail.h $(MAIL)/engine/pm.h

all: $(OUT)

$(O)/%.o: engine/%.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) -c $(TFLAGS) -I$(TOP)/ssh $< -o $@
$(O)/certcheck.o: engine/certcheck.c engine/roots.h $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) -c $(LTCFLAGS) -I$(TOP)/ssh $< -o $@
$(O)/pmrsa.o: engine/pmrsa.c
	@mkdir -p $(dir $@)
	$(CC) -c $(LTMFLAGS) $< -o $@
$(O)/tls13.o: $(TOP)/ssh/tls13.c tls/includes.h
	@mkdir -p $(dir $@)
	$(CC) -c $(LTCFLAGS) -I$(TOP)/ssh/db/src $< -o $@
$(O)/%.o: tls/%.c tls/includes.h tls/options.h
	@mkdir -p $(dir $@)
	$(CC) -c $(LTCFLAGS) -I$(TOP)/ssh/db/src $< -o $@
$(O)/ltc/%.o:
	@mkdir -p $(dir $@)
	$(CC) -c $(LTCFLAGS) $(LTC)/$(subst __,/,$*).c -o $@
$(O)/ltm/%.o: $(LTM)/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $(LTMFLAGS) $< -o $@
$(O)/libltm.a: $(LTM_OBJS)
	rm -f $@; $(if $(filter host,$(TARGET)),ar,arm-pe-ar) rcs $@ $^

ifeq ($(TARGET),host)
$(O)/%.o: host/%.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) -c $(TFLAGS) -I$(TOP)/ssh $< -o $@
$(OUT): $(ENG_OBJS) $(TLS_OBJS) $(LTC_OBJS) $(PLAT) $(O)/libltm.a
	$(CC) -o $@ $^
else
$(O)/pmepoc.o: engine/pmepoc.cpp $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) -c $(CXXFLAGS) $< -o $@
$(O)/psiglue.o: $(TOP)/ssh/psiglue.cpp $(TOP)/ssh/psishared.h
	@mkdir -p $(dir $@)
	$(CC) -c $(CXXFLAGS) '-DPSI_SHARED_NAME="PsiMailShared"' $< -o $@
$(O)/nsprintf.o: $(TOP)/web/compat/nsprintf.c
	@mkdir -p $(dir $@)
	$(CC) -c $(ARCH) $(DEFS) -fno-builtin -w $(LIBC) $< -o $@
LINKOBJS = $(REL)/eexe.o $(ENG_OBJS) $(TLS_OBJS) $(LTC_OBJS) $(PLAT) $(O)/libltm.a $(REL)/ecrt0.o $(SYSLIBS)
$(OUT): $(ENG_OBJS) $(TLS_OBJS) $(LTC_OBJS) $(PLAT) $(O)/libltm.a
	arm-pe-ld -s -e _E32Startup --base-file $(O)/psimail.bas -o $(O)/psimail.tmp.exe $(LINKOBJS)
	arm-pe-dlltool --as=arm-pe-as --output-exp $(O)/psimail.exp --base-file $(O)/psimail.bas $(LINKOBJS)
	arm-pe-ld -s -e _E32Startup -Map $(O)/psimail.map -o $(O)/psimail.tmp.exe $(O)/psimail.exp $(LINKOBJS)
	wine $(E)/tools/petran.exe $(O)/psimail.tmp.exe $@ -nocall -uid1 0x1000007a -uid2 0x00000000 -uid3 0x01000A7D -heap 0x10000 0x300000 -stack 0x10000
	@ls -la $@
endif

clean:
	rm -rf $(O)
.PHONY: all clean
