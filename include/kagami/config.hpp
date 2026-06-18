#pragma once

#include <string>
#include <cstdint>
#include <vector>

namespace kagami {

struct PolicyConfig {
    std::string owner = "auto";
    bool use_allow_uids = false;
    bool use_deny_uids = false;
    bool include_isolated_uids = false;
    std::vector<std::uint32_t> allow_uids;
    std::vector<std::uint32_t> deny_uids;
};

struct Config {
    std::string module_dir = "/data/adb/modules";
    std::string data_dir = "/data/adb/kagami";
    std::string log_file = "/data/adb/kagami/daemon.log";
    std::string mount_source = "KSU";
    std::string fs_type = "auto";
    // Work tmpfs for magic mount. Module files are copied here (NOT bind-mounted
    // from /data onto system). /dev is a writable tmpfs everywhere; /debug_ramdisk
    // is read-only on some devices. Configurable; avoid /mnt.
    std::string work_dir = "/dev/kagami";
    // OverlayFS mount base. MUST NOT live under /data: overlay lowerdirs backed by
    // /data carry data_file contexts, so vendor/system domains get SELinux-denied
    // (exec denied -> RIL/HALs fail). Use a tmpfs-friendly path like /dev or /mnt.
    // fs_type selects the storage mode ("auto" tries tmpfs then ext4; "tmpfs"/
    // "ext4"/"erofs" force one). For ext4/erofs the backing image persists on /data
    // at overlay_img but is loop-mounted under overlay_dir; overlay_img_size_mb
    // sizes a new ext4 image.
    std::string overlay_dir = "/dev/kagami_overlay";
    std::string overlay_img = "/data/adb/kagami/modules.img";
    int overlay_img_size_mb = 2048;
    // Read-only overlay by default (lowerdirs only), matching meta-overlayfs. A
    // writable upper/work layer is opt-in: it breaks early-boot vendor/system init
    // on some devices (SELinux-denied writes through the overlay -> RIL fails).
    bool overlay_writable = false;
    std::vector<std::string> partitions;
    bool debug = false;
    bool verbose = false;
    bool kasumi_enabled = true;
    bool overlayfs_enabled = true;
    bool magic_mount_enabled = true;
    // Active mount backend: "magic" | "overlay" | "none". Selected by the WebUI;
    // supersedes the overlayfs_enabled/magic_mount_enabled pair for selection.
    std::string mount_backend = "magic";
    PolicyConfig policy;
};

std::string default_config_json();
bool write_default_config(const std::string& path, std::string& error);
bool parse_config_json(const std::string& json, Config& config, std::string& error);
bool read_config_file(const std::string& path, Config& config, std::string& error);

} // namespace kagami
