#include "core/command.hpp"

#include "core/daemon.hpp"
#include "core/json.hpp"
#include "core/runtime.hpp"
#include "kagami/config.hpp"
#include "kagami/kasumi_client.hpp"
#include "kagami/kasumi_uapi_compat.hpp"
#include "mount/backend.hpp"
#include "mount/magic_mount.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <sys/statvfs.h>
#include <vector>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace kagami {

namespace fs = std::filesystem;

static const std::vector<std::string> kBuiltinPartitions = {
    "system", "vendor", "product", "system_ext", "odm", "oem",
};

static fs::path data_dir() {
    return runtime_data_dir();
}

static fs::path modules_dir() {
    return runtime_modules_dir();
}

static fs::path config_file() {
    return runtime_config_file();
}

static fs::path user_hide_rules_file() {
    return data_dir() / "user_hide_rules.json";
}

static fs::path lkm_autoload_file() {
    return data_dir() / "lkm_autoload";
}

static fs::path lkm_kmi_override_file() {
    return data_dir() / "lkm_kmi_override";
}

static void print_usage() {
    std::cout
        << "Kagami " << KAGAMI_VERSION << "\n"
        << "usage:\n"
        << "  kagamid version\n"
        << "  kagamid config show\n"
        << "  kagamid config gen [-o PATH]\n"
        << "  kagamid config apply [PATH]\n"
        << "  kagamid daemon status|serve|call|ping|stop\n"
        << "  kagamid api system|storage|lkm|features|hooks|backends|meta\n"
        << "  kagamid module list|check-conflicts|mount-all|unmount|normalize\n"
        << "  kagamid recovery status|boot-completed|reset\n"
        << "  kagamid kasumi version|list|enable|disable|policy\n";
}

static std::string arg_or_default(const std::vector<std::string>& args, std::size_t index, const std::string& fallback) {
    return index < args.size() ? args[index] : fallback;
}

// True once the device has finished booting. Magic mount must only run during
// the post-fs-data boot stage (metamount.sh); mounting over the live system
// afterwards breaks mount namespaces, so the CLI refuses mount-all post-boot.
static bool system_boot_completed() {
#if defined(__ANDROID__)
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get("sys.boot_completed", value) > 0) {
        return std::string(value) == "1";
    }
#endif
    return false;
}

static void print_string_array(const std::vector<std::string>& values) {
    std::cout << "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            std::cout << ",";
        }
        std::cout << json_quote(values[i]);
    }
    std::cout << "]";
}

static void print_u32_array(const std::vector<std::uint32_t>& values) {
    std::cout << "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            std::cout << ",";
        }
        std::cout << values[i];
    }
    std::cout << "]";
}

static std::string read_first_line(const std::string& path) {
    std::ifstream in(path);
    std::string line;
    if (std::getline(in, line)) {
        return line;
    }
    return "";
}

static std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return "";
    }
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

