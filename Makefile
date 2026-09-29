CC      ?= clang
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDFLAGS += -framework IOKit -framework CoreFoundation
PREFIX  ?= /usr/local

PROG    = sinowealth-mouse
ALIAS   = nos-m700
SRCS    = src/main.c src/sinowealth.c src/devices.c
HDRS    = src/sinowealth.h src/devices.h

all: $(PROG) $(ALIAS)

$(PROG): $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

# Backward-compatible name for v1 users: a symlink to the same binary.
$(ALIAS): $(PROG)
	ln -sf $(PROG) $(ALIAS)

install: $(PROG)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(PROG) $(DESTDIR)$(PREFIX)/bin/$(PROG)
	ln -sf $(PROG) $(DESTDIR)$(PREFIX)/bin/$(ALIAS)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(PROG) $(DESTDIR)$(PREFIX)/bin/$(ALIAS)

clean:
	rm -f $(PROG) $(ALIAS)

.PHONY: all install uninstall clean
