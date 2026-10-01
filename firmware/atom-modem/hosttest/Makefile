# Host tests for the Atom modem (no ESP32 needed). MIT licence.
#   make test       the unit tests (AT parser, +++ guard times, pacing)
#   make hostmodem  the modem as a PC program on a unix socket, for the
#                   Psion emulator's serial bridge (see hostmodem.cpp)
CXX ?= g++
CXXFLAGS ?= -O1 -g -Wall -Wextra -Wno-unused-parameter -std=c++17

test: test_modem
	./test_modem

test_modem: test_modem.cpp fakehal.h ../src/modem.cpp ../src/modem.h
	$(CXX) $(CXXFLAGS) -o $@ test_modem.cpp ../src/modem.cpp

hostmodem: hostmodem.cpp ../src/modem.cpp ../src/modem.h
	$(CXX) $(CXXFLAGS) -o $@ hostmodem.cpp ../src/modem.cpp

clean:
	rm -f test_modem hostmodem

.PHONY: test clean