static bool write_file(const fs::path& path, const std::string& content) {
    try {
        fs::create_directories(path.parent_path());
    } catch (...) {
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << content;
    return out.good();
}

static bool is_builtin_partition(const std::string& name) {
    return std::find(kBuiltinPartitions.begin(), kBuiltinPartitions.end(), name) != kBuiltinPartitions.end();
}

static bool module_has_partition_content(const fs::path& module_path, const std::string& partition) {
    const fs::path root = module_path / partition;
    if (!fs::is_directory(root)) {
        return false;
    }
    try {
        return fs::recursive_directory_iterator(root) != fs::recursive_directory_iterator();
    } catch (...) {
        return false;
    }
}

static std::vector<std::string> parse_json_string_array(const std::string& json) {
    std::vector<std::string> values;
    bool in_string = false;
    bool escape = false;
    std::string value;

    for (char c : json) {
        if (!in_string) {
            if (c == '"') {
                in_string = true;
                value.clear();
            }
            continue;
        }
        if (escape) {
            value.push_back(c);
            escape = false;
            continue;
        }
        if (c == '\\') {
            escape = true;
            continue;
        }
        if (c == '"') {
            values.push_back(value);
            in_string = false;
            continue;
        }
        value.push_back(c);
    }
    return values;
}

static std::map<std::string, std::string> parse_json_string_map(const std::string& json) {
    const auto tokens = parse_json_string_array(json);
    std::map<std::string, std::string> map;
    for (std::size_t i = 0; i + 1 < tokens.size(); i += 2) {
        map[tokens[i]] = tokens[i + 1];
    }
    return map;
}

static std::vector<std::string> load_user_hide_rules() {
    return parse_json_string_array(read_file(user_hide_rules_file()));
}

static bool save_user_hide_rules(const std::vector<std::string>& rules) {
    std::ostringstream out;
    out << "[\n";
    for (std::size_t i = 0; i < rules.size(); ++i) {
        out << "  " << json_quote(rules[i]);
        if (i + 1 < rules.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "]\n";
    return write_file(user_hide_rules_file(), out.str());
}

static std::map<std::string, std::string> read_prop_file(const fs::path& path) {
    std::map<std::string, std::string> props;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos || eq == 0) {
            continue;
        }
        props[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return props;
}

static std::string kernel_release() {
    const std::string version = read_first_line("/proc/version");
    const std::string marker = "Linux version ";
    const auto start = version.find(marker);
    if (start == std::string::npos) {
        return version.empty() ? "Unknown" : version;
    }
    const auto value_start = start + marker.size();
    const auto value_end = version.find(' ', value_start);
    return version.substr(value_start, value_end - value_start);
}

static std::string selinux_status() {
    const std::string enforce = read_first_line("/sys/fs/selinux/enforce");
    if (enforce == "0") {
        return "Permissive";
    }
    if (enforce == "1") {
        return "Enforcing";
    }
    return "Unknown";
}

static std::string format_bytes(unsigned long long bytes) {
    const char* units[] = {"B", "K", "M", "G", "T"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    std::ostringstream out;
    if (unit == 0) {
        out << static_cast<unsigned long long>(value) << units[unit];
    } else {
        out.setf(std::ios::fixed);
        out.precision(value < 10.0 ? 1 : 0);
        out << value << units[unit];
    }
    return out.str();
}

static std::string partition_mount_point(const std::string& partition) {
    if (partition == "system") {
        return "/system";
    }
    return "/" + partition;
}

static bool path_is_read_only_mount(const std::string& mount_point) {
    std::ifstream mounts("/proc/mounts");
    std::string line;
    while (std::getline(mounts, line)) {
        std::istringstream parts(line);
        std::string dev;
        std::string mp;
        std::string fs_type;
        std::string options;
        parts >> dev >> mp >> fs_type >> options;
        if (mp == mount_point) {
            return options.find("ro") != std::string::npos;
        }
    }
    return false;
}

static int print_storage_json() {
    const fs::path base = data_dir();

    struct statvfs st = {};
    if (statvfs(base.c_str(), &st) != 0) {
        std::cout << "{\"error\":\"not mounted\"}\n";
        return 0;
    }

    const auto total = static_cast<unsigned long long>(st.f_blocks) * st.f_frsize;
    const auto avail = static_cast<unsigned long long>(st.f_bavail) * st.f_frsize;
    const auto used = total > avail ? total - avail : 0;
    const int percent = total > 0 ? static_cast<int>((used * 100ULL) / total) : 0;

    std::cout
        << "{"
        << "\"size\":" << json_quote(format_bytes(total)) << ","
        << "\"used\":" << json_quote(format_bytes(used)) << ","
        << "\"avail\":" << json_quote(format_bytes(avail)) << ","
        << "\"percent\":" << percent << ","
        << "\"mode\":\"host\""
        << "}\n";
    return 0;
}

static int print_partitions_json() {
    std::cout << "[";
    for (std::size_t i = 0; i < kBuiltinPartitions.size(); ++i) {
        const auto& name = kBuiltinPartitions[i];
        const auto mount_point = partition_mount_point(name);
        if (i > 0) {
            std::cout << ",";
        }
        std::cout
            << "{"
            << "\"name\":" << json_quote(name) << ","
            << "\"mount_point\":" << json_quote(mount_point) << ","
            << "\"fs_type\":\"\","
            << "\"is_read_only\":" << (path_is_read_only_mount(mount_point) ? "true" : "false") << ","
            << "\"exists_as_symlink\":" << (fs::is_symlink(mount_point) ? "true" : "false")
            << "}";
    }
    std::cout << "]";
    return 0;
}

static int print_features_json(int bitmask) {
    const auto names = kasumi::feature_names(bitmask);
    std::cout << "{\"bitmask\":" << bitmask << ",\"names\":";
    print_string_array(names);
    std::cout << "}";
    return 0;
}

static std::string policy_owner_name(kasumi::PolicyOwner owner) {
    switch (owner) {
    case kasumi::PolicyOwner::Auto:
        return "auto";
    case kasumi::PolicyOwner::KernelSU:
        return "kernelsu";
    case kasumi::PolicyOwner::APatch:
        return "apatch";
    case kasumi::PolicyOwner::Magisk:
        return "magisk";
    case kasumi::PolicyOwner::Manual:
        return "manual";
    case kasumi::PolicyOwner::Disabled:
        return "disabled";
    }
    return "unknown";
}

static bool parse_policy_owner(const std::string& value, kasumi::PolicyOwner& owner) {
    if (value == "auto") {
        owner = kasumi::PolicyOwner::Auto;
    } else if (value == "kernelsu" || value == "ksu") {
        owner = kasumi::PolicyOwner::KernelSU;
    } else if (value == "apatch") {
        owner = kasumi::PolicyOwner::APatch;
    } else if (value == "magisk") {
        owner = kasumi::PolicyOwner::Magisk;
    } else if (value == "manual") {
        owner = kasumi::PolicyOwner::Manual;
    } else if (value == "disabled" || value == "off") {
        owner = kasumi::PolicyOwner::Disabled;
    } else {
        return false;
    }
    return true;
}

static std::string policy_uid_list_name(kasumi::PolicyUidList list) {
    switch (list) {
    case kasumi::PolicyUidList::Allow:
        return "allow";
    case kasumi::PolicyUidList::Deny:
        return "deny";
    case kasumi::PolicyUidList::All:
        return "all";
    }
    return "unknown";
}

static bool parse_policy_uid_list(const std::string& value, kasumi::PolicyUidList& list) {
    if (value == "allow" || value == "allowlist") {
        list = kasumi::PolicyUidList::Allow;
    } else if (value == "deny" || value == "denylist") {
        list = kasumi::PolicyUidList::Deny;
    } else if (value == "all") {
        list = kasumi::PolicyUidList::All;
    } else {
        return false;
    }
    return true;
}

static std::uint32_t parse_policy_flags(const std::vector<std::string>& args, std::size_t start, std::uint32_t fallback) {
    if (start >= args.size()) {
        return fallback;
    }
    std::uint32_t flags = 0;
    for (std::size_t i = start; i < args.size(); ++i) {
        const auto& value = args[i];
        if (value == "allow" || value == "allow-uids") {
            flags |= KSM_POLICY_FLAG_USE_ALLOW_UIDS;
        } else if (value == "deny" || value == "deny-uids") {
            flags |= KSM_POLICY_FLAG_USE_DENY_UIDS;
        } else if (value == "isolated" || value == "include-isolated") {
            flags |= KSM_POLICY_FLAG_INCLUDE_ISOLATED_UIDS;
        } else if (value.rfind("0x", 0) == 0) {
            flags |= static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 0));
        }
    }
    return flags;
}

static bool parse_uid_values(const std::vector<std::string>& args, std::size_t start, std::vector<std::uint32_t>& uids) {
    uids.clear();
    for (std::size_t i = start; i < args.size(); ++i) {
        char* end = nullptr;
        errno = 0;
        const unsigned long value = std::strtoul(args[i].c_str(), &end, 0);
        if (errno != 0 || end == args[i].c_str() || *end != '\0' || value > UINT32_MAX) {
            return false;
        }
        uids.push_back(static_cast<std::uint32_t>(value));
    }
    return true;
}

static void print_roots_json(std::uint32_t roots) {
    std::vector<std::string> names;
    if (roots & (1U << 0)) {
        names.emplace_back("kernelsu");
    }
    if (roots & (1U << 1)) {
        names.emplace_back("kernelsu_redirect");
    }
    if (roots & (1U << 2)) {
        names.emplace_back("apatch");
    }
    if (roots & (1U << 3)) {
        names.emplace_back("magisk");
    }
    if (roots & (1U << 4)) {
        names.emplace_back("multi");
    }
    if (roots & (1U << 5)) {
        names.emplace_back("non_root");
    }
    std::cout << "{\"bitmask\":" << roots << ",\"names\":";
    print_string_array(names);
    std::cout << "}";
}

static void print_policy_flags_json(std::uint32_t flags) {
    std::cout << "{"
              << "\"bitmask\":" << flags << ","
              << "\"use_allow_uids\":" << ((flags & KSM_POLICY_FLAG_USE_ALLOW_UIDS) ? "true" : "false") << ","
              << "\"use_deny_uids\":" << ((flags & KSM_POLICY_FLAG_USE_DENY_UIDS) ? "true" : "false") << ","
              << "\"include_isolated_uids\":" << ((flags & KSM_POLICY_FLAG_INCLUDE_ISOLATED_UIDS) ? "true" : "false")
              << "}";
}

static int print_policy_json() {
    const auto state = kasumi::policy_state();
    const auto allow_uids = state.ok ? kasumi::policy_uids(kasumi::PolicyUidList::Allow) : std::vector<std::uint32_t>{};
    const auto deny_uids = state.ok ? kasumi::policy_uids(kasumi::PolicyUidList::Deny) : std::vector<std::uint32_t>{};

    std::cout << "{"
              << "\"ok\":" << (state.ok ? "true" : "false") << ","
              << "\"errno\":" << state.last_errno << ","
              << "\"err\":" << state.err << ","
              << "\"api_version\":" << state.version << ","
              << "\"owner\":" << json_quote(policy_owner_name(state.owner)) << ","
              << "\"effective_owner\":" << json_quote(policy_owner_name(state.effective_owner)) << ","
              << "\"flags\":";
    print_policy_flags_json(state.flags);
    std::cout << ",\"detected_roots\":";
    print_roots_json(state.detected_roots);
    std::cout << ",\"allow_count\":" << state.allow_count
              << ",\"deny_count\":" << state.deny_count
              << ",\"max_uid_count\":" << state.max_uid_count
              << ",\"allow_uids\":";
    print_u32_array(allow_uids);
    std::cout << ",\"deny_uids\":";
    print_u32_array(deny_uids);
    std::cout << "}\n";
    return state.ok ? 0 : 1;
}

static bool apply_policy_config(const PolicyConfig& policy, std::string& error) {
    kasumi::PolicyOwner owner = kasumi::PolicyOwner::Auto;
    if (!parse_policy_owner(policy.owner, owner)) {
        error = "invalid policy owner: " + policy.owner;
        return false;
    }

    std::uint32_t flags = 0;
    if (policy.use_allow_uids || !policy.allow_uids.empty()) {
        flags |= KSM_POLICY_FLAG_USE_ALLOW_UIDS;
    }
    if (policy.use_deny_uids || !policy.deny_uids.empty()) {
        flags |= KSM_POLICY_FLAG_USE_DENY_UIDS;
    }
    if (policy.include_isolated_uids) {
        flags |= KSM_POLICY_FLAG_INCLUDE_ISOLATED_UIDS;
    }

    if (!kasumi::set_policy(owner, flags)) {
        error = "failed to set Kasumi policy owner";
        return false;
    }
    if ((flags & KSM_POLICY_FLAG_USE_ALLOW_UIDS) && !kasumi::set_policy_uids(kasumi::PolicyUidList::Allow, policy.allow_uids)) {
        error = "failed to set Kasumi policy allow uid list";
        return false;
    }
    if (!(flags & KSM_POLICY_FLAG_USE_ALLOW_UIDS) && !kasumi::clear_policy_uids(kasumi::PolicyUidList::Allow)) {
        error = "failed to clear Kasumi policy allow uid list";
        return false;
    }
    if ((flags & KSM_POLICY_FLAG_USE_DENY_UIDS) && !kasumi::set_policy_uids(kasumi::PolicyUidList::Deny, policy.deny_uids)) {
        error = "failed to set Kasumi policy deny uid list";
        return false;
    }
    if (!(flags & KSM_POLICY_FLAG_USE_DENY_UIDS) && !kasumi::clear_policy_uids(kasumi::PolicyUidList::Deny)) {
        error = "failed to clear Kasumi policy deny uid list";
        return false;
    }
    return true;
}

static int apply_config_file(const fs::path& path) {
    Config config;
    std::string error;
    if (!read_config_file(path.string(), config, error)) {
        std::cerr << error << "\n";
        return 1;
    }
    if (!apply_policy_config(config.policy, error)) {
        std::cerr << error << "\n";
        return 1;
    }
    return print_policy_json();
}

static void print_backend_statuses_json() {
    const auto statuses = mount::backend_statuses();
    std::cout << "[";
    for (std::size_t i = 0; i < statuses.size(); ++i) {
        const auto& status = statuses[i];
        if (i > 0) {
            std::cout << ",";
        }
        std::cout << "{"
                  << "\"kind\":" << json_quote(mount::backend_kind_name(status.kind)) << ","
                  << "\"name\":" << json_quote(status.name) << ","
                  << "\"available\":" << (status.available ? "true" : "false") << ","
                  << "\"preferred\":" << (status.preferred ? "true" : "false") << ","
                  << "\"detail\":" << json_quote(status.detail)
                  << "}";
    }
    std::cout << "]";
}

static int print_system_json() {
    const auto version = kasumi::version_info();
    const int bitmask = version.status == kasumi::Status::Available ? kasumi::features() : 0;
    const std::string hook_text = version.status == kasumi::Status::Available ? kasumi::hooks() : "";

    std::cout
        << "{"
        << "\"kernel\":" << json_quote(kernel_release()) << ","
        << "\"selinux\":" << json_quote(selinux_status()) << ","
        << "\"mount_base\":" << json_quote(kasumi::default_mirror_path()) << ","
        << "\"kasumi_available\":" << (version.status == kasumi::Status::Available ? "true" : "false") << ","
        << "\"kasumi_status\":" << static_cast<int>(version.status) << ","
        << "\"hooks\":" << json_quote(hook_text) << ","
        << "\"features\":";
    print_features_json(bitmask);
    std::cout
        << ",\"mountStats\":{\"total_mounts\":0,\"successful_mounts\":0,\"failed_mounts\":0,"
        << "\"tmpfs_created\":0,\"files_mounted\":0,\"dirs_mounted\":0,\"symlinks_created\":0,"
        << "\"overlayfs_mounts\":0,\"success_rate\":0},"
        << "\"detectedPartitions\":";
    print_partitions_json();
    std::cout << ",\"backends\":";
    print_backend_statuses_json();
    std::cout << "}\n";
    return 0;
}

static int print_meta_json() {
    std::cout << "{"
              << "\"module_id\":\"kagami\","
              << "\"metamodule\":true,"
              << "\"version\":" << json_quote(KAGAMI_VERSION) << ","
              << "\"data_dir\":" << json_quote(runtime_data_dir().string()) << ","
              << "\"modules_dir\":" << json_quote(runtime_modules_dir().string()) << ","
              << "\"config_file\":" << json_quote(runtime_config_file().string()) << ","
              << "\"socket\":" << json_quote(runtime_socket_file().string()) << ","
              << "\"pid_file\":" << json_quote(runtime_pid_file().string()) << ","
              << "\"log_file\":" << json_quote(runtime_log_file().string()) << ","
              << "\"backends\":";
    print_backend_statuses_json();
    std::cout << "}\n";
    return 0;
}

static int print_kasumi_version_json() {
    const auto version = kasumi::version_info();
    const std::string rules = version.status == kasumi::Status::Available ? kasumi::active_rules() : "";
    const auto modules = kasumi::active_modules_from_rules(rules);
    const bool mismatch = version.status == kasumi::Status::KernelTooOld ||
                          version.status == kasumi::Status::ClientTooOld;

    std::cout
        << "{"
        << "\"backend\":\"kasumi\","
        << "\"protocol_version\":" << version.expected_protocol << ","
        << "\"kernel_version\":" << version.kernel_protocol << ","
        << "\"kasumi_available\":" << (version.status == kasumi::Status::Available ? "true" : "false") << ","
        << "\"protocol_mismatch\":" << (mismatch ? "true" : "false") << ","
        << "\"mismatch_message\":"
        << json_quote(mismatch ? "Kasumi protocol mismatch" : "") << ","
        << "\"active_modules\":";
    print_string_array(modules);
    std::cout << ",\"mount_base\":" << json_quote(kasumi::default_mirror_path()) << "}\n";
    return 0;
}

static int print_kasumi_rules_json() {
    const std::string rules = kasumi::active_rules();
    std::istringstream lines(rules);
    std::string line;
    bool first = true;

    std::cout << "[";
    while (std::getline(lines, line)) {
        if (line.empty()) {
            continue;
        }

        std::istringstream parts(line);
        std::string type;
        parts >> type;
        std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });

        if (!first) {
            std::cout << ",";
        }
        first = false;

        std::cout << "{\"type\":" << json_quote(type);
        if (type == "ADD" || type == "MERGE") {
            std::string target;
            std::string source;
            parts >> target >> source;
            std::cout << ",\"target\":" << json_quote(target)
                      << ",\"source\":" << json_quote(source);
        } else if (type == "HIDE") {
            std::string path;
            parts >> path;
            std::cout << ",\"path\":" << json_quote(path);
        } else {
            std::string rest;
            std::getline(parts, rest);
            if (!rest.empty() && rest[0] == ' ') {
                rest.erase(0, 1);
            }
            std::cout << ",\"args\":" << json_quote(rest);
        }
        std::cout << "}";
    }
    std::cout << "]\n";
    return 0;
}

