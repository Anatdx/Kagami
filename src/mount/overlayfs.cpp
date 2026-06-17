#include "mount/backend.hpp"
#include "mount/mount_fs.hpp"

#include <string>
#include <vector>

// OverlayFS backend placeholder; not yet wired into mount_all_enabled.

namespace kagami::mount::overlay {

bool mount_modules(const std::vector<ModuleEntry>& /*modules*/, const Config& /*config*/) {
    fsutil::mlog("overlayfs backend pending; magic mount is the active backend");
    return false;
}

bool unmount_all(const Config& /*config*/) { return true; }

bool is_active(const Config& /*config*/) { return false; }

} // namespace kagami::mount::overlay
