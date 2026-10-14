# beeb/Makefile - the BBC Micro port, for the Master 128.
#
#   make          build/portal.ssd: !BOOT (*OPT 4,2) and PORTAL
#   make run      boot it in b2 as a Master 128 (MOS 3.20): in the b2 that's
#                 running, if there is one, or else in a new one
#   make test     all the checks below, against the C's, in b2: the one
#                 running, or one of its own with no window
#   make test_tables     the loader's tables, in PORTAL itself
#   make test_arith      the arithmetic macros, in a program of their own
#   make test_vcache     the vertex cache, likewise
#   make test_capture    the capture, likewise
#   make test_plot       the plotters and the unplot, likewise
#   make test_clip       the clips, likewise
#   make test_walk       the walk, the whole capture, likewise; and test_walk3
#                        over the stress scene, its depth capped at 3
#   make test_fill       the fill, likewise: each test/CHECK_portal.6502,
#                        built with -D TEST=TRUE
#   make map      remake src/mapdata.6502 from the C's demo scene, and
#                 test/stressdata.6502 from its stress scene
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
CHECKS = arith vcache capture plot clip walk walk3 fill

all: $(BUILD)/portal.ssd

$(BUILD)/portal.ssd: $(DEPS) | $(BUILD)
	$(BARON) -v --symbols $(BUILD)/portal.json -o $@ --title PORTAL --opt 2 $(SRC) \
	    > $(BUILD)/portal.lst

$(BUILD)/test_%.ssd: $(DEPS) | $(BUILD)
	$(BARON) -v -D TEST=TRUE --symbols $(BUILD)/test_$*.json -o $@ --title PORTAL --opt 2 \
	    boot.6502 test/$*_portal.6502 > $(BUILD)/test_$*.lst

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

test: test_tables $(CHECKS:%=test_%)

test_tables: $(BUILD)/portal.ssd
	cmake --build $(CBUILD) --target portalc_tables
	$(CBUILD)/src/portalc_tables > $(BUILD)/tables.json
	$(PY) -B host/test_tables.py $(BUILD)/portal.ssd $(BUILD)/portal.json $(BUILD)/tables.json

# a check against its C tool's answers: portalc_arithref for test_arith, and so on
$(CHECKS:%=test_%): test_%: $(BUILD)/test_%.ssd
	cmake --build $(CBUILD) --target portalc_$*ref
	$(CBUILD)/src/portalc_$*ref > $(BUILD)/$*.json
	$(PY) -B host/test_build.py $* $< $(BUILD)/test_$*.json $(BUILD)/$*.json

map:
	cmake --build $(CBUILD) --target portalc_mapasm
	$(CBUILD)/src/portalc_mapasm > src/mapdata.6502
	$(CBUILD)/src/portalc_mapasm --stress > test/stressdata.6502

clean:
	rm -rf $(BUILD)

.PHONY: all run test test_tables $(CHECKS:%=test_%) map clean
