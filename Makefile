CC      ?= clang
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDFLAGS += -framework IOKit -framework CoreFoundation

nos-m700: src/main.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

install: nos-m700
	install -m 755 nos-m700 /usr/local/bin/nos-m700

clean:
	rm -f nos-m700

.PHONY: install clean
