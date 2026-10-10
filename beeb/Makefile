# beeb/Makefile - the BBC Micro port, for the Master 128.
#
#   make          build/portal.ssd: !BOOT (*OPT 4,2) and PORTAL
#   make run      boot it in b2 as a Master 128 (MOS 3.20): in the b2 that's
#                 running, if there is one, or else in a new one
#   make clean
#
# A running b2 is driven through its HTTP API on port 48075: reset into the
# config, then the disc mounted in drive 0 and booted with SHIFT held.

BARON ?= baron
B2 ?= /snap/bin/b2
BUILD = build

B2_CONFIG = Master 128 (MOS 3.20)
B2_URL = http://127.0.0.1:48075
B2_WIN = b2

SRC = boot.6502 portal.6502

all: $(BUILD)/portal.ssd

$(BUILD)/portal.ssd: $(SRC) | $(BUILD)
	$(BARON) -v --symbols $(BUILD)/portal.json -o $@ --title PORTAL --opt 2 $(SRC) \
	    > $(BUILD)/portal.lst

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

clean:
	rm -rf $(BUILD)

.PHONY: all run clean
