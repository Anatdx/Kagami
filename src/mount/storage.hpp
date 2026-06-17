#pragma once

#include <string>

#include "kagami/config.hpp"

namespace kagami::mount::storage {

// OverlayFS needs a clean base for its layers: overlay refuses /data-backed
// layers (wrong SELinux contexts, f2fs feature clashes). We provide one as a
// tmpfs (when it preserves overlay xattrs), an ext4 loop image, or a read-only
// erofs image paired with a tmpfs for the writable layer.
enum class Mode { Tmpfs, Ext4, Erofs };

const char* mode_name(Mode mode);

struct Handle {
    bool ok = false;
    Mode mode = Mode::Ext4;
    std::string content_dir; // per-module lower trees live at content_dir/<id>/<part>
    std::string rw_dir;      // writable fs for per-partition upperdir/workdir
};

// Mount the overlay base per config.fs_type ("auto" => tmpfs if overlay xattrs
// work, else ext4). Must run inside the init mount namespace; marks the mounts
// private and registers them with KernelSU. Returns Handle{ok=false} on failure.
Handle setup(const Config& config);

// Unmount the base (content + writable). The ext4/erofs image is left on disk.
void teardown(const Handle& handle);

} // namespace kagami::mount::storage
