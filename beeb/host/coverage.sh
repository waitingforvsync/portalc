#!/bin/sh
# coverage.sh - the coverage gate (PLAN.md 5.5): run every golden stream
# through the coverage build of beebtool, then list the lines of the ported
# engine code (arith.c, camera.c, render.c) that no golden reaches.
#
#   host/coverage.sh            summary, and build/coverage/uncovered.txt;
#                               fails on any uncovered line host/uncovered.txt
#                               doesn't give a reason for

set -e
cd "$(dirname "$0")/.."
TOOL=../build/beeb/host/portalc_beebtool_cov
OUT=build/coverage
G=build/golden
LLVM_PROFDATA=${LLVM_PROFDATA:-llvm-profdata-21}
LLVM_COV=${LLVM_COV:-llvm-cov-21}
rm -rf "$OUT"
mkdir -p "$OUT"

run() {
    LLVM_PROFILE_FILE="$OUT/%p.profraw" "$TOOL" "$@"
}
run render 0 $G/sweep_poses.bin $OUT/frames.bin
run render 7 $G/map7_poses.bin $OUT/frames.bin
run render 0 $G/rim_poses.bin $OUT/frames.bin
run render 9 $G/stress_poses.bin $OUT/frames.bin
for m in 0 7 9; do
    [ -f $G/search${m}_poses.bin ] && run render $m $G/search${m}_poses.bin $OUT/frames.bin
done
run camera 0 $G/lcg_inputs.bin $OUT/cam.bin
run camera 0 $G/combo_inputs.bin $OUT/cam.bin
run camera 9 $G/lcg_inputs.bin $OUT/cam.bin
rm -f $OUT/frames.bin $OUT/cam.bin

$LLVM_PROFDATA merge -sparse $OUT/*.profraw -o $OUT/merged.profdata
SRC=$(cd ../src && pwd)
$LLVM_COV report "$TOOL" -instr-profile=$OUT/merged.profdata \
    "$SRC/arith.c" "$SRC/camera.c" "$SRC/render.c" "$SRC/sprite.c"
$LLVM_COV show "$TOOL" -instr-profile=$OUT/merged.profdata \
    "$SRC/arith.c" "$SRC/camera.c" "$SRC/render.c" -show-line-counts \
    > $OUT/show.txt
status=0
python3 host/uncovered.py $OUT/show.txt host/uncovered.txt > $OUT/uncovered.txt || status=1
echo "uncovered lines: $(grep -c '^[ !] ' $OUT/uncovered.txt || true) (build/coverage/uncovered.txt)"
if [ $status -ne 0 ]; then
    grep -v '^[ a-z]' $OUT/uncovered.txt | grep -v ': ' || true
    grep '^now covered\|not in the reviewed' $OUT/uncovered.txt || true
    echo "coverage gate: FAIL (see build/coverage/uncovered.txt)"
    exit 1
fi
echo "coverage gate: pass (every uncovered line is in host/uncovered.txt)"
