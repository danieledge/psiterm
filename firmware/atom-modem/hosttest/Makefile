# Host tests for the Atom modem (no ESP32 needed). MIT licence.
#   make test       the unit tests: the modem (AT parser, +++ guard times,
#                   pacing) and the web proxy with its HTML simplifier.
#                   PAGES=folder adds real saved pages (bbc.html,
#                   guardian.html, wiki.html, hn.html; see ../README.md)
#   test_proxy_tinfl is the proxy test again with the inflater the Atom uses
#                   (tinfl from miniz, as in the ESP32 ROM: tinfl/) in place
#                   of zlib, so its 32 KB wrapping window is tested too
#   make hostmodem  the modem as a PC program on a unix socket, for the
#                   Psion emulator's serial bridge (see hostmodem.cpp)
CXX ?= g++
CXXFLAGS ?= -O1 -g -Wall -Wextra -Wno-unused-parameter -std=c++17
SRC = ../src/modem.cpp ../src/proxy.cpp ../src/htmlsimp.cpp ../src/gzip.cpp
HDR = ../src/modem.h ../src/proxy.h ../src/htmlsimp.h ../src/gzip.h

test: test_modem test_proxy test_proxy_tinfl
	./test_modem
	./test_proxy
	./test_proxy_tinfl

test_modem: test_modem.cpp fakehal.h $(SRC) $(HDR)
	$(CXX) $(CXXFLAGS) -o $@ test_modem.cpp $(SRC) -lz

test_proxy: test_proxy.cpp fakehal.h $(SRC) $(HDR)
	$(CXX) $(CXXFLAGS) -o $@ test_proxy.cpp $(SRC) -lz

tinfl.o: tinfl/tinfl.c tinfl/rom/miniz.h
	$(CC) -O1 -g -Wall -Itinfl -c -o $@ tinfl/tinfl.c

test_proxy_tinfl: test_proxy.cpp fakehal.h $(SRC) $(HDR) tinfl.o
	$(CXX) $(CXXFLAGS) -DAM_TINFL -Itinfl -o $@ test_proxy.cpp $(SRC) tinfl.o -lz

# TLS for the proxy comes from the PC's OpenSSL 3 (libssl.so.3), loaded at
# run time, so no OpenSSL headers are needed to build
hostmodem: hostmodem.cpp $(SRC) $(HDR)
	$(CXX) $(CXXFLAGS) -o $@ hostmodem.cpp $(SRC) -lz -ldl

clean:
	rm -f test_modem test_proxy test_proxy_tinfl tinfl.o hostmodem

.PHONY: test clean