static int handle_config(const std::vector<std::string>& args) {
    const auto sub = arg_or_default(args, 1, "");
    if (sub == "show") {
        const auto config = read_file(config_file());
        std::cout << (config.empty() ? default_config_json() : config);
        return 0;
    }
    if (sub == "gen") {
        std::string output = config_file().string();
        for (std::size_t i = 2; i + 1 < args.size(); ++i) {
            if (args[i] == "-o" || args[i] == "--output") {
                output = args[i + 1];
            }
        }
        std::string error;
        if (!write_default_config(output, error)) {
            std::cerr << error << "\n";
            return 1;
        }
        return 0;
    }
    if (sub == "apply") {
        return apply_config_file(arg_or_default(args, 2, config_file().string()));
    }
    if (sub == "sync-partitions") {
        std::set<std::string> partitions;
        const fs::path module_root = modules_dir();
        if (fs::is_directory(module_root)) {
            for (const auto& module : fs::directory_iterator(module_root)) {
                if (!module.is_directory()) {
                    continue;
                }
                for (const auto& child : fs::directory_iterator(module.path())) {
                    if (child.is_directory()) {
                        const std::string name = child.path().filename().string();
                        if (!is_builtin_partition(name) && module_has_partition_content(module.path(), name)) {
                            partitions.insert(name);
                        }
                    }
                }
            }
        }
        if (partitions.empty()) {
            std::cout << "No new partitions\n";
        } else {
            for (const auto& partition : partitions) {
                std::cout << "Added partition: " << partition << "\n";
            }
        }
        return 0;
    }
    print_usage();
    return 1;
}

