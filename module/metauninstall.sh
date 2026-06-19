#!/system/bin/sh
# Kagami metamodule — regular-module uninstall hook.
# Purge the removed module's synced content from the overlay base.

[ -n "$MODULE_ID" ] || exit 0

BASE="/dev/kagami_overlay"
CONFIG="/data/adb/kagami/config.json"
if [ -f "$CONFIG" ]; then
    dir=$(grep -oE '"overlay_dir"[^,}]*' "$CONFIG" | cut -d'"' -f4)
    [ -n "$dir" ] && BASE="$dir"
fi

CONTENT="$BASE/mnt/$MODULE_ID"
if mountpoint -q "$BASE/mnt" 2>/dev/null && [ -d "$CONTENT" ]; then
    rm -rf "$CONTENT"
fi
exit 0
