#include "mount/backend.hpp"

#include "core/json_value.hpp"
#include "core/runtime.hpp"
#include "kagami/kasumi_client.hpp"
#include "mount/magic_mount.hpp"
#include "mount/mount_fs.hpp"
#include "mount/overlayfs.hpp"

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include <sys/system_properties.h>
#include <sys/wait.h>
#include <unistd.h>

namespace kagami::mount {

namespace fs = std::filesystem;

std::string backend_kind_name(BackendKind kind) {
    switch (kind) {
    case BackendKind::Kasumi:
        return "kasumi";
    case BackendKind::Overlayfs:
        return "overlayfs";
    case BackendKind::MagicMount:
        return "magic_mount";
    }
    return "unknown";
}

static bool proc_filesystems_has(const std::string& name) {
    std::ifstream in("/proc/filesystems");
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream parts(line);
        std::string first;
        std::string second;
        parts >> first >> second;
        if (first == name || second == name) {
            return true;
        }
    }
    return false;
}

static int count_committed_mounts() {
    std::ifstream in((runtime_data_dir() / "run" / "magic_mounts.list").string());
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        if (!line.empty()) {
            ++n;
        }
    }
    return n;
}

// --- bootloop protection state ---
constexpr int kMaxBootAttempts = 3;

static fs::path recovery_attempts_file() { return runtime_data_dir() / "run" / "boot_attempts"; }
static fs::path recovery_disabled_file() { return runtime_data_dir() / "run" / "mount_disabled"; }

static int read_boot_attempts() {
    std::ifstream in(recovery_attempts_file());
    int n = 0;
    in >> n;
    return n > 0 ? n : 0;
}

static void write_boot_attempts(int n) {
    std::error_code ec;
    fs::create_directories(recovery_attempts_file().parent_path(), ec);
    std::ofstream out(recovery_attempts_file(), std::ios::trunc);
    out << n << "\n";
}

// After a successful mount, daemonize a watcher that clears the bootloop counter
// once sys.boot_completed=1 (a fallback alongside boot-completed.sh). Double-fork
// + setsid so it outlives the short-lived metamount.sh process that invoked us.
static void spawn_boot_completed_watcher() {
    const pid_t pid = fork();
    if (pid < 0) {
        return;
    }
    if (pid > 0) {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        return;
    }
    setsid();
    if (fork() != 0) {
        _exit(0); // intermediate child exits; grandchild reparents to init
    }
    for (int i = 0; i < 150; ++i) { // ~5 min ceiling
        char buf[PROP_VALUE_MAX] = {};
        if (__system_property_get("sys.boot_completed", buf) > 0 && buf[0] == '1') {
            break;
        }
        sleep(2);
    }
    recovery_boot_completed();
    _exit(0);
}

std::vector<BackendStatus> backend_statuses() {
    const auto version = kasumi::version_info();
    const bool kasumi_available = version.status == kasumi::Status::Available;
    std::ostringstream kasumi_detail;
    kasumi_detail << "protocol expected=" << version.expected_protocol
                  << " kernel=" << version.kernel_protocol
                  << " uid=" << version.process_uid
                  << " euid=" << version.process_euid
                  << " status=" << static_cast<int>(version.status);

    const bool overlayfs_available = proc_filesystems_has("overlay");

    const int magic_mounts = count_committed_mounts();
    std::ostringstream magic_detail;
    magic_detail << "Magisk-style systemless mount; live mounts=" << magic_mounts;

    return {
        {BackendKind::Kasumi, "Kasumi", kasumi_available, true, kasumi_detail.str()},
        {BackendKind::Overlayfs, "OverlayFS", overlayfs_available, false,
         overlayfs_available ? "overlay filesystem registered" : "overlay filesystem is not registered"},
        {BackendKind::MagicMount, "Magic Mount", true, false, magic_detail.str()},
    };
}

std::vector<ModuleEntry> enumerate_mountable_modules(const Config& config) {
    std::vector<ModuleEntry> out;
    const fs::path root = config.module_dir.empty() ? runtime_modules_dir() : fs::path(config.module_dir);
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        return out;
    }
    for (const auto& e : fs::directory_iterator(root, ec)) {
        if (!e.is_directory(ec)) {
            continue;
        }
        const fs::path p = e.path();
        if (!fs::exists(p / "module.prop", ec)) {
            continue;
        }
        if (fs::exists(p / "disable", ec) || fs::exists(p / "remove", ec) ||
            fs::exists(p / "skip_mount", ec)) {
            continue;
        }
        out.push_back({p.filename().string(), p});
    }
    std::sort(out.begin(), out.end(),
              [](const ModuleEntry& a, const ModuleEntry& b) { return a.id < b.id; });
    return out;
}

