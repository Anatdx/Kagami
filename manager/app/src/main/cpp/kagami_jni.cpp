#include "kagami/kasumi_client.hpp"

#include <jni.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <cerrno>
#include <cstring>
#include <sstream>
#include <string>
#include <cstdlib>
#include <cstdint>
#include <vector>

static std::string json_quote(const std::string &value) {
    std::ostringstream out;
    out << '"';
    for (char ch : value) {
        switch (ch) {
            case '\\':
                out << "\\\\";
                break;
            case '"':
                out << "\\\"";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default: {
                const auto byte = static_cast<unsigned char>(ch);
                if (byte < 0x20) {
                    out << "\\u00";
                    const char *hex = "0123456789abcdef";
                    out << hex[(byte >> 4) & 0x0f] << hex[byte & 0x0f];
                } else {
                    out << ch;
                }
            }
        }
    }
    out << '"';
    return out.str();
}

static jstring to_jstring(JNIEnv *env, const std::string &value) {
    return env->NewStringUTF(value.c_str());
}

static std::string feature_names_json(int bitmask) {
    const std::vector<std::string> names = kagami::kasumi::feature_names(bitmask);
    std::ostringstream out;
    out << '[';
    for (size_t i = 0; i < names.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        out << json_quote(names[i]);
    }
    out << ']';
    return out.str();
}

static std::string uids_json(const std::vector<std::uint32_t> &uids) {
    std::ostringstream out;
    out << '[';
    for (size_t i = 0; i < uids.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        out << uids[i];
    }
    out << ']';
    return out.str();
}

static std::string policy_owner_name(kagami::kasumi::PolicyOwner owner) {
    switch (owner) {
        case kagami::kasumi::PolicyOwner::Auto:
            return "auto";
        case kagami::kasumi::PolicyOwner::KernelSU:
            return "kernelsu";
        case kagami::kasumi::PolicyOwner::APatch:
            return "apatch";
        case kagami::kasumi::PolicyOwner::Magisk:
            return "magisk";
        case kagami::kasumi::PolicyOwner::Manual:
            return "manual";
        case kagami::kasumi::PolicyOwner::Disabled:
            return "disabled";
    }
    return "unknown";
}

static bool parse_policy_owner(const char *value, kagami::kasumi::PolicyOwner *owner) {
    if (value == nullptr) {
        return false;
    }
    const std::string text(value);
    if (text == "auto") {
        *owner = kagami::kasumi::PolicyOwner::Auto;
    } else if (text == "kernelsu" || text == "ksu") {
        *owner = kagami::kasumi::PolicyOwner::KernelSU;
    } else if (text == "apatch") {
        *owner = kagami::kasumi::PolicyOwner::APatch;
    } else if (text == "magisk") {
        *owner = kagami::kasumi::PolicyOwner::Magisk;
    } else if (text == "manual") {
        *owner = kagami::kasumi::PolicyOwner::Manual;
    } else if (text == "disabled" || text == "off") {
        *owner = kagami::kasumi::PolicyOwner::Disabled;
    } else {
        return false;
    }
    return true;
}

static bool parse_policy_uid_list(const char *value, kagami::kasumi::PolicyUidList *list) {
    if (value == nullptr) {
        return false;
    }
    const std::string text(value);
    if (text == "allow" || text == "allowlist") {
        *list = kagami::kasumi::PolicyUidList::Allow;
    } else if (text == "deny" || text == "denylist") {
        *list = kagami::kasumi::PolicyUidList::Deny;
    } else if (text == "all") {
        *list = kagami::kasumi::PolicyUidList::All;
    } else {
        return false;
    }
    return true;
}

