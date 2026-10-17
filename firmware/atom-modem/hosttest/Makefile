# Host tests for the Atom modem (no ESP32 needed). MIT licence.
#   make test       the unit tests: the modem (AT parser, +++ guard times,
#                   pacing, the settings records and the schema, TLS
#                   termination, psiexec, the uplink manager), the USB
#                   network framing (NCM, Apple), and the web proxy with its
#                   HTML simplifier, reader mode and picture converter.
#                   PAGES=folder adds real saved pages (bbc.html,
#                   guardian.html, wiki.html, hn.html; see ../README.md)
#   test_proxy_tinfl is the proxy test again with the inflater the Atom uses
#                   (tinfl from miniz, as in the ESP32 ROM: tinfl/) in place
#                   of zlib, so its 32 KB wrapping window is tested too
#   make screens    the LCD's pages drawn to PNG files in screens/ (the
#                   fonts come from M5GFX: run `pio run -e atoms3r` once)
#   make hostmodem  the modem as a PC program on a unix socket, for the
#                   Psion emulator's serial bridge (see hostmodem.cpp)
CXX ?= g++
CC ?= gcc
CXXFLAGS ?= -O1 -g -Wall -Wextra -Wno-unused-parameter -std=c++17
IMG = ../../../mail/engine/img
CODEC = ../../../cast/codec
SRC = ../src/cast/cast.cpp ../src/cast/websock.cpp ../src/cast/castnet.cpp ../src/modem.cpp ../src/settings.cpp ../src/schema.cpp ../src/uplink.cpp ../src/proxy.cpp ../src/wgconf.cpp ../src/netlist.cpp ../src/dnsrelay.cpp ../src/atremote.cpp \
      ../src/htmlsimp.cpp ../src/gzip.cpp ../src/imgconv.cpp ../src/ppp.cpp ../src/status.cpp ../src/canvas.cpp ../src/button.cpp ../src/usbnet/ncm.cpp ../src/usbnet/ipheth.cpp
HDR = ../src/cast/cast.h ../src/cast/websock.h ../src/cast/castnet.h $(CODEC)/pccodec.h ../src/canvas.h ../src/fontmetrics.h ../src/modem.h ../src/atremote.h ../src/wgconf.h ../src/netlist.h ../src/dnsrelay.h ../src/status.h ../src/button.h ../src/ppp.h ../src/settings.h ../src/schema.h ../src/uplink.h ../src/proxy.h ../src/htmlsimp.h \
      ../src/gzip.h ../src/imgconv.h ../src/usbnet/ncm.h ../src/usbnet/ipheth.h
IMGSRC = $(IMG)/pmimg.c $(IMG)/pmjpeg.c $(IMG)/pmjprog.c $(IMG)/pmpng.c $(IMG)/pmgif.c $(IMG)/picojpeg.c
# (with PsiCast's screen codec, both halves: the tests play the Psion too)
IMGOBJ = pmimg.o pmjpeg.o pmjprog.o pmpng.o pmgif.o picojpeg.o pccodec.o
INC = -I../src -I$(IMG) -I$(CODEC)

test: test_modem test_v22 test_atremote test_usbnet test_proxy test_proxy_tinfl test_imapxcode test_fwtrial test_cast pictures
	./test_modem
	./test_v22
	./test_atremote
	./test_usbnet
	./test_proxy
	./test_proxy_tinfl
	./test_imapxcode
	./test_fwtrial
	./test_cast
	if command -v node >/dev/null; then node test_iconmatch.js; else echo 'test_iconmatch: SKIP (no node)'; fi

# the decoders PsiMail uses, compiled as they are (zlib from the system here)
pm%.o: $(IMG)/pm%.c $(IMG)/pmimg.h
	$(CC) -O1 -g -Wall -I$(IMG) -c -o $@ $<
picojpeg.o: $(IMG)/picojpeg.c $(IMG)/picojpeg.h
	$(CC) -O1 -g -Wall -I$(IMG) -c -o $@ $<
pccodec.o: $(CODEC)/pccodec.c $(CODEC)/pccodec.h
	$(CC) -O1 -g -Wall -fno-strict-aliasing -I$(CODEC) -c -o $@ $<

test_modem: test_modem.cpp fakehal.h $(SRC) $(HDR) $(IMGOBJ)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ test_modem.cpp $(SRC) $(IMGOBJ) -lz

# 2.2: WireGuard config and routing rule, DNS relay, saved networks, line
# trial, double-press, schema coverage, secrets (with the sanitizers)
test_v22: test_v22.cpp fakehal.h $(SRC) $(HDR) $(IMGOBJ)
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined $(INC) -o $@ test_v22.cpp $(SRC) $(IMGOBJ) -lz

