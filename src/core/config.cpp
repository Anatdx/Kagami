#include "kagami/config.hpp"

#include "core/json_value.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>

namespace kagami {

static bool ensure_parent_dir(const std::string& path, std::string& error) {
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) {
        return true;
    }

    std::string current;
    std::stringstream parts(path.substr(0, slash));
    std::string part;
    while (std::getline(parts, part, '/')) {
        if (part.empty()) {
            current = "/";
            continue;
        }
        if (current.size() > 1) {
            current += "/";
        }
        current += part;
        if (::mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
            error = "mkdir " + current + ": " + std::strerror(errno);
            return false;
        }
    }
    return true;
}

std::string default_config_json() {
    return R"({
  "moduledir": "/data/adb/modules",
  "tempdir": "",
  "mountsource": "KSU",
  "work_dir": "/dev/kagami",
  "overlay_dir": "/dev/kagami_overlay",
  "overlay_img": "/data/adb/kagami/modules.img",
  "overlay_img_size_mb": 2048,
  "overlay_writable": false,
  "logfile": "/data/adb/kagami/daemon.log",
  "debug": false,
  "verbose": false,
  "fs_type": "auto",
  "disable_umount": false,
  "enable_nuke": true,
  "ignore_protocol_mismatch": false,
  "enable_kernel_debug": false,
  "enable_stealth": true,
  "enable_hidexattr": false,
  "kasumi_enabled": true,
  "overlayfs_enabled": true,
  "magic_mount_enabled": true,
  "policy": {
    "owner": "auto",
    "use_allow_uids": false,
    "use_deny_uids": false,
    "include_isolated_uids": false,
    "allow_uids": [],
    "deny_uids": []
  },
  "uname_release": "",
  "uname_version": "",
  "cmdline_value": "",
  "partitions": []
}
)";
}

bool write_default_config(const std::string& path, std::string& error) {
    if (!ensure_parent_dir(path, error)) {
        return false;
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "open " + path + ": " + std::strerror(errno);
        return false;
    }
    out << default_config_json();
    return out.good();
}

static std::vector<std::string> json_string_array_or_empty(const JsonValue* value) {
    std::vector<std::string> out;
    if (!value || !value->is_array()) {
        return out;
    }
    for (const auto& item : value->array_value) {
        if (item.is_string()) {
            out.push_back(item.string_value);
        }
    }
    return out;
}

static std::vector<std::uint32_t> json_u32_array_or_empty(const JsonValue* value) {
    std::vector<std::uint32_t> out;
    if (!value || !value->is_array()) {
        return out;
    }
    for (const auto& item : value->array_value) {
        if (item.is_number() && item.number_value >= 0) {
            out.push_back(static_cast<std::uint32_t>(item.number_value));
        }
    }
    return out;
}

static bool json_bool_or(const JsonValue* root, const char* key, bool fallback) {
    const auto* value = root ? root->find(key) : nullptr;
    return value ? value->bool_or(fallback) : fallback;
}

static std::string json_string_or(const JsonValue* root, const char* key, const std::string& fallback) {
    const auto* value = root ? root->find(key) : nullptr;
    return value ? value->string_or(fallback) : fallback;
}

static int json_int_or(const JsonValue* root, const char* key, int fallback) {
    const auto* value = root ? root->find(key) : nullptr;
    return value ? static_cast<int>(value->u32_or(static_cast<std::uint32_t>(fallback))) : fallback;
}

bool parse_config_json(const std::string& json, Config& config, std::string& error) {
    JsonValue root;
    if (!parse_json(json, root, error)) {
        return false;
    }
    if (!root.is_object()) {
        error = "config root must be a JSON object";
        return false;
    }

    config.module_dir = json_string_or(&root, "moduledir", config.module_dir);
    config.data_dir = json_string_or(&root, "data_dir", config.data_dir);
    config.log_file = json_string_or(&root, "logfile", config.log_file);
    config.mount_source = json_string_or(&root, "mountsource", config.mount_source);
    config.work_dir = json_string_or(&root, "work_dir", config.work_dir);
    config.overlay_dir = json_string_or(&root, "overlay_dir", config.overlay_dir);
    config.overlay_img = json_string_or(&root, "overlay_img", config.overlay_img);
    config.overlay_img_size_mb = json_int_or(&root, "overlay_img_size_mb", config.overlay_img_size_mb);
    config.overlay_writable = json_bool_or(&root, "overlay_writable", config.overlay_writable);
    config.fs_type = json_string_or(&root, "fs_type", config.fs_type);
    config.debug = json_bool_or(&root, "debug", config.debug);
    config.verbose = json_bool_or(&root, "verbose", config.verbose);
    config.kasumi_enabled = json_bool_or(&root, "kasumi_enabled", config.kasumi_enabled);
    config.overlayfs_enabled = json_bool_or(&root, "overlayfs_enabled", config.overlayfs_enabled);
    config.magic_mount_enabled = json_bool_or(&root, "magic_mount_enabled", config.magic_mount_enabled);
    config.partitions = json_string_array_or_empty(root.find("partitions"));

    const JsonValue* policy = root.find("policy");
    if (policy && policy->is_object()) {
        config.policy.owner = json_string_or(policy, "owner", config.policy.owner);
        config.policy.use_allow_uids = json_bool_or(policy, "use_allow_uids", config.policy.use_allow_uids);
        config.policy.use_deny_uids = json_bool_or(policy, "use_deny_uids", config.policy.use_deny_uids);
        config.policy.include_isolated_uids = json_bool_or(policy, "include_isolated_uids", config.policy.include_isolated_uids);
        config.policy.allow_uids = json_u32_array_or_empty(policy->find("allow_uids"));
        config.policy.deny_uids = json_u32_array_or_empty(policy->find("deny_uids"));
    }

    return true;
}

bool read_config_file(const std::string& path, Config& config, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "open " + path + ": " + std::strerror(errno);
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return parse_config_json(buffer.str(), config, error);
}

} // namespace kagami
