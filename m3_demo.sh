#!/usr/bin/env bash
# M3 checkpoint demo: both sensors -> reader processes -> hub (Unix socket IPC) -> oc_store.
# Run from the OpenChair repo root after `make`.
#   ./m3_demo.sh            LIVE sensors on /dev/i2c-1 (the real demo)
#   ./m3_demo.sh sim        SIMULATED readings (rehearsal without hardware)
# Env: CAPTURE_MS (default 30000), CAPTURE_DIR (default ~/openchair-capture, on the SD card)
set -u
MODE=${1:-live}
B=./build/plain/openchair
CAPTURE_MS=${CAPTURE_MS:-30000}
CAP=${CAPTURE_DIR:-$HOME/openchair-capture}
LIFE=$((CAPTURE_MS + 8000))
SESSION=$(od -An -N8 -tx8 /dev/urandom | tr -d ' \n')
RUN=$(mktemp -d); chmod 700 "$RUN"
STAMP=$(date +%F-%H%M%S)
LOG=m3-$MODE-$STAMP
mkdir -p "$CAP" "$LOG"

[ -x "$B" ] || { echo "build first: make" >&2; exit 1; }
I2C=()
[ "$MODE" = live ] && I2C=(--i2c-dev /dev/i2c-1)

echo "mode=$MODE session=$SESSION capture_dir=$CAP logs=$LOG/"
"$B" --role hub --input "$MODE" --unsupervised-test --runtime-dir "$RUN" --session-id "$SESSION" \
     --light-instance 1 --distance-instance 1 \
     --capture-dir "$CAP" --capture-ms "$CAPTURE_MS" --lifetime-ms "$LIFE" \
     > "$LOG/hub.out" 2> "$LOG/hub.err" &
HUB=$!
sleep 0.5
"$B" --role light --input "$MODE" "${I2C[@]}" --runtime-dir "$RUN" --session-id "$SESSION" \
     --instance 1 --lifetime-ms "$((LIFE - 2000))" > "$LOG/light.out" 2> "$LOG/light.err" &
LIGHT=$!
"$B" --role distance --input "$MODE" "${I2C[@]}" --runtime-dir "$RUN" --session-id "$SESSION" \
     --instance 1 --lifetime-ms "$((LIFE - 2000))" > "$LOG/distance.out" 2> "$LOG/distance.err" &
DIST=$!
echo "pids: hub=$HUB light=$LIGHT distance=$DIST  (kill -9 one of them to show fault handling)"

wait "$HUB"; HUB_RC=$?
wait "$LIGHT" "$DIST" 2>/dev/null
rm -rf "$RUN"

echo "--- hub exit=$HUB_RC"
grep -E '^(READY|STORE_RECOVER|CAPRESULT|CAPSRC|CAPRB)' "$LOG/hub.out" | cut -c1-220
grep -h '^SUMMARY' "$LOG/light.out" "$LOG/distance.out" | cut -c1-160
