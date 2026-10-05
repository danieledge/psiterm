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
#   make hostmodem  the modem as a PC program on a unix socket, for the
#                   Psion emulator's serial bridge (see hostmodem.cpp)
CXX ?= g++
CC ?= gcc
CXXFLAGS ?= -O1 -g -Wall -Wextra -Wno-unused-parameter -std=c++17
IMG = ../../../mail/engine/img
SRC = ../src/modem.cpp ../src/settings.cpp ../src/schema.cpp ../src/uplink.cpp ../src/proxy.cpp \
      ../src/htmlsimp.cpp ../src/gzip.cpp ../src/imgconv.cpp ../src/usbnet/ncm.cpp ../src/usbnet/ipheth.cpp
HDR = ../src/modem.h ../src/settings.h ../src/schema.h ../src/uplink.h ../src/proxy.h ../src/htmlsimp.h \
      ../src/gzip.h ../src/imgconv.h ../src/usbnet/ncm.h ../src/usbnet/ipheth.h
IMGSRC = $(IMG)/pmimg.c $(IMG)/pmjpeg.c $(IMG)/pmjprog.c $(IMG)/pmpng.c $(IMG)/pmgif.c $(IMG)/picojpeg.c
IMGOBJ = pmimg.o pmjpeg.o pmjprog.o pmpng.o pmgif.o picojpeg.o
INC = -I../src -I$(IMG)

test: test_modem test_usbnet test_proxy test_proxy_tinfl pictures
	./test_modem
	./test_usbnet
	./test_proxy
	./test_proxy_tinfl

# the decoders PsiMail uses, compiled as they are (zlib from the system here)
pm%.o: $(IMG)/pm%.c $(IMG)/pmimg.h
	$(CC) -O1 -g -Wall -I$(IMG) -c -o $@ $<
picojpeg.o: $(IMG)/picojpeg.c $(IMG)/picojpeg.h
	$(CC) -O1 -g -Wall -I$(IMG) -c -o $@ $<

test_modem: test_modem.cpp fakehal.h $(SRC) $(HDR) $(IMGOBJ)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ test_modem.cpp $(SRC) $(IMGOBJ) -lz

test_usbnet: test_usbnet.cpp ../src/usbnet/ncm.cpp ../src/usbnet/ncm.h ../src/usbnet/ipheth.cpp ../src/usbnet/ipheth.h
	$(CXX) $(CXXFLAGS) $(INC) -o $@ test_usbnet.cpp ../src/usbnet/ncm.cpp ../src/usbnet/ipheth.cpp

test_proxy: test_proxy.cpp fakehal.h $(SRC) $(HDR) $(IMGOBJ)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ test_proxy.cpp $(SRC) $(IMGOBJ) -lz

tinfl.o: tinfl/tinfl.c tinfl/rom/miniz.h
	$(CC) -O1 -g -Wall -Itinfl -c -o $@ tinfl/tinfl.c

test_proxy_tinfl: test_proxy.cpp fakehal.h $(SRC) $(HDR) tinfl.o $(IMGOBJ)
	$(CXX) $(CXXFLAGS) $(INC) -DAM_TINFL -Itinfl -o $@ test_proxy.cpp $(SRC) tinfl.o $(IMGOBJ) -lz

# the test pictures for test_proxy (Pillow): a photo-like JPEG, a PNG and a
# GIF, each wider than the proxy's limit, and a small GIF that is passed on
pictures: pictures/photo.jpg
pictures/photo.jpg: mkpictures.py
	python3 mkpictures.py pictures

# TLS for the proxy comes from the PC's OpenSSL 3 (libssl.so.3), loaded at
# run time, so no OpenSSL headers are needed to build
hostmodem: hostmodem.cpp $(SRC) $(HDR) $(IMGOBJ)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ hostmodem.cpp $(SRC) $(IMGOBJ) -lz -ldl

clean:
	rm -f test_modem test_usbnet test_proxy test_proxy_tinfl tinfl.o hostmodem $(IMGOBJ)
	rm -rf pictures

.PHONY: test clean pictures