static int handle_api(const std::vector<std::string>& args) {
    const auto sub = arg_or_default(args, 1, "");
    if (sub == "system") {
        return print_system_json();
    }
    if (sub == "storage") {
        return print_storage_json();
    }
    if (sub == "lkm") {
        const auto version = kasumi::version_info();
        const std::string autoload = read_first_line(lkm_autoload_file().string());
        const std::string kmi = read_first_line(lkm_kmi_override_file().string());
        std::cout << "{\"loaded\":" << (version.status != kasumi::Status::NotPresent ? "true" : "false")
                  << ",\"autoload\":" << (autoload == "0" ? "false" : "true")
                  << ",\"kmi_override\":" << json_quote(kmi) << "}\n";
        return 0;
    }
    if (sub == "features") {
        const auto version = kasumi::version_info();
        return print_features_json(version.status == kasumi::Status::Available ? kasumi::features() : 0);
    }
    if (sub == "hooks") {
        if (!kasumi::is_available()) {
            std::cerr << "Kasumi not available.\n";
            return 1;
        }
        std::cout << kasumi::hooks() << "\n";
        return 0;
    }
    if (sub == "policy") {
        return print_policy_json();
    }
    if (sub == "backends") {
        print_backend_statuses_json();
        std::cout << "\n";
        return 0;
    }
    if (sub == "meta") {
        return print_meta_json();
    }
    print_usage();
    return 1;
}

