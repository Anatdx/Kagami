#!/system/bin/sh
#
# Kagami metamodule — regular-module uninstall hook.
#
# KernelSU runs this before deleting a regular module's directory, with
# MODULE_ID set. Kagami's install-time normalization (metainstall.sh) only
# writes *inside* the module directory, which KernelSU is about to remove, so
# there is no external state to clean up here. Live unmount of the removed
# module's files happens on the next metamount.sh pass (a full rebuild over the
# remaining enabled modules).
#
# Kept as the declared hook point for future per-module teardown.

exit 0
