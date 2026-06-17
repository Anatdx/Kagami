#include "mount/backend.hpp"

#include "core/runtime.hpp"
#include "kagami/kasumi_client.hpp"
#include "mount/magic_mount.hpp"
#include "mount/mount_fs.hpp"
#include "mount/overlayfs.hpp"

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <fstream>
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

MountReport mount_all_enabled(const Config& config) {
    MountReport report;

    const auto modules = enumerate_mountable_modules(config);
    report.modules = static_cast<int>(modules.size());

    // Backend selection: magic mount is the default; opt into overlayfs by
    // disabling magic mount (overlayfs_enabled && !magic_mount_enabled). A proper
    // control-plane selector is a follow-up.
    const bool use_overlay = config.overlayfs_enabled && !config.magic_mount_enabled;
    report.backend = use_overlay ? "overlayfs" : "magic_mount";

    if (!config.magic_mount_enabled && !config.overlayfs_enabled) {
        report.ok = false;
        report.detail = "no mount backend enabled in config";
        return report;
    }

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

    const bool ok = fsutil::run_in_init_mount_ns([&]() {
        return use_overlay ? overlay::mount_modules(modules, config)
                           : magic::mount_modules(modules, config);
    });

    report.ok = ok;
    report.mounts = use_overlay ? 0 : count_committed_mounts();
    report.detail = ok ? "ok" : (report.backend + " reported errors (see daemon.log)");
    if (ok) {
        spawn_boot_completed_watcher(); // clears the bootloop counter once boot completes
    }
    return report;
}

bool unmount_all(const Config& config) {
    const bool use_overlay = config.overlayfs_enabled && !config.magic_mount_enabled;
    return fsutil::run_in_init_mount_ns([&]() {
        return use_overlay ? overlay::unmount_all(config) : magic::unmount_all(config);
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