// Read per-module mount modes from module_mode.json:
// {"<id>": "auto|overlay|magic|kasumi|none"}.
static std::map<std::string, std::string> read_module_modes() {
    std::map<std::string, std::string> out;
    std::ifstream in((runtime_data_dir() / "module_mode.json").string());
    if (!in) {
        return out;
    }
    std::stringstream buf;
    buf << in.rdbuf();
    JsonValue root;
    std::string error;
    if (parse_json(buf.str(), root, error) && root.is_object()) {
        for (const auto& [id, value] : root.object_value) {
            if (value.is_string()) {
                out[id] = value.string_value;
            }
        }
    }
    return out;
}

static bool dir_has_direct_files(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return false;
    }
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_directory(ec)) {
            return true; // a non-dir entry sits directly at this level
        }
    }
    return false;
}

// A module needs magic mount when it places files directly at a partition root
// (e.g. system/build.prop): overlay only stacks on leaf subdirs, never on a
// partition root. Everything else can be overlaid.
static bool module_needs_magic(const ModuleEntry& m) {
    if (dir_has_direct_files(m.path / "system")) {
        return true;
    }
    for (const auto& part : fsutil::kManagedPartitions) {
        if (part == "system") {
            continue;
        }
        if (dir_has_direct_files(m.path / part) ||
            dir_has_direct_files(m.path / "system" / part)) {
            return true;
        }
    }
    return false;
}

// True if the module has any managed-partition tree (i.e. contributes mounts).
static bool module_has_content(const ModuleEntry& m) {
    for (const auto& part : fsutil::kManagedPartitions) {
        std::error_code ec;
        if (fs::is_directory(m.path / part, ec)) {
            return true;
        }
    }
    return false;
}

std::string resolve_module_backend(const ModuleEntry& m, const Config& config,
                                   const std::map<std::string, std::string>& modes) {
    if (!module_has_content(m)) {
        return "none"; // no managed-partition tree → contributes no mounts
    }
    std::string mode = "auto";
    const std::string& global = config.mount_backend;
    if (!global.empty() && global != "auto") {
        mode = global; // global override forces every module
    } else {
        const auto it = modes.find(m.id);
        if (it != modes.end() && !it->second.empty()) {
            mode = it->second;
        }
    }
    if (mode == "auto") {
        const bool can_overlay = proc_filesystems_has("overlay");
        mode = (can_overlay && !module_needs_magic(m)) ? "overlay" : "magic";
    }
    return mode;
}

// Rewrite Kagami's own module.prop description so the manager's module list shows
// the live mount status (like hymo). Keeps the file's inode/context (truncate).
static void update_self_status(bool ok, std::size_t overlay, std::size_t magic,
                              std::size_t kasumi) {
    const fs::path prop = runtime_modules_dir() / "kagami" / "module.prop";
    std::ifstream in(prop);
    if (!in) {
        return;
    }
    std::ostringstream desc;
    desc << (ok ? "😋" : "😭") << " Kagami · Overlay " << overlay << " / Magic " << magic
         << " / Kasumi " << kasumi;

    std::string content;
    std::string line;
    bool desc_done = false;
    while (std::getline(in, line)) {
        if (line.rfind("description=", 0) == 0) {
            content += "description=" + desc.str() + "\n";
            desc_done = true;
        } else {
            content += line + "\n";
        }
    }
    in.close();
    if (!desc_done) {
        content += "description=" + desc.str() + "\n";
    }
    std::ofstream out(prop, std::ios::trunc);
    out << content;
}

