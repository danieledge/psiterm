# Host tests for the PsiCast screen codec (cast/codec).
#
#   make -C cast/test test      round trip and decoder fuzz, under ASan/UBSan
#   make -C cast/test bench     the benchmark (frames from bench.py)
#
# Objects go to build/cast-test at the top of the checkout.

TOP   := ../..
OUT   := $(TOP)/build/cast-test
ZDIR  := $(TOP)/ssh/zlib
CDIR  := ../codec
ZSRC  := adler32 crc32 deflate inflate inftrees inffast trees zutil
CC    ?= gcc

WARN  := -Wall -Wextra -Wno-unused-function
BASE  := -g -fno-strict-aliasing -DNO_GZIP -I$(CDIR) -I$(ZDIR)
SAN   := -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
FAST  := -O2

ZSAN  := $(addprefix $(OUT)/san/z_,$(addsuffix .o,$(ZSRC)))
ZFAST := $(addprefix $(OUT)/fast/z_,$(addsuffix .o,$(ZSRC)))

.PHONY: all test bench clean halves
all: $(OUT)/rt $(OUT)/fuzz $(OUT)/bench $(OUT)/memuse

$(OUT)/san/z_%.o: $(ZDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BASE) $(SAN) -w -c $< -o $@
$(OUT)/fast/z_%.o: $(ZDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BASE) $(FAST) -w -c $< -o $@

$(OUT)/san/pccodec.o: $(CDIR)/pccodec.c $(CDIR)/pccodec.h $(CDIR)/pctune.h
	@mkdir -p $(dir $@)
	$(CC) $(BASE) $(SAN) $(WARN) -std=c89 -Wdeclaration-after-statement -c $< -o $@
$(OUT)/fast/pccodec.o: $(CDIR)/pccodec.c $(CDIR)/pccodec.h $(CDIR)/pctune.h
	@mkdir -p $(dir $@)
	$(CC) $(BASE) $(FAST) $(WARN) -c $< -o $@

$(OUT)/rt: rt.c tutil.h $(OUT)/san/pccodec.o $(ZSAN)
	$(CC) $(BASE) $(SAN) $(WARN) $< $(OUT)/san/pccodec.o $(ZSAN) -o $@
$(OUT)/fuzz: fuzz.c tutil.h $(OUT)/san/pccodec.o $(ZSAN)
	$(CC) $(BASE) $(SAN) $(WARN) $< $(OUT)/san/pccodec.o $(ZSAN) -o $@
$(OUT)/bench: bench.c tutil.h $(OUT)/fast/pccodec.o $(ZFAST)
	$(CC) $(BASE) $(FAST) $(WARN) $< $(OUT)/fast/pccodec.o $(ZFAST) -o $@

$(OUT)/memuse: memuse.c tutil.h $(OUT)/fast/pccodec.o $(ZFAST)
	$(CC) $(BASE) $(FAST) $(WARN) $< $(OUT)/fast/pccodec.o $(ZFAST) -o $@

# each half on its own, as the Psion and the Atom build them
halves: $(CDIR)/pccodec.c
	@mkdir -p $(OUT)
	$(CC) $(BASE) $(WARN) -std=c89 -Wdeclaration-after-statement -DPC_NO_DECODER -c $< -o $(OUT)/enc_only.o
	$(CC) $(BASE) $(WARN) -std=c89 -Wdeclaration-after-statement -DPC_NO_ENCODER -c $< -o $(OUT)/dec_only.o
	@nm $(OUT)/enc_only.o | grep -q " U inflate" && { echo "encoder pulls in inflate"; exit 1; } || true
	@nm $(OUT)/dec_only.o | grep -q " U deflate" && { echo "decoder pulls in deflate"; exit 1; } || true
	@echo "ok   encoder-only and decoder-only builds"

test: $(OUT)/rt $(OUT)/fuzz halves
	$(OUT)/rt
	$(OUT)/fuzz

bench: $(OUT)/bench
	python3 bench.py $(OUT)

clean:
	rm -rf $(OUT)
