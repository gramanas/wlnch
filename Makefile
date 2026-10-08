PKGS    = wayland-client xkbcommon freetype2 fontconfig
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Iprotocols $(shell pkg-config --cflags $(PKGS))
LDLIBS  = $(shell pkg-config --libs $(PKGS)) -lrt

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

PROTO_DIR = protocols

PROTO_HEADERS = \
	$(PROTO_DIR)/wlr-layer-shell-unstable-v1-client-protocol.h \
	$(PROTO_DIR)/xdg-shell-client-protocol.h \
	$(PROTO_DIR)/primary-selection-unstable-v1-client-protocol.h

PROTO_SOURCES = \
	$(PROTO_DIR)/wlr-layer-shell-unstable-v1-protocol.c \
	$(PROTO_DIR)/xdg-shell-protocol.c \
	$(PROTO_DIR)/primary-selection-unstable-v1-protocol.c

PROTO_OBJS = $(PROTO_SOURCES:.c=.o)

WLNCH_OBJS     = wlnch.o     $(PROTO_OBJS)
WLNCH_IN_OBJS  = wlnch-in.o  $(PROTO_OBJS)
WLNCH_OUT_OBJS = wlnch-out.o $(PROTO_OBJS)

all: wlnch wlnch-in wlnch-out

$(PROTO_DIR)/%-client-protocol.h: $(PROTO_DIR)/%.xml
	wayland-scanner client-header $< $@

$(PROTO_DIR)/%-protocol.c: $(PROTO_DIR)/%.xml
	wayland-scanner private-code $< $@

wlnch.o: wlnch.c config.h $(PROTO_HEADERS)
	$(CC) $(CFLAGS) -c -o $@ $<

wlnch-in.o: wlnch-in.c config.h $(PROTO_HEADERS)
	$(CC) $(CFLAGS) -c -o $@ $<

wlnch-out.o: wlnch-out.c config.h $(PROTO_HEADERS)
	$(CC) $(CFLAGS) -c -o $@ $<

$(PROTO_DIR)/%-protocol.o: $(PROTO_DIR)/%-protocol.c $(PROTO_HEADERS)
	$(CC) $(CFLAGS) -c -o $@ $<

wlnch: $(WLNCH_OBJS)
	$(CC) $(CFLAGS) -o $@ $(WLNCH_OBJS) $(LDLIBS)

wlnch-in: $(WLNCH_IN_OBJS)
	$(CC) $(CFLAGS) -o $@ $(WLNCH_IN_OBJS) $(LDLIBS)

wlnch-out: $(WLNCH_OUT_OBJS)
	$(CC) $(CFLAGS) -o $@ $(WLNCH_OUT_OBJS) $(LDLIBS)

install: wlnch wlnch-in wlnch-out
	install -Dm755 wlnch     $(DESTDIR)$(BINDIR)/wlnch
	install -Dm755 wlnch-in  $(DESTDIR)$(BINDIR)/wlnch-in
	install -Dm755 wlnch-out $(DESTDIR)$(BINDIR)/wlnch-out

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/wlnch
	rm -f $(DESTDIR)$(BINDIR)/wlnch-in
	rm -f $(DESTDIR)$(BINDIR)/wlnch-out

clean:
	rm -f wlnch wlnch-in wlnch-out *.o $(PROTO_OBJS) $(PROTO_HEADERS) $(PROTO_SOURCES)

.PHONY: all install uninstall clean
