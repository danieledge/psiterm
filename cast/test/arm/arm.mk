# The encoder built for the Psion's ARM (gcc 3.0 Psion 98r2, the same flags
# as the engines), linked with a tiny stand-in runtime, for armbench.py to
# run under unicorn and count instructions. Run from the repository root:
#   tools/docker/psibuild "make -f cast/test/arm/arm.mk"
# Output: build/cast-arm/pcbench.pe (+ .map, .syms). Its own build folder:
# nothing here touches the SDK's shared object folders.
TOP   := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
O     := $(TOP)/build/cast-arm
ZL    := $(TOP)/ssh/zlib
SDK   ?= $(PSION_SDK)
E     := $(SDK)/epoc_cpp_sdk/epoc32
export PATH := /usr/bin:/bin:$(SDK)/gcc-3.0-psion-98r2-9/bin:$(E)/tools
export TMP := /tmp/
CC    := arm-pe-gcc
ARCH  := -fomit-frame-pointer -O2 -fno-strict-aliasing -nostdinc -mcpu=arm710 -mapcs-32 -mshort-load-bytes -msoft-float
DEFS  := -D__SYMBIAN32__ -D__PSISOFT32__ -D__GCC32__ -D__EPOC32__ -D__MARM__ -D__EXE__ -DNDEBUG
CFL   := $(ARCH) $(DEFS) -fno-builtin -w -I$(E)/include/libc -I$(E)/include -I$(ZL) -I$(TOP)/cast/codec -DNO_GZIP
LIBGCC := $(SDK)/gcc-3.0-psion-98r2-9/lib/gcc-lib/arm-epoc-pe/3.0-psion-98r2/libgcc.a
ZSRC  := adler32 deflate trees zutil
OBJS  := $(O)/pccodec.o $(O)/armdrv.o $(O)/armrt.o $(addprefix $(O)/z_,$(ZSRC:=.o))

all: $(O)/pcbench.pe

$(O)/pccodec.o: $(TOP)/cast/codec/pccodec.c $(TOP)/cast/codec/pccodec.h
	@mkdir -p $(O)
	$(CC) -c $(CFL) -DPC_NO_DECODER $< -o $@
	arm-pe-size $@
$(O)/armdrv.o: $(TOP)/cast/test/arm/armdrv.c
	@mkdir -p $(O)
	$(CC) -c $(CFL) $< -o $@
$(O)/armrt.o: $(TOP)/cast/test/arm/armrt.c
	@mkdir -p $(O)
	$(CC) -c $(CFL) $< -o $@
$(O)/z_%.o: $(ZL)/%.c
	@mkdir -p $(O)
	$(CC) -c $(CFL) $< -o $@

$(O)/pcbench.pe: $(OBJS)
	arm-pe-ld -o $@ -e _drv_init -Map $(O)/pcbench.map $(OBJS) $(LIBGCC)
	arm-pe-nm -n $@ > $(O)/pcbench.syms
