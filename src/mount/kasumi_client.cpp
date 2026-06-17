#include "kagami/kasumi_client.hpp"

#include "kagami/kasumi_uapi_compat.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#if defined(__linux__)
#include <sys/types.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace kagami::kasumi {

std::string default_mirror_path() {
    return "/dev/kasumi_mirror";
}

static bool lkm_in_proc_modules() {
#if defined(__linux__)
    std::ifstream modules("/proc/modules");
    std::string line;
    while (std::getline(modules, line)) {
        if (line.compare(0, 11, "kasumi_lkm ") == 0 ||
            line.compare(0, 11, "kasumi_lkm\t") == 0) {
            return true;
        }
    }
#endif
    return false;
}

#if defined(__linux__)
static int s_kasumi_fd = -1;
static int s_last_getfd_errno = 0;

static int anon_fd() {
    if (s_kasumi_fd >= 0) {
        return s_kasumi_fd;
    }

    int fd = -1;
    errno = 0;
    prctl(KSM_PRCTL_GET_FD, reinterpret_cast<unsigned long>(&fd), 0, 0, 0);
    s_last_getfd_errno = errno;
    if (fd < 0) {
        errno = 0;
        syscall(SYS_reboot, KSM_MAGIC1, KSM_MAGIC2, KSM_CMD_GET_FD, &fd);
        s_last_getfd_errno = errno;
    }
    if (fd >= 0) {
        s_kasumi_fd = fd;
        s_last_getfd_errno = 0;
    }
    return fd;
}
#endif

static int execute(unsigned long cmd, void* arg) {
#if defined(__linux__)
    const int fd = anon_fd();
    if (fd < 0) {
#if defined(__linux__)
        if (s_last_getfd_errno != 0) {
            errno = s_last_getfd_errno;
        } else {
            errno = ENODEV;
        }
#else
        errno = ENODEV;
#endif
        return -1;
    }
    return ioctl(fd, cmd, arg);
#else
    (void)cmd;
    (void)arg;
    errno = ENOSYS;
    return -1;
#endif
}

static bool ioctl_arg_ok(int rc, int arg_err) {
    if (rc != 0) {
        return false;
    }
    if (arg_err != 0) {
        errno = arg_err < 0 ? -arg_err : arg_err;
        return false;
    }
    return true;
}

VersionInfo version_info() {
    VersionInfo info;
    info.expected_protocol = KSM_PROTOCOL_VERSION;
    info.process_uid = process_uid();
    info.process_euid = process_euid();

    int version = 0;
    if (execute(KSM_IOC_GET_VERSION, &version) != 0) {
        info.last_errno = errno;
        info.modules_visible = lkm_in_proc_modules();
        info.status = Status::NotPresent;
        return info;
    }

    info.kernel_protocol = version;
    if (version < KSM_PROTOCOL_VERSION) {
        info.status = Status::KernelTooOld;
    } else if (version > KSM_PROTOCOL_VERSION) {
        info.status = Status::ClientTooOld;
    } else {
        info.status = Status::Available;
    }
    return info;
}

int last_getfd_errno() {
#if defined(__linux__)
    return s_last_getfd_errno;
#else
    return ENOSYS;
#endif
}

int process_uid() {
#if defined(__linux__)
    return static_cast<int>(getuid());
#else
    return -1;
#endif
}

int process_euid() {
#if defined(__linux__)
    return static_cast<int>(geteuid());
#else
    return -1;
#endif
}

bool is_available() {
    return version_info().status == Status::Available;
}

std::string active_rules() {
    std::vector<char> buffer(64 * 1024, '\0');
    kasumi_syscall_list_arg arg = {};
    arg.buf = buffer.data();
    arg.size = buffer.size();
    if (execute(KSM_IOC_LIST_RULES, &arg) != 0) {
        return "";
    }
    return std::string(buffer.data());
}

std::string hooks() {
    std::vector<char> buffer(8 * 1024, '\0');
    kasumi_syscall_list_arg arg = {};
    arg.buf = buffer.data();
    arg.size = buffer.size();
    if (execute(KSM_IOC_GET_HOOKS, &arg) != 0) {
        return "";
    }
    return std::string(buffer.data());
}

int features() {
    int bitmask = 0;
    if (execute(KSM_IOC_GET_FEATURES, &bitmask) != 0) {
        return 0;
    }
    return bitmask;
}

