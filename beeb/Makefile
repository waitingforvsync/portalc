# beeb/Makefile - the BBC Micro port, for the Master 128.
#
#   make          build/portal.ssd: !BOOT (*OPT 4,2) and PORTAL
#   make run      boot it in b2 as a Master 128 (MOS 3.20): in the b2 that's
#                 running, if there is one, or else in a new one
#   make test     check the loader's tables, and the arithmetic macros, the
#                 vertex cache and the capture (in a build of their own,
#                 -D TEST=TRUE), against the C's, in b2: the one running, or one
#                 of its own with no window
#   make map      remake src/mapdata.6502 from the C's demo scene
#   make clean
#
# A running b2 is driven through its HTTP API on port 48075: reset into the
# config, then the disc mounted in drive 0 and booted with SHIFT held.

BARON ?= baron
B2 ?= /snap/bin/b2
PY ?= python3
BUILD = build
CBUILD = ../build

B2_CONFIG = Master 128 (MOS 3.20)
B2_URL = http://127.0.0.1:48075
B2_WIN = b2

SRC = boot.6502 portal.6502
DEPS = $(SRC) $(wildcard src/*.6502) $(wildcard test/*.6502)

all: $(BUILD)/portal.ssd

$(BUILD)/portal.ssd: $(DEPS) | $(BUILD)
	$(BARON) -v --symbols $(BUILD)/portal.json -o $@ --title PORTAL --opt 2 $(SRC) \
	    > $(BUILD)/portal.lst

$(BUILD)/portal_test.ssd: $(DEPS) | $(BUILD)
	$(BARON) -v -D TEST=TRUE --symbols $(BUILD)/portal_test.json -o $@ --title PORTAL --opt 2 $(SRC) \
	    > $(BUILD)/portal_test.lst

$(BUILD):
	mkdir -p $@

run: $(BUILD)/portal.ssd
	@if curl -sf -o /dev/null --max-time 2 '$(B2_URL)/peek/$(B2_WIN)/0/+1'; then \
	    curl -sf -o /dev/null -G --data-urlencode 'config=$(B2_CONFIG)' \
	        '$(B2_URL)/reset/$(B2_WIN)' && \
	    curl -sf -o /dev/null -H 'Content-Type: application/x.acorn.disc-image.ssd' \
	        --data-binary @$< '$(B2_URL)/run/$(B2_WIN)?name=portal.ssd'; \
	else \
	    nohup $(B2) --config='$(B2_CONFIG)' -0 $< -b > $(BUILD)/b2.log 2>&1 & \
	fi

test: $(BUILD)/portal.ssd $(BUILD)/portal_test.ssd
	cmake --build $(CBUILD) --target portalc_tables portalc_arithref portalc_vcacheref \
	    portalc_captureref
	$(CBUILD)/src/portalc_tables > $(BUILD)/tables.json
	$(PY) -B host/test_tables.py $(BUILD)/portal.ssd $(BUILD)/portal.json $(BUILD)/tables.json
	$(CBUILD)/src/portalc_arithref > $(BUILD)/arith.json
	$(CBUILD)/src/portalc_vcacheref > $(BUILD)/vcache.json
	$(CBUILD)/src/portalc_captureref > $(BUILD)/capture.json
	$(PY) -B host/test_build.py $(BUILD)/portal_test.ssd $(BUILD)/portal_test.json \
	    $(BUILD)/arith.json $(BUILD)/vcache.json $(BUILD)/capture.json

map:
	cmake --build $(CBUILD) --target portalc_mapasm
	$(CBUILD)/src/portalc_mapasm > src/mapdata.6502

clean:
	rm -rf $(BUILD)

.PHONY: all run test map clean
