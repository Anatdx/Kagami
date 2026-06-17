#!/system/bin/sh
#
# Kagami metamodule — regular-module install hook.
#
# KernelSU *sources* this during a normal module's installation (after file
# extraction, before completion), with MODPATH/MODULE_ID and the install.sh
# helpers in scope. We use it to normalize the freshly installed module's
# partition layout to the device topology, so metamount.sh can mount it
# consistently — e.g. on system-as-root devices where /vendor is a real
# partition and /system/vendor is a symlink, fold the module's top-level
# <part> content under system/<part> and recreate the symlink.
#
# NOTE: sourced, not executed — do NOT call `exit` here.

KAGAMID="/data/adb/modules/kagami/kagamid"
[ -x "$KAGAMID" ] || KAGAMID="$(command -v kagamid 2>/dev/null)"

if [ -n "$KAGAMID" ] && [ -x "$KAGAMID" ] && [ -n "$MODPATH" ]; then
    ui_print "- Kagami: normalizing module layout" 2>/dev/null || true
    "$KAGAMID" module normalize "$MODPATH" >/dev/null 2>&1 || true
fi
