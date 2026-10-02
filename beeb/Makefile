# beeb/Makefile - the BBC Micro port (PLAN.md section 3.7).
#
#   make            the release disc and the harness discs
#   make run        start b2 (if it isn't running) and boot the release disc
#   make demo       the release disc in b2 at real speed, a window of its own (our b2
#                   stopped first; close it before the checks). make -B demo MAP=7: seven sprites
#   make phase0     the Phase 0 checks, on b2
#   make phase1     the Phase 1 checks, on b2
#   make phase2     check S2: every arithmetic routine against the C, on b2
#   make phase3     check S3: fill, clear and the cursor against the C, on b2
#   make phase4     check S4: the plotters on every golden pose, on b2
#   make phase5     check S5u and S5: the clip group and the lines, replayed on b2
#   make phase6     check S5u and S6: the walk replayed, and whole frames, on b2
#   make phase7     check S5u and S7: the sprites replayed, and whole frames with them, on b2
#   make goldens    the pose and input streams
#   make search     the pose search (a few minutes): poses the goldens miss
#   make coverage   the coverage gate over every stream
#   make record     the unit-replay records, each then replayed on the host
#   make cost       the frame-cost model over the golden poses
#   make gonogo     predicted frame times from measured costs (after phase4)
#   make profile    where the frames' cycles go: the PC sampled over golden frames, sprites on
#   make stop       stop our b2 (it runs at full speed, so a core stays busy)
#   make clean
#
# B2_VIDEO=offscreen runs b2 with no window; without it b2 opens one.

BARON ?= baron
PY ?= python3
BUILD = build
MAP ?= 0
TOOL = ../build/beeb/host/portalc_beebtool
DATA = data/map0.6502 data/map1.6502 data/map7.6502 data/map9.6502 data/shades.6502 data/sine.6502 \
    data/consts.6502
