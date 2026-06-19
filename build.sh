#!/bin/bash
# Kagami Universal Build Script
# Works on both Linux and macOS, automatically detects OS and NDK

set -e

PROJECT_ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
OUT_DIR="${BUILD_DIR}/out"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Detect OS
OS_TYPE="unknown"
case "$(uname -s)" in
    Linux*)     OS_TYPE="linux";;
    Darwin*)    OS_TYPE="macos";;
    *)          OS_TYPE="unknown";;
esac

print_info() { echo -e "${BLUE}ℹ${NC} $1"; }
print_success() { echo -e "${GREEN}✓${NC} $1"; }
print_warning() { echo -e "${YELLOW}⚠${NC} $1"; }
print_error() { echo -e "${RED}✗${NC} $1"; }

# Find NDK based on OS
find_ndk() {
    if [ -n "$ANDROID_NDK" ] && [ -d "$ANDROID_NDK" ]; then
        print_success "Using NDK from environment: $ANDROID_NDK"
        return 0
    fi

    print_info "Searching for Android NDK..."

    local POSSIBLE_PATHS=(
        "$HOME/Library/Android/sdk/ndk"
        "$HOME/android-sdk/ndk"
        "$HOME/Android/Sdk/ndk"
        "$HOME/android-ndk"
        "/usr/local/share/android-ndk"
        "/opt/android-ndk"
        "$HOME/.local/share/android-ndk"
    )

    for base_path in "${POSSIBLE_PATHS[@]}"; do
        if [ -d "$base_path" ]; then
            ANDROID_NDK=$(find "$base_path" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | sort -V | tail -n 1)
            if [ -n "$ANDROID_NDK" ] && [ -d "$ANDROID_NDK" ]; then
                break
            fi
        fi
    done

    if [ -z "$ANDROID_NDK" ] || [ ! -d "$ANDROID_NDK" ]; then
        if [ "$OS_TYPE" = "macos" ] && command -v brew &> /dev/null; then
            local BREW_NDK=$(brew --prefix android-ndk 2>/dev/null || echo "")
            [ -n "$BREW_NDK" ] && [ -d "$BREW_NDK" ] && ANDROID_NDK="$BREW_NDK"
        fi
    fi

    if [ -z "$ANDROID_NDK" ] || [ ! -d "$ANDROID_NDK" ]; then
        print_error "Android NDK not found! Set ANDROID_NDK=/path/to/ndk"
        exit 1
    fi

    export ANDROID_NDK
    print_success "Found NDK: $ANDROID_NDK"
}

# Check dependencies
check_deps() {
    local missing_deps=()
    command -v cmake &> /dev/null || missing_deps+=("cmake")
    command -v ninja &> /dev/null || missing_deps+=("ninja")
    if [ ${#missing_deps[@]} -gt 0 ]; then
        print_error "Missing dependencies: ${missing_deps[*]}"
        exit 1
    fi
    print_success "All dependencies found (cmake, ninja)"
}

# Stage the static WebUI source (webui/) into module/webroot. webui/ is the
# tracked source of truth; module/webroot is a generated artifact (gitignored).
build_webui() {
    [[ $NO_WEBUI -eq 1 ]] && return 0
    print_info "Staging WebUI (webui/ -> module/webroot)..."
    if [[ ! -f "${PROJECT_ROOT}/webui/index.html" ]]; then
        print_error "webui/index.html not found"
        exit 1
    fi
    rm -rf "${PROJECT_ROOT}/module/webroot"
    cp -r "${PROJECT_ROOT}/webui" "${PROJECT_ROOT}/module/webroot"
    print_success "WebUI staged into module/webroot"
}

# Build the Kagami manager APK (release) and stage it into module/ so the
# packaging step bundles it into the module zip. Signing comes from KAGAMI_*
# (shell env or CI secrets); without them the APK is built unsigned. Skipped on
# --no-manager or when no JDK is present (keeps quick local packaging working).
MANAGER_APK_STAGED=""
build_manager() {
    [[ $NO_MANAGER -eq 1 ]] && { print_info "Skipping manager APK (--no-manager)"; return 0; }
    if ! command -v java &> /dev/null; then
        print_warning "java not found; skipping manager APK (zip won't include it)"
        return 0
    fi

    # Gradle needs the Android SDK; honour env, else probe the usual locations.
    if [ -z "$ANDROID_HOME" ] && [ -z "$ANDROID_SDK_ROOT" ] && [ ! -f "${PROJECT_ROOT}/manager/local.properties" ]; then
        local sdk
        for sdk in "$HOME/Library/Android/sdk" "$HOME/Android/Sdk" "$HOME/android-sdk" "/usr/local/lib/android/sdk"; do
            if [ -d "$sdk" ]; then export ANDROID_HOME="$sdk"; print_info "Android SDK: $sdk"; break; fi
        done
    fi

    print_info "Building Kagami manager APK (gradle assembleRelease)..."
    chmod +x "${PROJECT_ROOT}/manager/gradlew" 2>/dev/null || true
    ( cd "${PROJECT_ROOT}/manager" && ./gradlew --no-daemon ${VERBOSE:+--info} assembleRelease )

    local apk_dir="${PROJECT_ROOT}/manager/app/build/outputs/apk/release"
    local apk
    apk="$(find "$apk_dir" -name '*.apk' 2>/dev/null | grep -vi 'unsigned' | head -n1)"
    [ -z "$apk" ] && apk="$(find "$apk_dir" -name '*.apk' 2>/dev/null | head -n1)"
    if [ -z "$apk" ] || [ ! -f "$apk" ]; then
        print_error "Manager APK not found in $apk_dir after build"
        exit 1
    fi

    MANAGER_APK_STAGED="${PROJECT_ROOT}/module/KagamiManager.apk"
    cp "$apk" "$MANAGER_APK_STAGED"
    case "$apk" in
        *unsigned*) print_warning "Bundled UNSIGNED manager APK — set KAGAMI_* to sign ($(du -h "$apk" | cut -f1))" ;;
        *) print_success "Bundled signed manager APK ($(du -h "$apk" | cut -f1))" ;;
    esac
}