static std::string policy_json() {
    const auto state = kagami::kasumi::policy_state();
    const auto allow_uids = state.ok ? kagami::kasumi::policy_uids(kagami::kasumi::PolicyUidList::Allow) : std::vector<std::uint32_t>{};
    const auto deny_uids = state.ok ? kagami::kasumi::policy_uids(kagami::kasumi::PolicyUidList::Deny) : std::vector<std::uint32_t>{};
    std::ostringstream out;
    out << "{"
        << "\"ok\":" << (state.ok ? "true" : "false") << ","
        << "\"errno\":" << state.last_errno << ","
        << "\"err\":" << state.err << ","
        << "\"api_version\":" << state.version << ","
        << "\"owner\":" << json_quote(policy_owner_name(state.owner)) << ","
        << "\"effective_owner\":" << json_quote(policy_owner_name(state.effective_owner)) << ","
        << "\"flags\":" << state.flags << ","
        << "\"detected_roots\":" << state.detected_roots << ","
        << "\"allow_count\":" << state.allow_count << ","
        << "\"deny_count\":" << state.deny_count << ","
        << "\"max_uid_count\":" << state.max_uid_count << ","
        << "\"allow_uids\":" << uids_json(allow_uids) << ","
        << "\"deny_uids\":" << uids_json(deny_uids)
        << "}";
    return out.str();
}

static unsigned long parse_unsigned(const char *value, bool *ok) {
    if (value == nullptr || value[0] == '\0') {
        *ok = false;
        return 0;
    }
    char *end = nullptr;
    errno = 0;
    unsigned long result = std::strtoul(value, &end, 0);
    *ok = errno == 0 && end != value && *end == '\0';
    return result;
}

static std::string native_snapshot_json(bool ignore_protocol_mismatch) {
    const auto version = kagami::kasumi::version_info();
    const bool protocol_mismatch = version.status == kagami::kasumi::Status::KernelTooOld ||
                                   version.status == kagami::kasumi::Status::ClientTooOld;
    const bool available = version.status == kagami::kasumi::Status::Available ||
                           (ignore_protocol_mismatch && protocol_mismatch && version.kernel_protocol > 0);
    const int bitmask = available ? kagami::kasumi::features() : 0;
    const std::string hooks = available ? kagami::kasumi::hooks() : "";

    std::ostringstream out;
    out << "{"
        << "\"transport\":\"rootservice\","
        << "\"available\":" << (available ? "true" : "false") << ","
        << "\"protocol_ignored\":" << (ignore_protocol_mismatch && protocol_mismatch ? "true" : "false") << ","
        << "\"status\":" << static_cast<int>(version.status) << ","
        << "\"expected_protocol\":" << version.expected_protocol << ","
        << "\"kernel_protocol\":" << version.kernel_protocol << ","
        << "\"uid\":" << version.process_uid << ","
        << "\"euid\":" << version.process_euid << ","
        << "\"last_errno\":" << version.last_errno << ","
        << "\"last_error\":" << json_quote(version.last_errno == 0 ? "" : std::strerror(version.last_errno)) << ","
        << "\"modules_visible\":" << (version.modules_visible ? "true" : "false") << ","
        << "\"mount_base\":" << json_quote(kagami::kasumi::default_mirror_path()) << ","
        << "\"features\":{\"bitmask\":" << bitmask << ",\"names\":" << feature_names_json(bitmask) << "},"
        << "\"hooks\":" << json_quote(hooks) << ","
        << "\"policy\":" << policy_json()
        << "}";
    return out.str();
}

