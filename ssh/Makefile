# psissh.exe - Dropbear SSH client for the Psion Series 5mx
#   make -C ssh SDK=/path/to/psion_cpp_sdk_linux
# The shared layer (psiglue.cpp, tls13.c, ptlog.cpp, zlib, LibTomCrypt,
# LibTomMath and Dropbear's curve25519) comes from psion-core, the core/
# submodule. CORE is relative to this folder: compiling those sources by a
# relative path keeps __FILE__ (LTC_ARGCHK) free of the machine's paths.
SDK      ?= $(PSION_SDK)
CORE     ?= ../core
E        := $(SDK)/epoc_cpp_sdk/epoc32
REL      := $(E)/release/marm/rel
export PATH := /usr/bin:/bin:$(SDK)/gcc-3.0-psion-98r2-9/bin:$(E)/tools
# (trailing /: dlltool makes TMP\name, so /tmp\x.s would land in / and need root)
export TMP := /tmp/
export WINEDEBUG := -all
CC       := arm-pe-gcc
CXX      := arm-pe-gcc

ARCH     := -s -fomit-frame-pointer -O2 -fno-strict-aliasing -nostdinc -mcpu=arm710 -mapcs-32 -mshort-load-bytes -msoft-float
DEFS     := -D__SYMBIAN32__ -D__PSISOFT32__ -D__GCC32__ -D__EPOC32__ -D__MARM__ -D__EXE__ -DNDEBUG
INCS     := -I $(CORE)/ssh -I $(CORE)/ssh/zlib -I compat -I db -I db/src -I $(CORE)/ssh/db/src -I $(CORE)/ssh/db/libtomcrypt/src/headers -I $(CORE)/ssh/db/libtommath -I $(E)/include/libc -I $(E)/include
BASEC    := $(ARCH) $(DEFS) -std=gnu99 -fno-builtin -include compat/psicompat.h $(INCS)
CFLAGS   := $(BASEC) -DDROPBEAR_CLIENT
LTCFLAGS := $(BASEC) -DLTC_SOURCE
LTMFLAGS := $(BASEC)
CXXFLAGS := $(ARCH) $(DEFS) -Wno-ctor-dtor-privacy -fcheck-new -fvtable-thunks -I $(CORE)/ssh -I compat -I $(E)/include/libc -I $(E)/include

DB_SRCS  := dbutil buffer dbhelpers signkey rsa dbrandom queue atomicio compat ltc_prng crypto_desc \
            curve25519 ed25519 dbmalloc dbctype common-session packet common-algo common-kex \
            common-channel common-chansession termcodes process-packet common-runopts circbuffer \
            list chachapoly kex-x25519 cli-main cli-auth cli-authpasswd cli-kex cli-session \
            cli-runopts cli-chansession cli-channel cli-authinteract cli-readconf bignum dh_groups \
            signkey_ossh netio listener fake-rfc2553 cli-authpubkey gened25519 keyimport psi-keygen
DB_OBJS  := $(addprefix obj/db/,$(addsuffix .o,$(DB_SRCS)))
LTC_SRCS := $(shell sed 's|^libtomcrypt/|$(CORE)/ssh/db/libtomcrypt/|' $(CORE)/ssh/ltc_sources.txt)
LTM_SRCS := $(shell sed 's|^libtommath/|$(CORE)/ssh/db/libtommath/|' $(CORE)/ssh/ltm_sources.txt)
LTC_OBJS := $(patsubst $(CORE)/ssh/db/%.c,obj/%.o,$(LTC_SRCS))
LTM_OBJS := $(patsubst $(CORE)/ssh/db/%.c,obj/%.o,$(LTM_SRCS))
Z_SRCS   := adler32 crc32 deflate inflate inftrees inffast trees zutil
Z_OBJS   := $(addprefix obj/zlib/,$(addsuffix .o,$(Z_SRCS)))
GLUE_OBJS := obj/psiglue.o obj/psishim.o obj/tls13.o obj/bench.o obj/sftp.o obj/tmuxq.o obj/ptlog.o

all: $(REL)/psissh.exe

obj/db/%.o: db/src/%.c compat/psicompat.h db/localoptions.h db/config.h
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) $< -o $@

# (Dropbear's X25519 lives in core, shared with the TLS client)
obj/db/curve25519.o: $(CORE)/ssh/db/src/curve25519.c $(CORE)/ssh/db/src/curve25519_gf32.h compat/psicompat.h db/localoptions.h db/config.h
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) $< -o $@

