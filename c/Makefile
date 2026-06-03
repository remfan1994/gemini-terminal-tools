# ttychatter C command-line client Makefile
#
# This Makefile intentionally avoids Autotools/CMake for the first C command-line
# release.  The project goal here is a traditional small Unix program that can be
# built by reading one short file and typing `make`.  pkg-config is still used so
# the build inherits the correct compiler/linker flags for libcurl and json-c on
# Debian, Arch, BSD-ish systems, and other Unix-like distributions.

CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g -Wall -Wextra -Wpedantic
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags libcurl json-c 2>/dev/null)
LDLIBS += $(shell $(PKG_CONFIG) --libs libcurl json-c 2>/dev/null)

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man/man1

TARGET = ttychatter
SRC = src/ttychatter.c

.PHONY: all clean install uninstall check

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

check: $(TARGET)
	$(CC) $(CPPFLAGS) -fsyntax-only $(CFLAGS) $(SRC)
	./$(TARGET) --version
	./$(TARGET) --help >/dev/null

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -d $(DESTDIR)$(MANDIR)
	install -m 0644 ttychatter.1 $(DESTDIR)$(MANDIR)/ttychatter.1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	rm -f $(DESTDIR)$(MANDIR)/ttychatter.1

clean:
	rm -f $(TARGET) *.o src/*.o