std::vector<std::string> feature_names(int bitmask) {
    std::vector<std::string> names;
    if (bitmask & KSM_FEATURE_MOUNT_HIDE)
        names.emplace_back("mount_hide");
    if (bitmask & KSM_FEATURE_MAPS_SPOOF)
        names.emplace_back("maps_spoof");
    if (bitmask & KSM_FEATURE_STATFS_SPOOF)
        names.emplace_back("statfs_spoof");
    if (bitmask & KSM_FEATURE_CMDLINE_SPOOF)
        names.emplace_back("cmdline_spoof");
    if (bitmask & KSM_FEATURE_UNAME_SPOOF)
        names.emplace_back("uname_spoof");
    if (bitmask & KSM_FEATURE_KSTAT_SPOOF)
        names.emplace_back("kstat_spoof");
    if (bitmask & KSM_FEATURE_MERGE_DIR)
        names.emplace_back("merge_dir");
    if (bitmask & KSM_FEATURE_SELINUX_BYPASS)
        names.emplace_back("selinux_bypass");
    if (bitmask & KSM_FEATURE_FAKE_MOUNTINFO)
        names.emplace_back("fake_mountinfo");
    if (bitmask & KSM_FEATURE_SELINUX_FIX)
        names.emplace_back("selinux_fix");
    return names;
}

std::vector<std::string> active_modules_from_rules(const std::string& rules) {
    std::set<std::string> modules;
    std::istringstream lines(rules);
    std::string line;
    while (std::getline(lines, line)) {
        const std::string prefix = "/data/adb/modules/";
        std::size_t pos = line.find(prefix);
        while (pos != std::string::npos) {
            const std::size_t start = pos + prefix.size();
            const std::size_t end = line.find('/', start);
            if (end != std::string::npos && end > start) {
                modules.insert(line.substr(start, end - start));
            }
            pos = line.find(prefix, start);
        }
    }
    return {modules.begin(), modules.end()};
}

bool set_enabled(bool enable) {
    int value = enable ? 1 : 0;
    return execute(KSM_IOC_SET_ENABLED, &value) == 0;
}

bool set_debug(bool enable) {
    int value = enable ? 1 : 0;
    return execute(KSM_IOC_SET_DEBUG, &value) == 0;
}

bool set_stealth(bool enable) {
    int value = enable ? 1 : 0;
    return execute(KSM_IOC_SET_STEALTH, &value) == 0;
}

bool set_mount_hide(bool enable) {
    kasumi_mount_hide_arg arg = {};
    arg.enable = enable ? 1 : 0;
    return ioctl_arg_ok(execute(KSM_IOC_SET_MOUNT_HIDE, &arg), arg.err);
}

bool set_maps_spoof(bool enable) {
    kasumi_maps_spoof_arg arg = {};
    arg.enable = enable ? 1 : 0;
    return ioctl_arg_ok(execute(KSM_IOC_SET_MAPS_SPOOF, &arg), arg.err);
}

bool set_statfs_spoof(bool enable) {
    kasumi_statfs_spoof_arg arg = {};
    arg.enable = enable ? 1 : 0;
    return ioctl_arg_ok(execute(KSM_IOC_SET_STATFS_SPOOF, &arg), arg.err);
}

bool set_selinux_guard(bool enable) {
    int value = enable ? 1 : 0;
    return execute(KSM_IOC_SELINUX_FIX, &value) == 0;
}

bool set_uname(const std::string& release, const std::string& version) {
    kasumi_spoof_uname arg = {};
    std::strncpy(arg.release, release.c_str(), KSM_UNAME_LEN - 1);
    std::strncpy(arg.version, version.c_str(), KSM_UNAME_LEN - 1);
    return ioctl_arg_ok(execute(KSM_IOC_SET_UNAME, &arg), arg.err);
}

bool set_cmdline(const std::string& cmdline) {
    kasumi_spoof_cmdline arg = {};
    std::strncpy(arg.cmdline, cmdline.c_str(), KSM_FAKE_CMDLINE_SIZE - 1);
    return ioctl_arg_ok(execute(KSM_IOC_SET_CMDLINE, &arg), arg.err);
}

bool hide_path(const std::string& path) {
    kasumi_syscall_arg arg = {};
    arg.src = path.c_str();
    return execute(KSM_IOC_HIDE_RULE, &arg) == 0;
}

