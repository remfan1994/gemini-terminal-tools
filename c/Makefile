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
	./$(TARGET) --help | grep -q -- '--stream / --no-stream'
	./$(TARGET) --help | grep -q -- '--progress / --no-progress'
	./$(TARGET) --help | grep -q -- '--voice-input FILE'
	./$(TARGET) --help | grep -q -- '--speak / --no-speak'
	./$(TARGET) --help | grep -q -- '--turns SESSION'
	./$(TARGET) --help | grep -q -- '--branch SESSION TURN'
	./$(TARGET) --help | grep -q -- '--edit-turn SESSION TURN'
	./$(TARGET) --help | grep -q -- '--export SESSION'
	./$(TARGET) --help | grep -q -- '--format FORMAT'
	./$(TARGET) --help | grep -q -- '--autoscan-model'
	set -e; opts=$$(sed -n 's/^[[:space:]]*{\"\([^\"]*\)\",.*/--\1/p' $(SRC)); \
	for opt in $$opts; do \
		./$(TARGET) --help | grep -q -- "$$opt" || { echo "missing $$opt in --help"; exit 1; }; \
		grep -q -- "$$opt" ttychatter.1 || { echo "missing $$opt in man page"; exit 1; }; \
		grep -q -- "$$opt" README || { echo "missing $$opt in README"; exit 1; }; \
		grep -q -- "$$opt" WALKTHROUGH || { echo "missing $$opt in WALKTHROUGH"; exit 1; }; \
	done
	grep -q '^OPTIONS' ttychatter.1
	grep -q -- '--update-models' ttychatter.1
	grep -q -- '--loopback-file FILE' ttychatter.1
	grep -q -- '--prompt TEXT' ttychatter.1
	grep -q -- '--input input.txt' ttychatter.1
	grep -q -- '--rename-session SESSION TITLE' ttychatter.1
	grep -q -- 'SESSION_TITLE_MODEL' ttychatter.1
	grep -q -- '--stream' ttychatter.1
	grep -q -- '--branch SESSION TURN' ttychatter.1
	grep -q -- '--edit-turn SESSION TURN' ttychatter.1
	grep -q -- '--export SESSION' ttychatter.1
	grep -q -- 'VOICE_INPUT_CMD' ttychatter.1
	grep -q -- 'STREAMING AND STATUS' ttychatter.1
	grep -q -- 'BRANCHING, EDITING, AND EXPORT' ttychatter.1
	grep -q -- '--autoscan-model' ttychatter.1
	grep -q -- 'SESSION_AUTO_TITLE=1' README
	grep -q -- '--stream' README
	grep -q -- '--voice-input' README
	grep -q -- '--branch notes 2' README
	grep -q -- '--autoscan-model' README
	grep -q -- '--prompt' WALKTHROUGH
	grep -q -- '--stream' WALKTHROUGH
	grep -q -- '--voice-input' WALKTHROUGH
	grep -q -- ':branch 2' WALKTHROUGH
	grep -q -- '--autoscan-model' WALKTHROUGH
	grep -q -- 'XDG_CONFIG_HOME' README
	grep -q -- 'XDG_CONFIG_HOME' ttychatter.1
	grep -q -- 'Interactive command equivalents' README
	tmp=$$(mktemp -d); \
	printf ':help\n:q\n' | XDG_CONFIG_HOME=$$tmp/config XDG_DATA_HOME=$$tmp/data XDG_CACHE_HOME=$$tmp/cache ./$(TARGET) --demo --interactive --new > $$tmp/runtime-help; \
	grep -q -- ':fork N' $$tmp/runtime-help; \
	grep -q -- ':edit-turn N' $$tmp/runtime-help; \
	grep -q -- ':progress \[on|off\]' $$tmp/runtime-help; \
	grep -q -- ':transcribe FILE' $$tmp/runtime-help; \
	rm -rf $$tmp
	tmp=$$(mktemp -d); \
	XDG_CONFIG_HOME=$$tmp/config XDG_DATA_HOME=$$tmp/data XDG_CACHE_HOME=$$tmp/cache ./$(TARGET) --demo --prompt 'make check auto session' >/dev/null; \
	test $$(find $$tmp/data/ttychatter/openrouter/sessions -type f -name 'session-*.log' | wc -l) -eq 1; \
	rm -rf $$tmp
	tmp=$$(mktemp -d); \
	XDG_CONFIG_HOME=$$tmp/config XDG_DATA_HOME=$$tmp/data XDG_CACHE_HOME=$$tmp/cache ./$(TARGET) --set MODEL=demo/xdg >/dev/null; \
	test -f $$tmp/config/ttychatter/openrouter/config; \
	XDG_CONFIG_HOME=$$tmp/config XDG_DATA_HOME=$$tmp/data XDG_CACHE_HOME=$$tmp/cache ./$(TARGET) --config | grep -q 'model=demo/xdg'; \
	XDG_CONFIG_HOME=$$tmp/config XDG_DATA_HOME=$$tmp/data XDG_CACHE_HOME=$$tmp/cache ./$(TARGET) --demo --update-models >/dev/null; \
	test -f $$tmp/cache/ttychatter/openrouter/models.json; \
	rm -rf $$tmp
	tmp=$$(mktemp -d); \
	mkdir -p $$tmp/config/ttychatter/openrouter; \
	printf 'SESSION_AUTO_TITLE=1\nSESSION_TITLE_MODEL=demo/title\nSESSION_TITLE_MAX_WORDS=4\n' > $$tmp/config/ttychatter/openrouter/config; \
	XDG_CONFIG_HOME=$$tmp/config XDG_DATA_HOME=$$tmp/data XDG_CACHE_HOME=$$tmp/cache ./$(TARGET) --demo --prompt 'make check generated title smoke' >/dev/null; \
	XDG_CONFIG_HOME=$$tmp/config XDG_DATA_HOME=$$tmp/data XDG_CACHE_HOME=$$tmp/cache ./$(TARGET) --list | grep -q 'Make check generated title'; \
	rm -rf $$tmp
	tmp=$$(mktemp -d); \
	XDG_CONFIG_HOME=$$tmp/config XDG_DATA_HOME=$$tmp/data XDG_CACHE_HOME=$$tmp/cache ./$(TARGET) --demo --prompt 'first branch message' --session branchdemo >/dev/null; \
	XDG_CONFIG_HOME=$$tmp/config XDG_DATA_HOME=$$tmp/data XDG_CACHE_HOME=$$tmp/cache ./$(TARGET) --demo --prompt 'second branch message' --session branchdemo >/dev/null; \
	XDG_CONFIG_HOME=$$tmp/config XDG_DATA_HOME=$$tmp/data XDG_CACHE_HOME=$$tmp/cache ./$(TARGET) --turns branchdemo | grep -q 'second branch message'; \
	branch=$$(XDG_CONFIG_HOME=$$tmp/config XDG_DATA_HOME=$$tmp/data XDG_CACHE_HOME=$$tmp/cache ./$(TARGET) --branch branchdemo 2); \
	test -f $$branch; \
	! grep -q 'second branch message' $$branch; \
	branch2=$$(XDG_CONFIG_HOME=$$tmp/config XDG_DATA_HOME=$$tmp/data XDG_CACHE_HOME=$$tmp/cache ./$(TARGET) --branch branchdemo:2); \
	test -f $$branch2; \
	XDG_CONFIG_HOME=$$tmp/config XDG_DATA_HOME=$$tmp/data XDG_CACHE_HOME=$$tmp/cache ./$(TARGET) --rename-session branchdemo Friendly Branch Demo >/dev/null; \
	XDG_CONFIG_HOME=$$tmp/config XDG_DATA_HOME=$$tmp/data XDG_CACHE_HOME=$$tmp/cache ./$(TARGET) --search-sessions 'Friendly Branch' | grep -q 'title: Friendly Branch Demo'; \
	XDG_CONFIG_HOME=$$tmp/config XDG_DATA_HOME=$$tmp/data XDG_CACHE_HOME=$$tmp/cache ./$(TARGET) --search-sessions 'branchdemo.log' | grep -q 'file: branchdemo.log'; \
	XDG_CONFIG_HOME=$$tmp/config XDG_DATA_HOME=$$tmp/data XDG_CACHE_HOME=$$tmp/cache ./$(TARGET) --export branchdemo --format markdown | grep -q 'first branch message'; \
	rm -rf $$tmp
	tmp=$$(mktemp -d); \
	XDG_CONFIG_HOME=$$tmp/config XDG_DATA_HOME=$$tmp/data XDG_CACHE_HOME=$$tmp/cache ./$(TARGET) --demo --prompt 'original edit turn' --session editdemo >/dev/null; \
	printf '#!/bin/sh\nprintf edited-turn-from-make-check > "$$1"\n' > $$tmp/editor.sh; chmod +x $$tmp/editor.sh; \
	EDITOR=$$tmp/editor.sh XDG_CONFIG_HOME=$$tmp/config XDG_DATA_HOME=$$tmp/data XDG_CACHE_HOME=$$tmp/cache ./$(TARGET) --demo --edit-turn editdemo 1 >/dev/null; \
	grep -R -q 'edited-turn-from-make-check' $$tmp/data/ttychatter/openrouter/sessions; \
	rm -rf $$tmp
	tmp=$$(mktemp -d); \
	mkdir -p $$tmp/config/ttychatter/openrouter; \
	printf 'VOICE_INPUT_CMD=cat %%f\nVOICE_OUTPUT_CMD=cat > %s/spoken.txt\n' $$tmp > $$tmp/config/ttychatter/openrouter/config; \
	printf 'voice make check prompt' > $$tmp/audio.txt; \
	XDG_CONFIG_HOME=$$tmp/config XDG_DATA_HOME=$$tmp/data XDG_CACHE_HOME=$$tmp/cache ./$(TARGET) --demo --voice-input $$tmp/audio.txt --speak >/dev/null; \
	test -s $$tmp/spoken.txt; \
	grep -R -q 'voice make check prompt' $$tmp/data/ttychatter/openrouter/sessions; \
	rm -rf $$tmp
	tmp=$$(mktemp -d); \
	XDG_CONFIG_HOME=$$tmp/config XDG_DATA_HOME=$$tmp/data XDG_CACHE_HOME=$$tmp/cache ./$(TARGET) --demo --stream --prompt 'stream make check prompt' > $$tmp/out; \
	grep -q 'Demo response' $$tmp/out; \
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