static int handle_module(const std::vector<std::string>& args) {
    const auto sub = arg_or_default(args, 1, "");
    if (sub == "list") {
        const auto active_modules = kasumi::active_modules_from_rules(kasumi::active_rules());
        const auto modes = parse_json_string_map(read_file(data_dir() / "module_mode.json"));
        std::cout << "{\"modules\":[";
        bool first = true;
        const fs::path module_root = modules_dir();
        if (fs::is_directory(module_root)) {
            for (const auto& entry : fs::directory_iterator(module_root)) {
                if (!entry.is_directory()) {
                    continue;
                }
                const std::string id = entry.path().filename().string();
                const auto props = read_prop_file(entry.path() / "module.prop");
                if (!first) {
                    std::cout << ",";
                }
                first = false;
                const bool active = std::find(active_modules.begin(), active_modules.end(), id) != active_modules.end();
                const auto mode_it = modes.find(id);
                const std::string mode = mode_it == modes.end() ? "auto" : mode_it->second;
                std::string strategy = mode;
                if (strategy == "auto") {
                    strategy = active ? "kasumi" : "auto";
                }
                std::cout << "{"
                          << "\"id\":" << json_quote(id) << ","
                          << "\"name\":" << json_quote(props.count("name") ? props.at("name") : id) << ","
                          << "\"version\":" << json_quote(props.count("version") ? props.at("version") : "") << ","
                          << "\"author\":" << json_quote(props.count("author") ? props.at("author") : "") << ","
                          << "\"description\":" << json_quote(props.count("description") ? props.at("description") : "") << ","
                          << "\"mode\":" << json_quote(mode) << ","
                          << "\"strategy\":" << json_quote(strategy) << ","
                          << "\"path\":" << json_quote(entry.path().string()) << ","
                          << "\"rules\":[]"
                          << "}";
            }
        }
        std::cout << "]}\n";
        return 0;
    }
    if (sub == "check-conflicts") {
        std::cout << "[]\n";
        return 0;
    }
    if (sub == "mount-all") {
        // Boot-only entry (the metamodule metamount.sh hook). Refuse once the
        // device has booted: magic mount over the live system post-boot breaks
        // mount namespaces (it propagates into adbd / service namespaces).
        if (system_boot_completed() && std::getenv("KAGAMI_MOUNT_HERE") == nullptr) {
            std::cerr << "refusing module mount-all: magic mount only runs at boot via "
                         "metamount.sh; post-boot mounting breaks namespaces\n";
            return 1;
        }
        Config config;
        std::string cfg_err;
        read_config_file(config_file().string(), config, cfg_err); // defaults on error
        const auto report = mount::mount_all_enabled(config);
        std::cout << "{"
                  << "\"ok\":" << (report.ok ? "true" : "false") << ","
                  << "\"backend\":" << json_quote(report.backend) << ","
                  << "\"modules\":" << report.modules << ","
                  << "\"mounts\":" << report.mounts << ","
                  << "\"detail\":" << json_quote(report.detail)
                  << "}\n";
        return report.ok ? 0 : 1;
    }
    if (sub == "normalize") {
        const std::string path = arg_or_default(args, 2, "");
        if (path.empty()) {
            std::cerr << "usage: kagamid module normalize <module_path>\n";
            return 1;
        }
        const bool ok = mount::magic::normalize_module(path);
        std::cout << "{\"ok\":" << (ok ? "true" : "false") << ",\"module\":" << json_quote(path) << "}\n";
        return ok ? 0 : 1;
    }
    if (sub == "unmount") {
        // Tear down Kagami's own mounts (source-gated; never touches real partitions).
        Config config;
        std::string cfg_err;
        read_config_file(config_file().string(), config, cfg_err); // defaults on error
        const bool ok = mount::unmount_all(config);
        std::cout << "{\"ok\":" << (ok ? "true" : "false") << "}\n";
        return ok ? 0 : 1;
    }
    print_usage();
    return 1;
}

