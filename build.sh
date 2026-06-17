#!/usr/bin/env bash
#
# Kagami universal build script (Linux/macOS). Builds the kagamid daemon with the
# Android NDK and packages the metamodule into a KernelSU-flashable zip. Packaging
# delegates to scripts/package.sh (the proven path); this adds NDK auto-detect,
# dependency checks, a fresh WebUI build, and multi-ABI convenience on top.
#
# Usage:
#   ./build.sh                 # package arm64-v8a (default)
#   ./build.sh build           # compile kagamid only (no zip)
#   ./build.sh package --all-abi
#   ./build.sh package --abi x86_64
#   ./build.sh webui           # rebuild module/webroot from webui/
#   ./build.sh clean
#
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
info()  { echo -e "${BLUE}ℹ${NC} $1"; }
ok()    { echo -e "${GREEN}✓${NC} $1"; }
warn()  { echo -e "${YELLOW}⚠${NC} $1"; }
err()   { echo -e "${RED}✗${NC} $1"; }

find_ndk() {
    local n="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-${ANDROID_NDK:-}}}"
    if [ -z "$n" ]; then
        for base in "$HOME/Library/Android/sdk/ndk" "$HOME/Android/Sdk/ndk" \
                    "$HOME/android-sdk/ndk" /opt/android-ndk*; do
            [ -d "$base" ] || continue
            local cand
            cand="$(find "$base" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | sort -V | tail -n 1)"
            [ -n "$cand" ] && n="$cand"
        done
        if [ -z "$n" ] && command -v brew >/dev/null 2>&1; then
            local b; b="$(brew --prefix android-ndk 2>/dev/null || true)"
            [ -d "$b" ] && n="$b"
        fi
    fi
    if [ -z "$n" ] || [ ! -f "$n/build/cmake/android.toolchain.cmake" ]; then
        err "Android NDK not found; set ANDROID_NDK_HOME"
        exit 1
    fi
    export ANDROID_NDK_HOME="$n"
    ok "NDK: $n"
}

check_deps() {
    local missing=()
    for t in cmake zip; do command -v "$t" >/dev/null 2>&1 || missing+=("$t"); done
    if [ "${#missing[@]}" -gt 0 ]; then
        err "missing dependencies: ${missing[*]}"
        exit 1
    fi
    ok "dependencies ok (cmake, zip)"
}

build_webui() {
    [ "$NO_WEBUI" -eq 1 ] && { info "skip webui (using committed module/webroot)"; return 0; }
    if ! command -v npm >/dev/null 2>&1; then
        warn "npm not found; using committed module/webroot"
        return 0
    fi
    info "building webui..."
    ( cd "$PROJECT_ROOT/webui" && npm ci && npm run build )
    rm -rf "$PROJECT_ROOT/module/webroot"
    cp -r "$PROJECT_ROOT/webui/dist" "$PROJECT_ROOT/module/webroot"
    ok "webui built"
}

build_arch() {
    local abi="$1"
    info "compiling kagamid ($abi)..."
    cmake -S "$PROJECT_ROOT" -B "$PROJECT_ROOT/build/$abi" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$abi" -DANDROID_PLATFORM="${ANDROID_PLATFORM:-android-26}" \
        -DKAGAMI_BUILD_WEBUI=OFF >/dev/null
    cmake --build "$PROJECT_ROOT/build/$abi" --target kagamid
    ok "built kagamid-$abi"
}

package_arch() {
    local abi="$1"
    info "packaging ($abi)..."
    ABI="$abi" ANDROID_NDK_HOME="$ANDROID_NDK_HOME" "$PROJECT_ROOT/scripts/package.sh"
}

CMD="${1:-package}"; shift || true
NO_WEBUI=0
ABIS=("arm64-v8a")
while [ $# -gt 0 ]; do
    case "$1" in
        --no-webui) NO_WEBUI=1 ;;
        --all-abi)  ABIS=("arm64-v8a" "armeabi-v7a" "x86_64") ;;
        --abi)      shift; ABIS=("$1") ;;
        *)          warn "ignoring unknown arg: $1" ;;
    esac
    shift
done

check_deps
find_ndk

case "$CMD" in
    build)   for a in "${ABIS[@]}"; do build_arch "$a"; done ;;
    webui)   NO_WEBUI=0; build_webui ;;
    package) build_webui; for a in "${ABIS[@]}"; do package_arch "$a"; done ;;
    clean)   rm -rf "$PROJECT_ROOT/build" "$PROJECT_ROOT"/build-android-*; ok "cleaned" ;;
    *)
        echo "usage: $0 {build|webui|package|clean} [--abi <abi>|--all-abi] [--no-webui]"
        exit 1
        ;;
esac
ok "done"
