# Links 2 for PsiWeb: the PC proof (links-psi), built 32-bit like the ARM
# target, with zlib, libpng and IJG libjpeg compiled from source as they
# will be for EPOC. Normally run through build_host.sh inside the
# psion-build container (tools/docker/psibuild).
#   make -C web/links            -> build/links/host/links-psi
WEB     := $(abspath ..)
TOP     := $(abspath ../..)
LK      ?= $(TOP)/build/links
SRC     := $(LK)/links-2.30
PNG     := $(LK)/libpng-1.6.43
JPG     := $(LK)/jpeg-9f
ZL      := $(TOP)/ssh/zlib
O       ?= $(LK)/host
OUT     := $(O)/links-psi

CC      := gcc -m32
OPT     := -O2 -g
INC     := -I$(O)/inc -I$(PNG) -I$(JPG) -I$(ZL)
# OpenSSL for https on the PC only (the Psion will use ssh/tls13.c)
SSLINC  := -I$(O)/sslinc
SSLLIBS := /usr/lib/i386-linux-gnu/libssl.so.3 /usr/lib/i386-linux-gnu/libcrypto.so.3

LINKS_SRCS := af_unix auth avif beos bfu block bookmark cache charsets compress connect \
	cookies data default dip directfb dither dns doh dos drivers error file finger fn_impl \
	fontconf font_inc framebuf freetype ftp gif grx hpux html html_gr html_r html_tbl http \
	https img imgcache jpeg jsint kbd language listedit lru mailto main memory menu objreq \
	os_dep pmshell png sched select session smb string suffix svg svgalib terminal tiff types \
	url view view_gr vms webp x xbm
JPG_SRCS := jaricom jcomapi jdapimin jdapistd jdarith jdatasrc jdcoefct jdcolor jddctmgr \
	jdhuff jdinput jdmainct jdmarker jdmaster jdmerge jdpostct jdsample jerror jidctflt \
	jidctfst jidctint jquant1 jquant2 jutils jmemmgr jmemnobs
PNG_SRCS := png pngerror pngget pngmem pngpread pngread pngrio pngrtran pngrutil pngset pngtrans \
	pngwrite pngwutil pngwio pngwtran
Z_SRCS   := adler32 crc32 inflate inftrees inffast zutil deflate trees

LINKS_OBJS := $(addprefix $(O)/links/,$(LINKS_SRCS:=.o))
# (psi_grey.c, as in the ARM build: psi_drv.c uses its table of greys)
PSI_OBJS   := $(O)/psi/psi_drv.o $(O)/psi/psi_mem.o $(O)/psi/pwhost.o $(O)/psi/psi_grey.o $(O)/psi/psi_digest.o
JPG_LIB    := $(O)/libjpeg.a
PNG_LIB    := $(O)/libpng.a
Z_LIB      := $(O)/libz.a

LCFLAGS := $(OPT) -w -DHAVE_CONFIG_H -DHAVE_CONFIG2_H -I$(SRC) -I$(WEB)/links/host $(INC) $(SSLINC)

all: $(OUT)

$(O)/inc/pnglibconf.h: $(PNG)/scripts/pnglibconf.h.prebuilt
	@mkdir -p $(dir $@)
	cp $< $@
$(O)/inc/jconfig.h: $(JPG)/jconfig.txt
	@mkdir -p $(dir $@)
	cp $< $@
# 32-bit view of the 64-bit OpenSSL configuration header
$(O)/sslinc/openssl/configuration.h: /usr/include/x86_64-linux-gnu/openssl/configuration.h
	@mkdir -p $(dir $@)
	sed -e 's/^\(# *\)define SIXTY_FOUR_BIT_LONG/\1undef SIXTY_FOUR_BIT_LONG/' \
	    -e 's/^\(# *\)undef THIRTY_TWO_BIT/\1define THIRTY_TWO_BIT/' $< > $@
	cp /usr/include/x86_64-linux-gnu/openssl/opensslconf.h $(dir $@)

HDRS := $(O)/inc/pnglibconf.h $(O)/inc/jconfig.h $(O)/sslinc/openssl/configuration.h

$(O)/links/%.o: $(SRC)/%.c $(HDRS) $(WEB)/links/host/config2.h $(SRC)/links.h $(SRC)/cfg.h
	@mkdir -p $(dir $@)
	$(CC) -c $(LCFLAGS) $< -o $@

$(O)/psi/psi_drv.o: psi_drv.c $(HDRS) $(WEB)/fb/pwback.h $(SRC)/links.h
	@mkdir -p $(dir $@)
	$(CC) -c $(LCFLAGS) -I$(WEB)/fb -I$(WEB) $< -o $@
$(O)/psi/psi_mem.o: psi_mem.c
	@mkdir -p $(dir $@)
	$(CC) -c $(OPT) -Wall $< -o $@
$(O)/psi/psi_grey.o: psi_grey.c $(WEB)/fb/pwback.h $(TOP)/ssh/psigrey.h
	@mkdir -p $(dir $@)
	$(CC) -c $(OPT) -w -std=gnu99 -I$(WEB)/fb $< -o $@
$(O)/psi/psi_digest.o: psi_digest.c
	@mkdir -p $(dir $@)
	$(CC) -c $(OPT) -Wall $< -o $@
$(O)/psi/%.o: $(WEB)/fb/%.c $(WEB)/fb/pwback.h
	@mkdir -p $(dir $@)
	$(CC) -c $(OPT) -w -std=gnu99 -D_DEFAULT_SOURCE -I$(TOP)/ssh -I$(WEB) $< -o $@

$(O)/jpeg/%.o: $(JPG)/%.c $(O)/inc/jconfig.h
	@mkdir -p $(dir $@)
	$(CC) -c $(OPT) -w -I$(O)/inc -I$(JPG) $< -o $@
$(O)/png/%.o: $(PNG)/%.c $(O)/inc/pnglibconf.h
	@mkdir -p $(dir $@)
	$(CC) -c $(OPT) -w -DPNG_ARM_NEON_OPT=0 -DPNG_INTEL_SSE_OPT=0 -DPNG_POWERPC_VSX_OPT=0 \
		-DPNG_MIPS_MSA_OPT=0 -DPNG_MIPS_MMI_OPT=0 -DPNG_LOONGARCH_LSX_OPT=0 -I$(O)/inc -I$(PNG) -I$(ZL) $< -o $@
$(O)/zlib/%.o: $(ZL)/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $(OPT) -w -I$(ZL) $< -o $@

$(JPG_LIB): $(addprefix $(O)/jpeg/,$(JPG_SRCS:=.o))
	rm -f $@; ar rcs $@ $^
$(PNG_LIB): $(addprefix $(O)/png/,$(PNG_SRCS:=.o))
	rm -f $@; ar rcs $@ $^
$(Z_LIB): $(addprefix $(O)/zlib/,$(Z_SRCS:=.o))
	rm -f $@; ar rcs $@ $^

$(OUT): $(LINKS_OBJS) $(PSI_OBJS) $(JPG_LIB) $(PNG_LIB) $(Z_LIB)
	$(CC) -o $@ $(LINKS_OBJS) $(PSI_OBJS) $(JPG_LIB) $(PNG_LIB) $(Z_LIB) $(SSLLIBS) -lm -Wl,-Map,$(O)/links-psi.map
	@ls -la $@

.PHONY: all
