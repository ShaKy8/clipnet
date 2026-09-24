# clipnetd: the CLIP//NET clipboard daemon.
#
#   make          build ./clipnetd
#   make test     build and run the unit tests
#   make test-live  round trip through the real clipboard (needs a session)
#   make asan     unit tests under AddressSanitizer + UBSan
#   make clean

PKGS     := wayland-client sqlite3 lua5.4
CC       ?= cc
CFLAGS   ?= -O2 -g -pipe
# `override` so `make CFLAGS=...` (as the asan target does) adds to these
# instead of dropping them.
override CFLAGS   += -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wshadow -Wno-unused-parameter \
                     -Wmissing-prototypes -Wstrict-prototypes -fno-strict-aliasing
override CPPFLAGS += -Isrc -Ivendor -Igen $(shell pkg-config --cflags $(PKGS))
override LDLIBS   += $(shell pkg-config --libs $(PKGS)) -lm

WL_PROTOCOLS := $(shell pkg-config --variable=pkgdatadir wayland-protocols)
EXT_DC       := $(WL_PROTOCOLS)/staging/ext-data-control/ext-data-control-v1.xml
HYPR_GS      := protocols/hyprland-global-shortcuts-v1.xml

GEN_H := gen/ext-data-control-v1.h gen/hyprland-global-shortcuts-v1.h
GEN_C := gen/ext-data-control-v1.c gen/hyprland-global-shortcuts-v1.c

SRC  := $(filter-out src/main.c,$(wildcard src/*.c))
OBJ  := $(SRC:src/%.c=build/%.o) $(GEN_C:gen/%.c=build/gen-%.o) build/cJSON.o
TESTS := $(patsubst tests/%.c,build/tests/%,$(filter-out tests/harness.c,$(wildcard tests/test_*.c)))

all: clipnetd

clipnetd: build/main.o $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

gen/ext-data-control-v1.h: $(EXT_DC) | gen
	wayland-scanner client-header $< $@
gen/ext-data-control-v1.c: $(EXT_DC) | gen
	wayland-scanner private-code $< $@
gen/hyprland-global-shortcuts-v1.h: $(HYPR_GS) | gen
	wayland-scanner client-header $< $@
gen/hyprland-global-shortcuts-v1.c: $(HYPR_GS) | gen
	wayland-scanner private-code $< $@

build/%.o: src/%.c $(GEN_H) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<
build/gen-%.o: gen/%.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wno-missing-prototypes -c -o $@ $<
build/cJSON.o: vendor/cJSON.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -w -c -o $@ $<

build/tests/%: tests/%.c tests/harness.c tests/harness.h $(OBJ) | build/tests
	$(CC) $(CPPFLAGS) -Itests $(CFLAGS) -Wno-missing-prototypes -o $@ $< tests/harness.c $(OBJ) $(LDLIBS)

build/tests/offer: tests/tools/offer.c build/gen-ext-data-control-v1.o | build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wno-missing-prototypes -o $@ $^ $(LDLIBS)

test: $(TESTS)
	@set -e; for t in $(TESTS); do $$t; done

# Against the live Wayland session (saves and restores the clipboard).
test-live: clipnetd build/tests/offer
	../tests/integration/live_test.sh

asan:
	$(MAKE) clean
	$(MAKE) test CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
	             LDFLAGS="-fsanitize=address,undefined"
	$(MAKE) clean

gen build build/tests:
	mkdir -p $@

clean:
	rm -rf build gen clipnetd

.PHONY: all test test-live asan clean
-include $(wildcard build/*.d)