SRC = $(wildcard src/*.6502) $(wildcard test/*.6502) $(DATA)

all: $(BUILD)/portal.ssd $(BUILD)/harness0.ssd $(BUILD)/harness1.ssd $(BUILD)/harness0_map7.ssd \
    $(BUILD)/harness0_map9.ssd $(BUILD)/harness_s2.ssd $(BUILD)/harness_s2b.ssd $(BUILD)/harness_s34_0.ssd $(BUILD)/harness_s34_1.ssd \
    $(BUILD)/harness_prof.ssd $(BUILD)/harness_prof1.ssd $(BUILD)/harness_prof_map7.ssd $(BUILD)/harness_prof_map9.ssd

# the C side: the host tool, the data it exports, and the golden streams
tool:
	cmake --build ../build --target portalc_beebtool portalc_beebtool_cov portalc_beebtool_search \
	    portalc_beebtool_trace

$(DATA): $(TOOL)
	mkdir -p data
	$(TOOL) export data

$(TOOL): tool

goldens: tool | $(BUILD)
	mkdir -p $(BUILD)/golden
	$(TOOL) goldens $(BUILD)/golden

search: goldens
	../build/beeb/host/portalc_beebtool_search search $(BUILD)/golden 2000000 2> $(BUILD)/search.log

coverage: goldens
	host/coverage.sh

record: goldens
	../build/beeb/host/portalc_beebtool_trace record $(BUILD)/golden
	../build/beeb/host/portalc_beebtool_trace replaycheck $(BUILD)/golden

cost: goldens
	../build/beeb/host/portalc_beebtool_trace cost $(BUILD)/golden

gonogo: goldens
	../build/beeb/host/portalc_beebtool_trace cost $(BUILD)/golden $(BUILD)/counters.csv > /dev/null
	$(PY) host/gonogo.py $(BUILD)/counters.csv

$(BUILD):
	mkdir -p $(BUILD)/portal $(BUILD)/harness0 $(BUILD)/harness1 $(BUILD)/golden

$(BUILD)/portal.ssd: boot.6502 portal.6502 $(SRC) | $(BUILD)
	$(BARON) -v -D MAP=$(MAP) --symbols $(BUILD)/portal.json -p $(BUILD)/portal -o $@ \
	    --title PORTAL --opt 2 boot.6502 portal.6502 > $(BUILD)/portal.lst

# the release on another map: build/portal_map1.ssd starts in the second room
$(BUILD)/portal_map%.ssd: boot.6502 portal.6502 $(SRC) | $(BUILD)
	mkdir -p $(BUILD)/portal_map$*
	$(BARON) -v -D MAP=$* --symbols $(BUILD)/portal_map$*.json -p $(BUILD)/portal_map$* -o $@ \
	    --title PORTAL --opt 2 boot.6502 portal.6502 > $(BUILD)/portal_map$*.lst

# A harness carries the release code so far and one suite of checks (PLAN.md 5.3): S2's, S3's and
# S4's (SUITE 3), or (SUITE 0) the rest, on buffer A or B.
$(BUILD)/harness%.ssd: tboot.6502 harness.6502 $(SRC) | $(BUILD)
	$(BARON) -v -D MAP=$(MAP) -D TESTBUF=$* -D SUITE=0 --symbols $(BUILD)/harness$*.json -p $(BUILD)/harness$* \
	    -o $@ --title HARNESS --opt 2 tboot.6502 harness.6502 > $(BUILD)/harness$*.lst

# the seven-sprite map and the stress map, for the checks that need them
$(BUILD)/harness0_map%.ssd: tboot.6502 harness.6502 $(SRC) | $(BUILD)
	mkdir -p $(BUILD)/harness0_map$*
	$(BARON) -v -D MAP=$* -D TESTBUF=0 -D SUITE=0 --symbols $(BUILD)/harness0_map$*.json -p $(BUILD)/harness0_map$* \
	    -o $@ --title HARNESS --opt 2 tboot.6502 harness.6502 > $(BUILD)/harness0_map$*.lst

# S3's and S4's suite, on each buffer
$(BUILD)/harness_s34_%.ssd: tboot.6502 harness.6502 $(SRC) | $(BUILD)
	mkdir -p $(BUILD)/harness_s34_$*
	$(BARON) -v -D MAP=$(MAP) -D TESTBUF=$* -D SUITE=3 --symbols $(BUILD)/harness_s34_$*.json \
	    -p $(BUILD)/harness_s34_$* -o $@ --title HARNESS --opt 2 tboot.6502 harness.6502 > $(BUILD)/harness_s34_$*.lst

# whole frames' suite (S6, S7) and the profiler's: map 0 on buffer A, or B (1), or maps 7 and 9
$(BUILD)/harness_prof.ssd: tboot.6502 harness.6502 $(SRC) | $(BUILD)
	mkdir -p $(BUILD)/harness_prof
	$(BARON) -v -D MAP=0 -D TESTBUF=0 -D SUITE=4 --symbols $(BUILD)/harness_prof.json -p $(BUILD)/harness_prof \
	    -o $@ --title HARNESS --opt 2 tboot.6502 harness.6502 > $(BUILD)/harness_prof.lst

$(BUILD)/harness_prof1.ssd: tboot.6502 harness.6502 $(SRC) | $(BUILD)
	mkdir -p $(BUILD)/harness_prof1
	$(BARON) -v -D MAP=0 -D TESTBUF=1 -D SUITE=4 --symbols $(BUILD)/harness_prof1.json -p $(BUILD)/harness_prof1 \
	    -o $@ --title HARNESS --opt 2 tboot.6502 harness.6502 > $(BUILD)/harness_prof1.lst

$(BUILD)/harness_prof_map%.ssd: tboot.6502 harness.6502 $(SRC) | $(BUILD)
	mkdir -p $(BUILD)/harness_prof_map$*
	$(BARON) -v -D MAP=$* -D TESTBUF=0 -D SUITE=4 --symbols $(BUILD)/harness_prof_map$*.json \
	    -p $(BUILD)/harness_prof_map$* -o $@ --title HARNESS --opt 2 tboot.6502 harness.6502 \
	    > $(BUILD)/harness_prof_map$*.lst

# S2's suite, in two parts: the arithmetic (_s2) and the projections (_s2b)
$(BUILD)/harness_s2.ssd: tboot.6502 harness.6502 $(SRC) | $(BUILD)
	mkdir -p $(BUILD)/harness_s2
	$(BARON) -v -D MAP=$(MAP) -D TESTBUF=0 -D SUITE=2 -D S2_PART=1 --symbols $(BUILD)/harness_s2.json \
	    -p $(BUILD)/harness_s2 -o $@ --title HARNESS --opt 2 tboot.6502 harness.6502 > $(BUILD)/harness_s2.lst

$(BUILD)/harness_s2b.ssd: tboot.6502 harness.6502 $(SRC) | $(BUILD)
	mkdir -p $(BUILD)/harness_s2b
	$(BARON) -v -D MAP=$(MAP) -D TESTBUF=0 -D SUITE=2 -D S2_PART=2 --symbols $(BUILD)/harness_s2b.json \
	    -p $(BUILD)/harness_s2b -o $@ --title HARNESS --opt 2 tboot.6502 harness.6502 > $(BUILD)/harness_s2b.lst

# what the C says the Beeb should hold
$(BUILD)/expect: $(TOOL) | $(BUILD)
	mkdir -p $@
	$(TOOL) tables $@/tables.bin
	$(TOOL) expect 0 $@/map0.json
	$(TOOL) expect 7 $@/map7.json
	$(TOOL) expect 9 $@/map9.json
	touch $@

run: $(BUILD)/portal.ssd
	host/b2up.sh
	$(PY) host/b2.py boot $(BUILD)/portal.ssd

phase0: all
	host/b2up.sh
	$(PY) host/b2.py phase0

phase1: all $(BUILD)/expect
	host/b2up.sh
	$(PY) host/b2.py phase1

profile: $(BUILD)/harness_prof.ssd $(BUILD)/harness_prof_map7.ssd $(BUILD)/harness_prof_map9.ssd goldens
	host/b2up.sh
	$(PY) host/profile.py

phase7: $(BUILD)/harness0.ssd $(BUILD)/harness0_map7.ssd $(BUILD)/harness0_map9.ssd $(BUILD)/harness_prof.ssd \
    $(BUILD)/harness_prof1.ssd $(BUILD)/harness_prof_map7.ssd $(BUILD)/harness_prof_map9.ssd record
	host/b2up.sh
	$(PY) host/b2.py phase7

phase6: $(BUILD)/harness0.ssd $(BUILD)/harness0_map7.ssd $(BUILD)/harness0_map9.ssd $(BUILD)/harness_prof.ssd \
    $(BUILD)/harness_prof1.ssd $(BUILD)/harness_prof_map7.ssd $(BUILD)/harness_prof_map9.ssd record
	../build/beeb/host/portalc_beebtool_trace synthclose $(BUILD)/golden 1 4000
	host/b2up.sh
	$(PY) host/b2.py phase6

phase5: $(BUILD)/harness0.ssd $(BUILD)/harness0_map7.ssd $(BUILD)/harness0_map9.ssd record
	host/b2up.sh
	$(PY) host/b2.py phase5
	$(PY) host/b2.py phase5 transform-all
	$(PY) host/b2.py phase5 make-synthetic

phase4: $(BUILD)/harness_s34_0.ssd $(BUILD)/harness_s34_1.ssd goldens
	host/b2up.sh
	$(PY) host/b2.py phase4

phase3: $(BUILD)/harness_s34_0.ssd $(BUILD)/harness_s34_1.ssd tool
	host/b2up.sh
	$(PY) host/b2.py phase3

phase2: $(BUILD)/harness_s2.ssd $(BUILD)/harness_s2b.ssd tool
	host/b2up.sh
	$(PY) host/b2.py phase2

stop:
	-kill $$(cat $(BUILD)/b2.pid) 2>/dev/null
	-pkill -f '[p]ortalc-b2 --config=B/Acorn 1770 --no-limit-speed'
	rm -f $(BUILD)/b2.pid

DEMO_SSD = $(if $(filter 0,$(MAP)),$(BUILD)/portal.ssd,$(BUILD)/portal_map$(MAP).ssd)

demo: $(DEMO_SSD) stop
	SDL_AUDIODRIVER=dummy nohup /snap/bin/b2 --config-folder=$$HOME/snap/b2/common/portalc-b2 \
	    --config="B/Acorn 1770" -0 $(DEMO_SSD) -b > $(BUILD)/demo.log 2>&1 &

clean:
	rm -rf $(BUILD)

.PHONY: all tool goldens search coverage record cost run phase0 phase1 phase2 phase3 phase4 phase5 phase6 phase7 gonogo profile stop demo clean
