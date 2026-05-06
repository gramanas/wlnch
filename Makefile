PKGS    = wayland-client xkbcommon freetype2 fontconfig
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Iprotocols $(shell pkg-config --cflags $(PKGS))
LDLIBS  = $(shell pkg-config --libs $(PKGS)) -lrt

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

PROTO_DIR = protocols

PROTO_HEADERS = \
	$(PROTO_DIR)/wlr-layer-shell-unstable-v1-client-protocol.h \
	$(PROTO_DIR)/xdg-shell-client-protocol.h

PROTO_SOURCES = \
	$(PROTO_DIR)/wlr-layer-shell-unstable-v1-protocol.c \
	$(PROTO_DIR)/xdg-shell-protocol.c

PROTO_OBJS = $(PROTO_SOURCES:.c=.o)

OBJS = wlnch.o $(PROTO_OBJS)

all: wlnch

$(PROTO_DIR)/%-client-protocol.h: $(PROTO_DIR)/%.xml
	wayland-scanner client-header $< $@

$(PROTO_DIR)/%-protocol.c: $(PROTO_DIR)/%.xml
	wayland-scanner private-code $< $@

wlnch.o: wlnch.c config.h $(PROTO_HEADERS)
	$(CC) $(CFLAGS) -c -o $@ $<

$(PROTO_DIR)/%-protocol.o: $(PROTO_DIR)/%-protocol.c $(PROTO_HEADERS)
	$(CC) $(CFLAGS) -c -o $@ $<

wlnch: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

install: wlnch
	install -Dm755 wlnch $(DESTDIR)$(BINDIR)/wlnch

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/wlnch

clean:
	rm -f wlnch *.o $(PROTO_OBJS) $(PROTO_HEADERS) $(PROTO_SOURCES)

.PHONY: all install uninstall clean
