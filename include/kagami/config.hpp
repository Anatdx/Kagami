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
    std::vector<std::string> partitions;
    bool debug = false;
    bool verbose = false;
    bool kasumi_enabled = true;
    bool overlayfs_enabled = true;
    bool magic_mount_enabled = true;
    PolicyConfig policy;
};

std::string default_config_json();
bool write_default_config(const std::string& path, std::string& error);
bool parse_config_json(const std::string& json, Config& config, std::string& error);
bool read_config_file(const std::string& path, Config& config, std::string& error);

} // namespace kagami
