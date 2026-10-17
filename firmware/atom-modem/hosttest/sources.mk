# sources.mk - the firmware's portable sources for the host tests, from
# $(FW) (the firmware's directory; .. from here). Included by this Makefile
# and by an extension module's own test Makefile (src/ext.h), which adds its
# sources to these. MIT licence (see LICENSE at the top of the repository).
FW ?= ..
FW_IMG = $(FW)/../../mail/engine/img
FW_SRC = $(FW)/src/modem.cpp $(FW)/src/settings.cpp $(FW)/src/schema.cpp $(FW)/src/ext.cpp $(FW)/src/uplink.cpp $(FW)/src/proxy.cpp \
      $(FW)/src/wgconf.cpp $(FW)/src/netlist.cpp $(FW)/src/dnsrelay.cpp $(FW)/src/atremote.cpp \
      $(FW)/src/htmlsimp.cpp $(FW)/src/gzip.cpp $(FW)/src/imgconv.cpp $(FW)/src/ppp.cpp $(FW)/src/status.cpp $(FW)/src/canvas.cpp \
      $(FW)/src/button.cpp $(FW)/src/usbnet/ncm.cpp $(FW)/src/usbnet/ipheth.cpp
FW_HDR = $(FW)/src/canvas.h $(FW)/src/fontmetrics.h $(FW)/src/modem.h $(FW)/src/ext.h $(FW)/src/atremote.h $(FW)/src/wgconf.h \
      $(FW)/src/netlist.h $(FW)/src/dnsrelay.h $(FW)/src/status.h $(FW)/src/button.h $(FW)/src/ppp.h $(FW)/src/settings.h \
      $(FW)/src/schema.h $(FW)/src/uplink.h $(FW)/src/proxy.h $(FW)/src/htmlsimp.h $(FW)/src/gzip.h $(FW)/src/imgconv.h \
      $(FW)/src/usbnet/ncm.h $(FW)/src/usbnet/ipheth.h
# PsiMail's picture decoders, compiled as they are (zlib from the system
# here; pattern rules, so that including this sets no default goal)
FW_IMGOBJ = pmimg.o pmjpeg.o pmjprog.o pmpng.o pmgif.o picojpeg.o
FW_INC = -I$(FW)/src -I$(FW_IMG)

pm%.o: $(FW_IMG)/pm%.c $(FW_IMG)/pmimg.h
	$(CC) -O1 -g -Wall -I$(FW_IMG) -c -o $@ $<
pico%.o: $(FW_IMG)/pico%.c $(FW_IMG)/pico%.h
	$(CC) -O1 -g -Wall -I$(FW_IMG) -c -o $@ $<
