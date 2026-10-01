#!/bin/sh
# b2up.sh - start b2 for the port's checks (PLAN.md section 5.1), unless
# our instance is already running.
#
# Our instance uses its own config folder (so Rich's b2 settings are never
# touched), the stock "B/Acorn 1770" Model B, unlimited speed and dummy
# audio, and records its pid in build/b2.pid. If something else already
# holds b2's HTTP port (48075), this refuses: b2.py would otherwise drive
# someone's own session.

set -e
cd "$(dirname "$0")/.."
CONF="$HOME/snap/b2/common/portalc-b2"
PIDF=build/b2.pid
mkdir -p build "$CONF"

ours() {
    [ -f "$PIDF" ] && kill -0 "$(cat "$PIDF")" 2>/dev/null
}

if python3 host/b2.py alive; then
    if ours; then
        exit 0
    fi
    echo "b2up: something else is listening on 48075 (not the b2 in $PIDF); refusing" >&2
    exit 1
fi

# B2_VIDEO=offscreen tries running without a window (PLAN.md D5)
if [ -n "$B2_VIDEO" ]; then
    export SDL_VIDEODRIVER="$B2_VIDEO"
fi
SDL_AUDIODRIVER=dummy nohup /snap/bin/b2 --config-folder="$CONF" --config="B/Acorn 1770" \
    --no-limit-speed -e http > build/b2.log 2>&1 &
echo $! > "$PIDF"
python3 host/b2.py waitup 60