obj/libtomcrypt/%.o: $(CORE)/ssh/db/libtomcrypt/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $(LTCFLAGS) $< -o $@

obj/libtomcrypt/src/ciphers/aes/aes_enc.o: $(CORE)/ssh/db/libtomcrypt/src/ciphers/aes/aes.c
	@mkdir -p $(dir $@)
	$(CC) -c $(LTCFLAGS) -DENCRYPT_ONLY $< -o $@

obj/zlib/%.o: $(CORE)/ssh/zlib/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $(LTMFLAGS) -DNO_GZIP $< -o $@

obj/libz.a: $(Z_OBJS)
	rm -f $@; arm-pe-ar rcs $@ $^

obj/libtommath/%.o: $(CORE)/ssh/db/libtommath/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $(LTMFLAGS) $< -o $@

obj/bench.o: bench/bench.c bench/rsa_vec.h
	@mkdir -p obj
	$(CC) -c $(CFLAGS) $< -o $@

obj/psishim.o: psishim.c $(CORE)/ssh/psishared.h $(CORE)/ssh/tls13.h
	@mkdir -p obj
	$(CC) -c $(CFLAGS) $< -o $@

obj/sftp.o: sftp.c $(CORE)/ssh/psishared.h compat/psicompat.h
	@mkdir -p obj
	$(CC) -c $(CFLAGS) $< -o $@

obj/tmuxq.o: tmuxq.c $(CORE)/ssh/psishared.h compat/psicompat.h
	@mkdir -p obj
	$(CC) -c $(CFLAGS) $< -o $@

obj/tls13.o: $(CORE)/ssh/tls13.c $(CORE)/ssh/tls13.h
	@mkdir -p obj
	$(CC) -c $(CFLAGS) $< -o $@

obj/psiglue.o: $(CORE)/ssh/psiglue.cpp $(CORE)/ssh/psishared.h $(CORE)/ssh/psibell.h $(CORE)/ssh/psilink.h
	@mkdir -p obj
	$(CXX) -c $(CXXFLAGS) $< -o $@

obj/ptlog.o: $(CORE)/ssh/ptlog.cpp
	@mkdir -p obj
	$(CXX) -c $(CXXFLAGS) $< -o $@

obj/libltc.a: $(LTC_OBJS)
	rm -f $@; arm-pe-ar rcs $@ $^

obj/libltm.a: $(LTM_OBJS)
	rm -f $@; arm-pe-ar rcs $@ $^

V4BX_DIR := obj
include $(CORE)/tools/v4bx.mk
LIBS := $(REL)/ecrt0.o obj/libz.a obj/libltc.a obj/libltm.a $(REL)/estlib.lib $(REL)/euser.lib $(REL)/c32.lib $(REL)/efsrv.lib $(REL)/esock.lib $(REL)/insock.lib $(REL)/nifman.lib $(LIBGCC)

$(REL)/psissh.exe: $(DB_OBJS) $(GLUE_OBJS) obj/libz.a obj/libltc.a obj/libltm.a $(LIBGCC)
	arm-pe-ld -s -e _E32Startup --base-file obj/psissh.bas -o obj/psissh.tmp.exe $(REL)/eexe.o $(DB_OBJS) $(GLUE_OBJS) $(LIBS)
	arm-pe-dlltool --as=arm-pe-as --output-exp obj/psissh.exp --base-file obj/psissh.bas $(REL)/eexe.o $(DB_OBJS) $(GLUE_OBJS) $(LIBS)
	arm-pe-ld -s -e _E32Startup -Map obj/psissh.map -o obj/psissh.tmp.exe obj/psissh.exp $(REL)/eexe.o $(DB_OBJS) $(GLUE_OBJS) $(LIBS)
	python3 $(V4BX_PY) --check obj/psissh.tmp.exe     # (no BX: the netBook has none)
	wine $(E)/tools/petran.exe obj/psissh.tmp.exe $@ -nocall -uid1 0x1000007a -uid2 0x00000000 -uid3 0x01000A79 -heap 0x10000 0x400000 -stack 0x20000

dbobjs: $(DB_OBJS)
ltcobjs: $(LTC_OBJS)
ltmobjs: $(LTM_OBJS)

clean:
	rm -rf obj
.PHONY: all clean dbobjs ltcobjs ltmobjs