bool delete_rule(const std::string& path) {
    kasumi_syscall_arg arg = {};
    arg.src = path.c_str();
    return execute(KSM_IOC_DEL_RULE, &arg) == 0;
}

bool add_maps_rule(unsigned long target_ino, unsigned long target_dev, unsigned long spoofed_ino, unsigned long spoofed_dev, const std::string& spoofed_path) {
    kasumi_maps_rule arg = {};
    arg.target_ino = target_ino;
    arg.target_dev = target_dev;
    arg.spoofed_ino = spoofed_ino;
    arg.spoofed_dev = spoofed_dev;
    std::strncpy(arg.spoofed_pathname, spoofed_path.c_str(), KSM_MAX_LEN_PATHNAME - 1);
    return ioctl_arg_ok(execute(KSM_IOC_ADD_MAPS_RULE, &arg), arg.err);
}

bool clear_maps_rules() {
    return execute(KSM_IOC_CLEAR_MAPS_RULES, nullptr) == 0;
}

PolicyState policy_state() {
    PolicyState state;
    kasumi_policy_state_arg arg = {};
    arg.version = KSM_POLICY_API_VERSION;
    arg.size = sizeof(arg);
    if (execute(KSM_IOC_GET_POLICY, &arg) != 0) {
        state.last_errno = errno;
        state.err = errno == 0 ? -1 : -errno;
        return state;
    }
    state.version = arg.version;
    state.owner = static_cast<PolicyOwner>(arg.owner);
    state.effective_owner = static_cast<PolicyOwner>(arg.effective_owner);
    state.flags = arg.flags;
    state.detected_roots = arg.detected_roots;
    state.allow_count = arg.allow_count;
    state.deny_count = arg.deny_count;
    state.max_uid_count = arg.max_uid_count;
    state.err = arg.err;
    state.ok = arg.err == 0;
    return state;
}

std::vector<std::uint32_t> policy_uids(PolicyUidList list) {
    const auto state = policy_state();
    std::uint32_t count = 0;
    if (list == PolicyUidList::Allow) {
        count = state.allow_count;
    } else if (list == PolicyUidList::Deny) {
        count = state.deny_count;
    }
    if (!state.ok || count == 0) {
        return {};
    }

    std::vector<std::uint32_t> uids(count);
    kasumi_policy_uid_list_arg arg = {};
    arg.version = KSM_POLICY_API_VERSION;
    arg.size = sizeof(arg);
    arg.list = static_cast<std::uint32_t>(list);
    arg.count = count;
    arg.uids = static_cast<__aligned_u64>(reinterpret_cast<std::uintptr_t>(uids.data()));
    if (!ioctl_arg_ok(execute(KSM_IOC_GET_POLICY_UIDS, &arg), arg.err)) {
        return {};
    }
    if (arg.count < uids.size()) {
        uids.resize(arg.count);
    }
    return uids;
}

bool set_policy(PolicyOwner owner, std::uint32_t flags) {
    kasumi_policy_config_arg arg = {};
    arg.version = KSM_POLICY_API_VERSION;
    arg.size = sizeof(arg);
    arg.owner = static_cast<std::uint32_t>(owner);
    arg.flags = flags;
    return ioctl_arg_ok(execute(KSM_IOC_SET_POLICY, &arg), arg.err);
}

bool set_policy_uids(PolicyUidList list, const std::vector<std::uint32_t>& uids) {
    kasumi_policy_uid_list_arg arg = {};
    arg.version = KSM_POLICY_API_VERSION;
    arg.size = sizeof(arg);
    arg.list = static_cast<std::uint32_t>(list);
    arg.count = static_cast<std::uint32_t>(uids.size());
    arg.uids = uids.empty() ? 0 : static_cast<__aligned_u64>(reinterpret_cast<std::uintptr_t>(uids.data()));
    return ioctl_arg_ok(execute(KSM_IOC_SET_POLICY_UIDS, &arg), arg.err);
}

bool clear_policy_uids(PolicyUidList list) {
    kasumi_policy_uid_list_arg arg = {};
    arg.version = KSM_POLICY_API_VERSION;
    arg.size = sizeof(arg);
    arg.list = static_cast<std::uint32_t>(list);
    return ioctl_arg_ok(execute(KSM_IOC_CLEAR_POLICY_UIDS, &arg), arg.err);
}

} // namespace kagami::kasumi
