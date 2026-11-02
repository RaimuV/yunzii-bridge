CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu11
LDLIBS   = -lusb-1.0 -lpthread
PREFIX  ?= /usr/local

BIN = yunzii-bridge

all: $(BIN)

$(BIN): $(BIN).c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/sbin/$(BIN)

clean:
	rm -f $(BIN)

.PHONY: all install clean
