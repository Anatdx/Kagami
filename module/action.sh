#!/system/bin/sh

MODDIR="${0%/*}"
"$MODDIR/kagamid" daemon status 2>/dev/null || true
"$MODDIR/kagamid" api system 2>/dev/null || true