static int handle_kasumi(const std::vector<std::string>& args) {
    const auto sub = arg_or_default(args, 1, "");
    if (sub == "version") {
        return print_kasumi_version_json();
    }
    if (sub == "list") {
        return print_kasumi_rules_json();
    }
    if (sub == "features") {
        const auto version = kasumi::version_info();
        return print_features_json(version.status == kasumi::Status::Available ? kasumi::features() : 0);
    }
    if (sub == "policy") {
        const auto op = arg_or_default(args, 2, "show");
        if (op == "show" || op == "state") {
            return print_policy_json();
        }
        if (op == "owner" || op == "set") {
            kasumi::PolicyOwner owner = kasumi::PolicyOwner::Auto;
            if (!parse_policy_owner(arg_or_default(args, 3, ""), owner)) {
                std::cerr << "policy owner must be auto|kernelsu|apatch|magisk|manual|disabled\n";
                return 1;
            }
            const auto current = kasumi::policy_state();
            const std::uint32_t flags = parse_policy_flags(args, 4, current.ok ? current.flags : 0);
            if (!kasumi::set_policy(owner, flags)) {
                std::cerr << "failed to set Kasumi policy owner\n";
                return 1;
            }
            return 0;
        }
        if (op == "allow" || op == "deny") {
            std::vector<std::uint32_t> uids;
            if (!parse_uid_values(args, 3, uids)) {
                std::cerr << "policy " << op << " requires numeric uid values\n";
                return 1;
            }
            const auto list = op == "allow" ? kasumi::PolicyUidList::Allow : kasumi::PolicyUidList::Deny;
            if (!kasumi::set_policy_uids(list, uids)) {
                std::cerr << "failed to set Kasumi policy " << op << " uid list\n";
                return 1;
            }
            return 0;
        }
        if (op == "clear") {
            kasumi::PolicyUidList list = kasumi::PolicyUidList::All;
            if (!parse_policy_uid_list(arg_or_default(args, 3, "all"), list)) {
                std::cerr << "policy clear target must be allow|deny|all\n";
                return 1;
            }
            if (!kasumi::clear_policy_uids(list)) {
                std::cerr << "failed to clear Kasumi policy " << policy_uid_list_name(list) << " uid list\n";
                return 1;
            }
            return 0;
        }
        if (op == "apply") {
            return apply_config_file(arg_or_default(args, 3, config_file().string()));
        }
        std::cerr << "usage: kagamid kasumi policy [show|owner OWNER [allow] [deny] [isolated]|allow UID...|deny UID...|clear allow|deny|all]\n";
        return 1;
    }
    if (sub == "enable" || sub == "disable") {
        if (!kasumi::set_enabled(sub == "enable")) {
            std::cerr << "failed to set Kasumi enabled state\n";
            return 1;
        }
        return 0;
    }
    if (sub == "mount-hide" || sub == "maps-spoof" || sub == "statfs-spoof") {
        const bool on = arg_or_default(args, 2, "off") == "on";
        bool ok = false;
        if (sub == "mount-hide") {
            ok = kasumi::set_mount_hide(on);
        } else if (sub == "maps-spoof") {
            ok = kasumi::set_maps_spoof(on);
        } else {
            ok = kasumi::set_statfs_spoof(on);
        }
        if (!ok) {
            std::cerr << "failed to set Kasumi " << sub << "\n";
            return 1;
        }
        return 0;
    }
    print_usage();
    return 1;
}