# Configure and build kagamid for a specific architecture
build_arch() {
    local ARCH=$1
    local BUILD_SUBDIR="${BUILD_DIR}/${ARCH}"

    print_info "Building kagamid for ${ARCH}..."
    mkdir -p "${BUILD_SUBDIR}" "${OUT_DIR}"

    cmake -B "${BUILD_SUBDIR}" \
        -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="${ANDROID_NDK}/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="${ARCH}" \
        -DANDROID_PLATFORM=android-26 \
        ${VERBOSE} \
        "${PROJECT_ROOT}"
    cmake --build "${BUILD_SUBDIR}" --target kagamid

    local BIN="${BUILD_SUBDIR}/kagamid-${ARCH}"
    if [ -f "$BIN" ]; then
        cp "$BIN" "${OUT_DIR}/"
        print_success "Built kagamid-${ARCH} ($(du -h "$BIN" | cut -f1))"
    else
        print_error "Binary kagamid-${ARCH} not found!"
        exit 1
    fi
}

# Stamp module.prop from git
stamp_version() {
    [ -d "${PROJECT_ROOT}/.git" ] || return 0
    local COMMIT_COUNT VERSION_TAG PROP
    COMMIT_COUNT=$(git -C "${PROJECT_ROOT}" rev-list --count HEAD 2>/dev/null || echo "1")
    VERSION_TAG=$(git -C "${PROJECT_ROOT}" describe --tags --abbrev=0 2>/dev/null || echo "v0.1.0")
    PROP="${PROJECT_ROOT}/module/module.prop"
    if [ "$OS_TYPE" = "macos" ]; then
        sed -i '' "s/^version=.*/version=${VERSION_TAG}/" "$PROP"
        sed -i '' "s/^versionCode=.*/versionCode=${COMMIT_COUNT}/" "$PROP"
    else
        sed -i "s/^version=.*/version=${VERSION_TAG}/" "$PROP"
        sed -i "s/^versionCode=.*/versionCode=${COMMIT_COUNT}/" "$PROP"
    fi
    print_info "Module version: ${VERSION_TAG} (versionCode=${COMMIT_COUNT})"
}

# Main
COMMAND="${1:-package}"
shift || true
NO_WEBUI=0
NO_MANAGER=0
VERBOSE=""
while [[ $# -gt 0 ]]; do
    case $1 in
        --no-webui) NO_WEBUI=1; shift ;;
        --no-manager) NO_MANAGER=1; shift ;;
        --verbose|-v) VERBOSE="--log-level=VERBOSE"; shift ;;
        *) shift ;;
    esac
done

echo ""
echo "╔════════════════════════════════════════╗"
echo "║   Kagami Universal Build Script        ║"
echo "║   OS: $(printf '%-31s' "$OS_TYPE")  ║"
echo "╚════════════════════════════════════════╝"
echo ""

check_deps
find_ndk

case $COMMAND in
    init)
        mkdir -p "${BUILD_DIR}"; print_success "Initialized."
        ;;
    webui)
        NO_WEBUI=0; build_webui
        ;;
    arm64)
        build_arch "arm64-v8a"
        ;;
    manager)
        build_manager
        ;;
    package)
        build_webui
        build_arch "arm64-v8a"
        build_manager
        stamp_version
        print_info "Packaging (cmake)..."
        cmake --build "${BUILD_DIR}/arm64-v8a" --target package
        [ -n "$MANAGER_APK_STAGED" ] && rm -f "$MANAGER_APK_STAGED"
        ;;
    clean)
        rm -rf "${BUILD_DIR}"; print_success "Cleaned."
        ;;
    *)
        echo "Usage: $0 {init|webui|manager|arm64|package|clean} [--no-webui] [--no-manager] [--verbose]"
        exit 1
        ;;
esac

echo ""
print_success "Build completed!"
echo ""
