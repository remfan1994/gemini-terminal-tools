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
	./$(TARGET) --help | grep -q -- '--update-models'
	./$(TARGET) --help | grep -q -- '--test-model MODEL'
	./$(TARGET) --help | grep -q -- '--loopback-file FILE'
	./$(TARGET) --help | grep -q -- '--prompt TEXT'
	./$(TARGET) --help | grep -q -- '--input FILE'
	./$(TARGET) --help | grep -q -- '--session SESSION'
	./$(TARGET) --help | grep -q -- '--rename-session SESSION TITLE'
	./$(TARGET) --help | grep -q -- 'SESSION_AUTO_TITLE=1'
	grep -q '^OPTIONS' ttychatter.1
	grep -q -- '--update-models' ttychatter.1
	grep -q -- '--loopback-file FILE' ttychatter.1
	grep -q -- '--prompt TEXT' ttychatter.1
	grep -q -- '--input input.txt' ttychatter.1
	grep -q -- '--rename-session SESSION TITLE' ttychatter.1
	grep -q -- 'SESSION_TITLE_MODEL' ttychatter.1
	grep -q -- 'SESSION_AUTO_TITLE=1' README
	grep -q -- '--prompt' WALKTHROUGH
	tmp=$$(mktemp -d); \
	XDG_CONFIG_HOME=$$tmp/config XDG_DATA_HOME=$$tmp/data XDG_CACHE_HOME=$$tmp/cache ./$(TARGET) --demo --prompt 'make check auto session' >/dev/null; \
	test $$(find $$tmp/data/ttychatter/openrouter/sessions -type f -name 'session-*.log' | wc -l) -eq 1; \
	rm -rf $$tmp
	tmp=$$(mktemp -d); \
	mkdir -p $$tmp/config/ttychatter/openrouter; \
	printf 'SESSION_AUTO_TITLE=1\nSESSION_TITLE_MODEL=demo/title\nSESSION_TITLE_MAX_WORDS=4\n' > $$tmp/config/ttychatter/openrouter/config; \
	XDG_CONFIG_HOME=$$tmp/config XDG_DATA_HOME=$$tmp/data XDG_CACHE_HOME=$$tmp/cache ./$(TARGET) --demo --prompt 'make check generated title smoke' >/dev/null; \
	XDG_CONFIG_HOME=$$tmp/config XDG_DATA_HOME=$$tmp/data XDG_CACHE_HOME=$$tmp/cache ./$(TARGET) --list | grep -q 'Make check generated title'; \
	rm -rf $$tmp

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