static int handle_lkm(const std::vector<std::string>& args) {
    const auto sub = arg_or_default(args, 1, "");
    if (sub == "set-autoload") {
        const bool on = arg_or_default(args, 2, "on") != "off";
        return write_file(lkm_autoload_file(), on ? "1\n" : "0\n") ? 0 : 1;
    }
    if (sub == "set-kmi") {
        const std::string kmi = arg_or_default(args, 2, "");
        return write_file(lkm_kmi_override_file(), kmi + "\n") ? 0 : 1;
    }
    if (sub == "clear-kmi") {
        std::error_code ec;
        fs::remove(lkm_kmi_override_file(), ec);
        return ec ? 1 : 0;
    }
    if (sub == "load" || sub == "unload") {
        if ((sub == "load" && kasumi::is_available()) ||
            (sub == "unload" && !kasumi::is_available())) {
            return 0;
        }
        std::cerr << "Kasumi LKM management is not implemented in this skeleton\n";
        return 1;
    }
    print_usage();
    return 1;
}

static int handle_hide(const std::vector<std::string>& args) {
    const auto sub = arg_or_default(args, 1, "");
    if (sub == "list") {
        const auto rules = load_user_hide_rules();
        print_string_array(rules);
        std::cout << "\n";
        return 0;
    }
    if (sub == "add") {
        const std::string path = arg_or_default(args, 2, "");
        if (path.empty() || path[0] != '/') {
            std::cerr << "hide path must be absolute\n";
            return 1;
        }
        auto rules = load_user_hide_rules();
        if (std::find(rules.begin(), rules.end(), path) == rules.end()) {
            rules.push_back(path);
        }
        if (!save_user_hide_rules(rules)) {
            return 1;
        }
        if (kasumi::is_available()) {
            kasumi::hide_path(path);
        }
        return 0;
    }
    if (sub == "remove") {
        const std::string path = arg_or_default(args, 2, "");
        auto rules = load_user_hide_rules();
        rules.erase(std::remove(rules.begin(), rules.end(), path), rules.end());
        if (!save_user_hide_rules(rules)) {
            return 1;
        }
        if (kasumi::is_available()) {
            kasumi::delete_rule(path);
        }
        return 0;
    }
    print_usage();
    return 1;
}

