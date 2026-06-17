#!/system/bin/sh

MODDIR="${0%/*}"
BASE_DIR="/data/adb/kagami"
LOG_FILE="$BASE_DIR/daemon.log"

mkdir -p "$BASE_DIR"
chmod 0755 "$MODDIR/kagamid" 2>/dev/null || true

"$MODDIR/kagamid" daemon start >>"$LOG_FILE" 2>&1 || true

exit 0
