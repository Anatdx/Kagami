#!/system/bin/sh
#
# Kagami metamodule — boot-completed hook.
#
# KernelSU runs this once the device reaches boot_completed. Reaching here means
# this boot's mounts did not brick the system, so we clear the bootloop-protection
# counter. If a boot instead hangs/loops, this never runs, the counter keeps
# climbing, and mount-all disables mounting after a few unconfirmed boots.

MODDIR="${0%/*}"
"$MODDIR/kagamid" recovery boot-completed >/dev/null 2>&1 || true
exit 0
