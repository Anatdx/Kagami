#!/system/bin/sh
#
# Kagami metamodule — mount hook.
#
# KernelSU runs this at the post-fs-data stage, after every module's
# post-fs-data.sh and before zygote starts, in the init mount namespace. This
# is where a metamodule mounts all enabled modules. All mounts use the "KSU"
# source so KernelSU can track / unload / hide them.

MODDIR="${0%/*}"
BASE_DIR="/data/adb/kagami"
LOG_FILE="$BASE_DIR/daemon.log"

mkdir -p "$BASE_DIR" "$BASE_DIR/run"
chmod 0755 "$MODDIR/kagamid" 2>/dev/null || true

if [ -x "$MODDIR/kagamid" ]; then
    "$MODDIR/kagamid" module mount-all >>"$LOG_FILE" 2>&1
fi

# Best-effort: notify KernelSU that module mounting has completed (matches
# other metamodules; harmless if the command is absent).
if [ -x /data/adb/ksud ]; then
    /data/adb/ksud kernel notify-module-mounted >/dev/null 2>&1 || true
fi

exit 0