static std::string action_json(bool ok) {
    std::ostringstream out;
    out << "{"
        << "\"ok\":" << (ok ? "true" : "false") << ","
        << "\"errno\":" << (ok ? 0 : errno) << ","
        << "\"error\":" << json_quote(ok ? "" : std::strerror(errno))
        << "}";
    return out.str();
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_anatdx_kagami_KasumiNative_snapshotJson(JNIEnv *env, jobject, jboolean ignore_protocol_mismatch) {
    return to_jstring(env, native_snapshot_json(ignore_protocol_mismatch == JNI_TRUE));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_anatdx_kagami_KasumiNative_setEnabled(JNIEnv *env, jobject, jboolean enabled) {
    return to_jstring(env, action_json(kagami::kasumi::set_enabled(enabled == JNI_TRUE)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_anatdx_kagami_KasumiNative_setDebug(JNIEnv *env, jobject, jboolean enabled) {
    return to_jstring(env, action_json(kagami::kasumi::set_debug(enabled == JNI_TRUE)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_anatdx_kagami_KasumiNative_setMountHide(JNIEnv *env, jobject, jboolean enabled) {
    return to_jstring(env, action_json(kagami::kasumi::set_mount_hide(enabled == JNI_TRUE)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_anatdx_kagami_KasumiNative_setMapsSpoof(JNIEnv *env, jobject, jboolean enabled) {
    return to_jstring(env, action_json(kagami::kasumi::set_maps_spoof(enabled == JNI_TRUE)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_anatdx_kagami_KasumiNative_setStatfsSpoof(JNIEnv *env, jobject, jboolean enabled) {
    return to_jstring(env, action_json(kagami::kasumi::set_statfs_spoof(enabled == JNI_TRUE)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_anatdx_kagami_KasumiNative_setSelinuxGuard(JNIEnv *env, jobject, jboolean enabled) {
    return to_jstring(env, action_json(kagami::kasumi::set_selinux_guard(enabled == JNI_TRUE)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_anatdx_kagami_KasumiNative_hidePath(JNIEnv *env, jobject, jstring path) {
    const char *native_path = env->GetStringUTFChars(path, nullptr);
    const bool ok = native_path != nullptr && kagami::kasumi::hide_path(native_path);
    if (native_path != nullptr) {
        env->ReleaseStringUTFChars(path, native_path);
    }
    return to_jstring(env, action_json(ok));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_anatdx_kagami_KasumiNative_deleteRule(JNIEnv *env, jobject, jstring path) {
    const char *native_path = env->GetStringUTFChars(path, nullptr);
    const bool ok = native_path != nullptr && kagami::kasumi::delete_rule(native_path);
    if (native_path != nullptr) {
        env->ReleaseStringUTFChars(path, native_path);
    }
    return to_jstring(env, action_json(ok));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_anatdx_kagami_KasumiNative_addMapsRule(JNIEnv *env, jobject, jstring target_ino, jstring target_dev, jstring spoofed_ino, jstring spoofed_dev, jstring spoofed_path) {
    const char *native_target_ino = env->GetStringUTFChars(target_ino, nullptr);
    const char *native_target_dev = env->GetStringUTFChars(target_dev, nullptr);
    const char *native_spoofed_ino = env->GetStringUTFChars(spoofed_ino, nullptr);
    const char *native_spoofed_dev = env->GetStringUTFChars(spoofed_dev, nullptr);
    const char *native_spoofed_path = env->GetStringUTFChars(spoofed_path, nullptr);
    bool ok_target_ino = false;
    bool ok_target_dev = false;
    bool ok_spoofed_ino = false;
    bool ok_spoofed_dev = false;
    const unsigned long parsed_target_ino = parse_unsigned(native_target_ino, &ok_target_ino);
    const unsigned long parsed_target_dev = parse_unsigned(native_target_dev, &ok_target_dev);
    const unsigned long parsed_spoofed_ino = parse_unsigned(native_spoofed_ino, &ok_spoofed_ino);
    const unsigned long parsed_spoofed_dev = parse_unsigned(native_spoofed_dev, &ok_spoofed_dev);
    bool ok = false;

    if (ok_target_ino && ok_target_dev && ok_spoofed_ino && ok_spoofed_dev && native_spoofed_path != nullptr && native_spoofed_path[0] != '\0') {
        ok = kagami::kasumi::add_maps_rule(parsed_target_ino, parsed_target_dev, parsed_spoofed_ino, parsed_spoofed_dev, native_spoofed_path);
    } else {
        errno = EINVAL;
    }

    if (native_target_ino != nullptr) env->ReleaseStringUTFChars(target_ino, native_target_ino);
    if (native_target_dev != nullptr) env->ReleaseStringUTFChars(target_dev, native_target_dev);
    if (native_spoofed_ino != nullptr) env->ReleaseStringUTFChars(spoofed_ino, native_spoofed_ino);
    if (native_spoofed_dev != nullptr) env->ReleaseStringUTFChars(spoofed_dev, native_spoofed_dev);
    if (native_spoofed_path != nullptr) env->ReleaseStringUTFChars(spoofed_path, native_spoofed_path);
    return to_jstring(env, action_json(ok));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_anatdx_kagami_KasumiNative_clearMapsRules(JNIEnv *env, jobject) {
    return to_jstring(env, action_json(kagami::kasumi::clear_maps_rules()));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_anatdx_kagami_KasumiNative_setPolicy(JNIEnv *env, jobject, jstring owner, jint flags) {
    const char *native_owner = env->GetStringUTFChars(owner, nullptr);
    kagami::kasumi::PolicyOwner parsed_owner = kagami::kasumi::PolicyOwner::Auto;
    bool ok = false;
    if (parse_policy_owner(native_owner, &parsed_owner)) {
        ok = kagami::kasumi::set_policy(parsed_owner, static_cast<std::uint32_t>(flags));
    } else {
        errno = EINVAL;
    }
    if (native_owner != nullptr) {
        env->ReleaseStringUTFChars(owner, native_owner);
    }
    return to_jstring(env, action_json(ok));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_anatdx_kagami_KasumiNative_setPolicyUids(JNIEnv *env, jobject, jstring list, jintArray uids) {
    const char *native_list = env->GetStringUTFChars(list, nullptr);
    kagami::kasumi::PolicyUidList parsed_list = kagami::kasumi::PolicyUidList::Allow;
    bool ok = false;
    if (parse_policy_uid_list(native_list, &parsed_list)) {
        std::vector<std::uint32_t> native_uids;
        const jsize count = uids == nullptr ? 0 : env->GetArrayLength(uids);
        native_uids.resize(static_cast<size_t>(count));
        if (count > 0) {
            std::vector<jint> tmp(static_cast<size_t>(count));
            env->GetIntArrayRegion(uids, 0, count, tmp.data());
            for (jsize i = 0; i < count; ++i) {
                native_uids[static_cast<size_t>(i)] = static_cast<std::uint32_t>(tmp[static_cast<size_t>(i)]);
            }
        }
        ok = kagami::kasumi::set_policy_uids(parsed_list, native_uids);
    } else {
        errno = EINVAL;
    }
    if (native_list != nullptr) {
        env->ReleaseStringUTFChars(list, native_list);
    }
    return to_jstring(env, action_json(ok));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_anatdx_kagami_KasumiNative_clearPolicyUids(JNIEnv *env, jobject, jstring list) {
    const char *native_list = env->GetStringUTFChars(list, nullptr);
    kagami::kasumi::PolicyUidList parsed_list = kagami::kasumi::PolicyUidList::All;
    bool ok = false;
    if (parse_policy_uid_list(native_list, &parsed_list)) {
        ok = kagami::kasumi::clear_policy_uids(parsed_list);
    } else {
        errno = EINVAL;
    }
    if (native_list != nullptr) {
        env->ReleaseStringUTFChars(list, native_list);
    }
    return to_jstring(env, action_json(ok));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_anatdx_kagami_KasumiNative_statPath(JNIEnv *env, jobject, jstring path) {
    const char *native_path = path == nullptr ? nullptr : env->GetStringUTFChars(path, nullptr);
    struct stat st = {};
    bool ok = false;
    int saved_errno = 0;
    if (native_path != nullptr && native_path[0] != '\0') {
        if (stat(native_path, &st) == 0) {
            ok = true;
        } else {
            saved_errno = errno;
        }
    } else {
        saved_errno = EINVAL;
    }
    if (native_path != nullptr) {
        env->ReleaseStringUTFChars(path, native_path);
    }
    const unsigned long ino = ok ? static_cast<unsigned long>(st.st_ino) : 0UL;
    const unsigned long dev = ok ? static_cast<unsigned long>(st.st_dev) : 0UL;
    const unsigned long mode = ok ? static_cast<unsigned long>(st.st_mode) : 0UL;
    const unsigned int major_id = ok ? major(st.st_dev) : 0u;
    const unsigned int minor_id = ok ? minor(st.st_dev) : 0u;
    std::ostringstream out;
    out << "{"
        << "\"ok\":" << (ok ? "true" : "false") << ","
        << "\"errno\":" << saved_errno << ","
        << "\"error\":" << json_quote(ok ? "" : std::strerror(saved_errno)) << ","
        << "\"ino\":" << ino << ","
        << "\"dev\":" << dev << ","
        << "\"dev_major\":" << major_id << ","
        << "\"dev_minor\":" << minor_id << ","
        << "\"mode\":" << mode
        << "}";
    return to_jstring(env, out.str());
}