MountReport mount_all_enabled(const Config& config) {
    MountReport report;

    const auto modules = enumerate_mountable_modules(config);
    report.modules = static_cast<int>(modules.size());

    // Bootloop protection: refuse to mount if previous boots never confirmed
    // completion, so a bad mount cannot brick boot. The counter is bumped here
    // and cleared by recovery_boot_completed() once boot_completed is reached.
    std::error_code ec;
    fs::create_directories(recovery_attempts_file().parent_path(), ec);
    if (fs::exists(recovery_disabled_file(), ec)) {
        report.ok = true;
        report.detail = "mounting disabled by bootloop protection; run 'kagamid recovery reset'";
        return report;
    }
    const int attempts = read_boot_attempts();
    if (attempts >= kMaxBootAttempts) {
        std::ofstream(recovery_disabled_file().string(), std::ios::trunc).put('\n');
        report.ok = true;
        report.detail = "bootloop protection tripped after " + std::to_string(attempts) +
                        " unconfirmed boots; mounting disabled (run 'kagamid recovery reset')";
        return report;
    }
    write_boot_attempts(attempts + 1);

    // Orchestrate per module: a global override (config.mount_backend != "auto")
    // forces every module; otherwise each module's mode (module_mode.json, default
    // "auto") decides, with auto falling back overlay -> magic -> none.
    const auto modes = read_module_modes();
    std::vector<ModuleEntry> overlay_set;
    std::vector<ModuleEntry> magic_set;
    std::vector<ModuleEntry> kasumi_set;
    for (const auto& m : modules) {
        const std::string mode = resolve_module_backend(m, config, modes);
        if (mode == "overlay") {
            overlay_set.push_back(m);
        } else if (mode == "magic") {
            magic_set.push_back(m);
        } else if (mode == "kasumi") {
            kasumi_set.push_back(m);
        }
        // "none" → skip
    }

    fsutil::mlog("orchestrator: overlay=" + std::to_string(overlay_set.size()) +
                 " magic=" + std::to_string(magic_set.size()) +
                 " kasumi=" + std::to_string(kasumi_set.size()));

    bool ok = true;
    if (!overlay_set.empty() || !magic_set.empty()) {
        ok = fsutil::run_in_init_mount_ns([&]() {
            bool r = true;
            // Magic first: magic::mount_modules clears stale KSU mounts at start,
            // which would otherwise tear down overlay's freshly-mounted KSU leaves
            // (both backends use the same mount source).
            if (!magic_set.empty()) {
                r = magic::mount_modules(magic_set, config) && r;
            }
            if (!overlay_set.empty()) {
                r = overlay::mount_modules(overlay_set, config) && r;
            }
            return r;
        });
    }
    if (!kasumi_set.empty()) {
        // Per-module kasumi (LKM) dispatch is a follow-up; flag it for now.
        fsutil::mlog("orchestrator: " + std::to_string(kasumi_set.size()) +
                     " module(s) set to kasumi; LKM per-module wiring pending");
    }

    report.backend = "hybrid(overlay=" + std::to_string(overlay_set.size()) +
                     ",magic=" + std::to_string(magic_set.size()) +
                     ",kasumi=" + std::to_string(kasumi_set.size()) + ")";
    report.ok = ok;
    report.mounts = count_committed_mounts();
    report.detail = ok ? "ok" : "some backends reported errors (see daemon.log)";
    update_self_status(ok, overlay_set.size(), magic_set.size(), kasumi_set.size());
    if (ok) {
        spawn_boot_completed_watcher(); // clears the bootloop counter once boot completes
    }
    return report;
}

bool unmount_all(const Config& config) {
    // Hybrid: both backends may have live mounts; tear down both (each is a
    // source-gated no-op when it owns nothing).
    return fsutil::run_in_init_mount_ns([&]() {
        bool r = true;
        r = overlay::unmount_all(config) && r;
        r = magic::unmount_all(config) && r;
        return r;
    });
}

void recovery_boot_completed() {
    std::error_code ec;
    fs::remove(recovery_attempts_file(), ec); // boot confirmed: clear the counter
}

void recovery_reset() {
    std::error_code ec;
    fs::remove(recovery_attempts_file(), ec);
    fs::remove(recovery_disabled_file(), ec);
}

std::string recovery_status_json() {
    std::error_code ec;
    const bool disabled = fs::exists(recovery_disabled_file(), ec);
    std::ostringstream out;
    out << "{\"boot_attempts\":" << read_boot_attempts()
        << ",\"max_attempts\":" << kMaxBootAttempts
        << ",\"mounting_disabled\":" << (disabled ? "true" : "false") << "}";
    return out.str();
}

} // namespace kagami::mount