# 2.2: AT commands over PPP for PsiConnect (atremote.h): the filter, the
# session, RemoteLine, a PPP call surviving it (with the sanitizers)
test_atremote: test_atremote.cpp fakehal.h $(SRC) $(HDR) $(IMGOBJ)
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined $(INC) -o $@ test_atremote.cpp $(SRC) $(IMGOBJ) -lz

# the firmware trial after an update over the web (src/fwtrial.h), against a
# model of the bootloader's two-slot otadata (with the sanitizers)
test_fwtrial: test_fwtrial.cpp ../src/fwtrial.cpp ../src/fwtrial.h
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined $(INC) -o $@ test_fwtrial.cpp ../src/fwtrial.cpp

# PsiCast (src/cast): link A's framing and session against a Psion played with
# the real encoder, tokens, WebSocket, per-viewer merging (pixel-exact), the
# input path and the modem route (with the sanitizers)
test_cast: test_cast.cpp fakehal.h $(SRC) $(HDR) $(IMGOBJ)
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined $(INC) -o $@ test_cast.cpp $(SRC) $(IMGOBJ) -lz

test_usbnet: test_usbnet.cpp ../src/usbnet/ncm.cpp ../src/usbnet/ncm.h ../src/usbnet/ipheth.cpp ../src/usbnet/ipheth.h
	$(CXX) $(CXXFLAGS) $(INC) -o $@ test_usbnet.cpp ../src/usbnet/ncm.cpp ../src/usbnet/ipheth.cpp

test_proxy: test_proxy.cpp fakehal.h $(SRC) $(HDR) $(IMGOBJ)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ test_proxy.cpp $(SRC) $(IMGOBJ) -lz

# the IMAP image-transcoding filter: just the filter + the picture converter
# and the shared image decoders, built with the sanitizers (as imgtest.py does)
XSRC = ../src/imapxcode.cpp ../src/imgconv.cpp
XHDR = ../src/imapxcode.h ../src/imgconv.h ../src/htmlsimp.h
XSAN = -fsanitize=address,undefined

test_imapxcode: test_imapxcode.cpp $(XSRC) $(XHDR) $(IMGOBJ) pictures
	$(CXX) $(CXXFLAGS) $(XSAN) $(INC) -o $@ test_imapxcode.cpp $(XSRC) $(IMGOBJ) -lz

tinfl.o: tinfl/tinfl.c tinfl/rom/miniz.h
	$(CC) -O1 -g -Wall -Itinfl -c -o $@ tinfl/tinfl.c

test_proxy_tinfl: test_proxy.cpp fakehal.h $(SRC) $(HDR) tinfl.o $(IMGOBJ)
	$(CXX) $(CXXFLAGS) $(INC) -DAM_TINFL -Itinfl -o $@ test_proxy.cpp $(SRC) tinfl.o $(IMGOBJ) -lz

# the test pictures for test_proxy (Pillow): a photo-like JPEG, a PNG and a
# GIF, each wider than the proxy's limit, and a small GIF that is passed on
pictures: pictures/photo.jpg
pictures/photo.jpg: mkpictures.py
	python3 mkpictures.py pictures

# the LCD's pages as pictures, from the same layout the AtomS3R draws
M5FONTS = ../.pio/libdeps/atoms3r/M5GFX/src/lgfx/Fonts
render_screens: render_screens.cpp fakehal.h $(SRC) $(HDR) ../src/fontmetrics.h $(IMGOBJ)
	$(CXX) $(CXXFLAGS) $(INC) -I$(M5FONTS) -Wno-narrowing -o $@ render_screens.cpp $(SRC) $(IMGOBJ) -lz
screens: render_screens
	./render_screens > /dev/null
	python3 mkscreens.py screens

# TLS for the proxy comes from the PC's OpenSSL 3 (libssl.so.3), loaded at
# run time, so no OpenSSL headers are needed to build
hostmodem: hostmodem.cpp $(SRC) $(HDR) $(IMGOBJ) ../src/cast/castpage.h
	$(CXX) $(CXXFLAGS) $(INC) -o $@ hostmodem.cpp $(SRC) $(IMGOBJ) -lz -ldl

# the codec as a shared library, for the end-to-end test's fake Psion (ctypes)
libpccodec.so: $(CODEC)/pccodec.c $(CODEC)/pccodec.h
	$(CC) -O2 -fPIC -shared -fno-strict-aliasing -I$(CODEC) -o $@ $< -lz

# PsiCast end to end: hostmodem, a fake Psion on both routes, WebSocket viewers
cast-e2e: hostmodem libpccodec.so
	python3 test_cast_e2e.py

clean:
	rm -f test_fwtrial test_cast libpccodec.so test_modem test_v22 test_atremote test_usbnet test_proxy test_proxy_tinfl test_imapxcode tinfl.o hostmodem render_screens $(IMGOBJ)
	rm -rf pictures screens

.PHONY: test clean pictures screens cast-e2e
