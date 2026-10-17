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
include sources.mk
SRC = $(FW_SRC)
HDR = $(FW_HDR)
IMG = $(FW_IMG)
IMGOBJ = $(FW_IMGOBJ)
INC = $(FW_INC)

test: test_modem test_v22 test_atremote test_usbnet test_proxy test_proxy_tinfl test_imapxcode test_fwtrial pictures
	./test_modem
	./test_v22
	./test_atremote
	./test_usbnet
	./test_proxy
	./test_proxy_tinfl
	./test_imapxcode
	./test_fwtrial

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
render_screens: render_screens.cpp shots.h fakehal.h $(SRC) $(HDR) ../src/fontmetrics.h $(IMGOBJ)
	$(CXX) $(CXXFLAGS) $(INC) -I$(M5FONTS) -Wno-narrowing -o $@ render_screens.cpp $(SRC) $(IMGOBJ) -lz
screens: render_screens
	./render_screens > /dev/null
	python3 mkscreens.py screens

# TLS for the proxy comes from the PC's OpenSSL 3 (libssl.so.3), loaded at
# run time, so no OpenSSL headers are needed to build
# (an extension module's test Makefile builds its own, with the module in)
hostmodem: hostmodem.cpp $(SRC) $(HDR) $(IMGOBJ)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ hostmodem.cpp $(SRC) $(IMGOBJ) -lz -ldl

clean:
	rm -f test_fwtrial test_modem test_v22 test_atremote test_usbnet test_proxy test_proxy_tinfl test_imapxcode tinfl.o hostmodem render_screens $(IMGOBJ)
	rm -rf pictures screens

.PHONY: test clean pictures screens