static int handle_recovery(const std::vector<std::string>& args) {
    const auto sub = arg_or_default(args, 1, "status");
    if (sub == "boot-completed") {
        mount::recovery_boot_completed();
        std::cout << "{\"ok\":true,\"action\":\"boot-completed\"}\n";
        return 0;
    }
    if (sub == "reset") {
        mount::recovery_reset();
        std::cout << "{\"ok\":true,\"action\":\"reset\"}\n";
        return 0;
    }
    if (sub == "status") {
        std::cout << mount::recovery_status_json() << "\n";
        return 0;
    }
    std::cerr << "usage: kagamid recovery status|boot-completed|reset\n";
    return 1;
}

int run_command(const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h") {
        print_usage();
        return args.empty() ? 1 : 0;
    }
    if (args[0] == "version" || args[0] == "--version") {
        std::cout << KAGAMI_VERSION << "\n";
        return 0;
    }
    if (args[0] == "daemon") {
        return run_daemon_command(args);
    }
    if (args[0] == "config") {
        return handle_config(args);
    }
    if (args[0] == "api") {
        return handle_api(args);
    }
    if (args[0] == "module") {
        return handle_module(args);
    }
    if (args[0] == "kasumi") {
        return handle_kasumi(args);
    }
    if (args[0] == "debug") {
        if (args.size() >= 2 && (args[1] == "enable" || args[1] == "disable")) {
            return kasumi::set_debug(args[1] == "enable") ? 0 : 1;
        }
        if (args.size() >= 3 && args[1] == "stealth") {
            return kasumi::set_stealth(args[2] == "enable") ? 0 : 1;
        }
        if (args.size() >= 2 && args[1] == "set-uname") {
            return kasumi::set_uname(arg_or_default(args, 2, ""), arg_or_default(args, 3, "")) ? 0 : 1;
        }
        if (args.size() >= 2 && args[1] == "set-cmdline") {
            return kasumi::set_cmdline(arg_or_default(args, 2, "")) ? 0 : 1;
        }
        if (args.size() >= 2 && args[1] == "clear-cmdline") {
            return kasumi::set_cmdline("") ? 0 : 1;
        }
        return 0;
    }
    if (args[0] == "lkm") {
        return handle_lkm(args);
    }
    if (args[0] == "hide") {
        return handle_hide(args);
    }
    if (args[0] == "recovery") {
        return handle_recovery(args);
    }

    print_usage();
    return 1;
}

CommandResult run_command_capture(const std::vector<std::string>& args) {
    std::ostringstream stdout_buffer;
    std::ostringstream stderr_buffer;
    auto* old_stdout = std::cout.rdbuf(stdout_buffer.rdbuf());
    auto* old_stderr = std::cerr.rdbuf(stderr_buffer.rdbuf());
    const int exit_code = run_command(args);
    std::cout.rdbuf(old_stdout);
    std::cerr.rdbuf(old_stderr);
    return {exit_code, stdout_buffer.str(), stderr_buffer.str()};
}

} // namespace kagami
