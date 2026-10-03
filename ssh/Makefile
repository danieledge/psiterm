# psissh.exe - Dropbear SSH client for the Psion Series 5mx
SDK      := /home/claude/psion_cpp_sdk_linux
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
INCS     := -I zlib -I compat -I db -I db/src -I db/libtomcrypt/src/headers -I db/libtommath -I $(E)/include/libc -I $(E)/include
BASEC    := $(ARCH) $(DEFS) -std=gnu99 -fno-builtin -include compat/psicompat.h $(INCS)
CFLAGS   := $(BASEC) -DDROPBEAR_CLIENT
LTCFLAGS := $(BASEC) -DLTC_SOURCE
LTMFLAGS := $(BASEC)
CXXFLAGS := $(ARCH) $(DEFS) -Wno-ctor-dtor-privacy -fcheck-new -fvtable-thunks -I compat -I $(E)/include/libc -I $(E)/include

DB_SRCS  := dbutil buffer dbhelpers signkey rsa dbrandom queue atomicio compat ltc_prng crypto_desc \
            curve25519 ed25519 dbmalloc dbctype common-session packet common-algo common-kex \
            common-channel common-chansession termcodes process-packet common-runopts circbuffer \
            list chachapoly kex-x25519 cli-main cli-auth cli-authpasswd cli-kex cli-session \
            cli-runopts cli-chansession cli-channel cli-authinteract cli-readconf bignum dh_groups \
            signkey_ossh netio listener fake-rfc2553 cli-authpubkey gened25519 keyimport psi-keygen
DB_OBJS  := $(addprefix obj/db/,$(addsuffix .o,$(DB_SRCS)))
LTC_SRCS := $(shell sed 's|^libtomcrypt/|db/libtomcrypt/|' ltc_sources.txt)
LTM_SRCS := $(shell sed 's|^libtommath/|db/libtommath/|' ltm_sources.txt)
LTC_OBJS := $(patsubst db/%.c,obj/%.o,$(LTC_SRCS))
LTM_OBJS := $(patsubst db/%.c,obj/%.o,$(LTM_SRCS))
Z_SRCS   := adler32 crc32 deflate inflate inftrees inffast trees zutil
Z_OBJS   := $(addprefix obj/zlib/,$(addsuffix .o,$(Z_SRCS)))
GLUE_OBJS := obj/psiglue.o obj/psishim.o obj/tls13.o obj/bench.o obj/sftp.o obj/tmuxq.o

all: $(REL)/psissh.exe

obj/db/%.o: db/src/%.c compat/psicompat.h db/localoptions.h db/config.h
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) $< -o $@

obj/db/curve25519.o: db/src/curve25519_gf32.h

obj/libtomcrypt/%.o: db/libtomcrypt/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $(LTCFLAGS) $< -o $@

obj/libtomcrypt/src/ciphers/aes/aes_enc.o: db/libtomcrypt/src/ciphers/aes/aes.c
	@mkdir -p $(dir $@)
	$(CC) -c $(LTCFLAGS) -DENCRYPT_ONLY $< -o $@

obj/zlib/%.o: zlib/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $(LTMFLAGS) -DNO_GZIP $< -o $@

obj/libz.a: $(Z_OBJS)
	rm -f $@; arm-pe-ar rcs $@ $^

obj/libtommath/%.o: db/libtommath/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $(LTMFLAGS) $< -o $@

obj/bench.o: bench/bench.c bench/rsa_vec.h
	@mkdir -p obj
	$(CC) -c $(CFLAGS) $< -o $@

obj/psishim.o: psishim.c psishared.h tls13.h
	@mkdir -p obj
	$(CC) -c $(CFLAGS) $< -o $@

obj/sftp.o: sftp.c psishared.h compat/psicompat.h
	@mkdir -p obj
	$(CC) -c $(CFLAGS) $< -o $@

obj/tmuxq.o: tmuxq.c psishared.h compat/psicompat.h
	@mkdir -p obj
	$(CC) -c $(CFLAGS) $< -o $@

obj/tls13.o: tls13.c tls13.h
	@mkdir -p obj
	$(CC) -c $(CFLAGS) $< -o $@

obj/psiglue.o: psiglue.cpp psishared.h psibell.h
	@mkdir -p obj
	$(CXX) -c $(CXXFLAGS) $< -o $@

obj/libltc.a: $(LTC_OBJS)
	rm -f $@; arm-pe-ar rcs $@ $^

obj/libltm.a: $(LTM_OBJS)
	rm -f $@; arm-pe-ar rcs $@ $^

LIBGCC := $(SDK)/gcc-3.0-psion-98r2-9/lib/gcc-lib/arm-epoc-pe/3.0-psion-98r2/libgcc.a
LIBS := $(REL)/ecrt0.o obj/libz.a obj/libltc.a obj/libltm.a $(REL)/estlib.lib $(REL)/euser.lib $(REL)/c32.lib $(REL)/efsrv.lib $(REL)/esock.lib $(REL)/insock.lib $(REL)/nifman.lib $(LIBGCC)

$(REL)/psissh.exe: $(DB_OBJS) $(GLUE_OBJS) obj/libz.a obj/libltc.a obj/libltm.a
	arm-pe-ld -s -e _E32Startup --base-file obj/psissh.bas -o obj/psissh.tmp.exe $(REL)/eexe.o $(DB_OBJS) $(GLUE_OBJS) $(LIBS)
	arm-pe-dlltool --as=arm-pe-as --output-exp obj/psissh.exp --base-file obj/psissh.bas $(REL)/eexe.o $(DB_OBJS) $(GLUE_OBJS) $(LIBS)
	arm-pe-ld -s -e _E32Startup -Map obj/psissh.map -o obj/psissh.tmp.exe obj/psissh.exp $(REL)/eexe.o $(DB_OBJS) $(GLUE_OBJS) $(LIBS)
	wine $(E)/tools/petran.exe obj/psissh.tmp.exe $@ -nocall -uid1 0x1000007a -uid2 0x00000000 -uid3 0x01000A79 -heap 0x10000 0x400000 -stack 0x20000

dbobjs: $(DB_OBJS)
ltcobjs: $(LTC_OBJS)
ltmobjs: $(LTM_OBJS)

clean:
	rm -rf obj
.PHONY: all clean dbobjs ltcobjs ltmobjs
