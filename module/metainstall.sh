#!/system/bin/sh
# Kagami metamodule — regular-module install hook (sourced by KSU; no `exit`).

export KSU_HAS_METAMODULE="true"
export KSU_METAMODULE="kagami"

# Kagami (kagamid @ post-fs-data) is the only mount backend.
handle_partition() {
    echo 0 >/dev/null
    true
}

mark_replace() {
    replace_target="$1"
    mkdir -p "$replace_target"
    setfattr -n trusted.overlay.opaque -v y "$replace_target"
}

ui_print "- Using Kagami metainstall"
install_module
ui_print "- Installation complete"
