#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/.." && pwd)
ndk_root=${ANDROID_NDK_HOME:-}
openssl_root=${OPENSSL_SOURCE:-}
version=$(cat "$repo_root/VERSION")
if [[ ! "$version" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]]; then
    printf 'VERSION must contain one SemVer MAJOR.MINOR.PATCH value.\n' >&2
    exit 2
fi
default_output="$repo_root/dist/ihg2-arm-native-android-armv7-v$version.tar.gz"
force=0
if [[ $# -gt 2 ]]; then
    printf 'Usage: %s [OUTPUT.tar.gz] [--force]\n' "$0" >&2
    exit 2
fi
if [[ "${1:-}" == --force ]]; then
    output=$default_output
    force=1
else
    output=${1:-$default_output}
    if [[ "${2:-}" == --force ]]; then force=1; elif [[ $# -eq 2 ]]; then
        printf 'Usage: %s [OUTPUT.tar.gz] [--force]\n' "$0" >&2
        exit 2
    fi
fi
binary="$repo_root/build/android-armv7/ihg2-arm-bridge"

if [[ -z "$ndk_root" || ! -x "$ndk_root/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" ]]; then
    printf 'Set ANDROID_NDK_HOME to an Android NDK r30 installation.\n' >&2
    exit 2
fi
if [[ -z "$openssl_root" || ! -f "$openssl_root/LICENSE.txt" ]]; then
    printf 'Set OPENSSL_SOURCE to the OpenSSL source tree used for this build.\n' >&2
    exit 2
fi
if [[ ! -x "$binary" ]]; then
    printf 'ARM bridge binary not found. Run make android first.\n' >&2
    exit 2
fi
if ! LC_ALL=C grep -aFq 'IHG2_ARM_AUTO_ONLY_V1' "$binary"; then
    printf 'ARM bridge is stale or is not an automatic-only build. Run make android first.\n' >&2
    exit 2
fi
if ! LC_ALL=C grep -aFq "IHG2 ARM bridge version $version" "$binary"; then
    printf 'ARM bridge binary version does not match VERSION (%s). Run make android first.\n' "$version" >&2
    exit 2
fi

out_dir=$(dirname "$output")
mkdir -p "$out_dir"
output=$(cd "$out_dir" && pwd)/$(basename "$output")
if [[ -e "$output" && "$force" -ne 1 ]]; then
    printf 'Refusing to overwrite existing release archive: %s. Bump VERSION or pass --force for an intentional rebuild.\n' "$output" >&2
    exit 1
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/ihg2-arm-package.XXXXXX")
archive_tmp="$output.tmp.$$"
checksum_tmp="$output.sha256.tmp.$$"
cleanup() {
    rm -rf "$work"
    rm -f "$archive_tmp"
    rm -f "$checksum_tmp"
}
trap cleanup EXIT

bundle="$work/ihg2-arm-native-android-armv7"
mkdir -p "$bundle"
cp "$repo_root/VERSION" "$bundle/VERSION"
cp "$binary" "$bundle/ihg2-arm-bridge"
"$ndk_root/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" --strip-unneeded "$bundle/ihg2-arm-bridge"
chmod 0755 "$bundle/ihg2-arm-bridge"
cp "$repo_root/scripts/deploy-android.sh" "$bundle/install.sh"
cp "$repo_root/scripts/preflight-android.sh" "$bundle/preflight.sh"
cp "$repo_root/scripts/deploy-android.ps1" "$bundle/install.ps1"
cp "$repo_root/scripts/preflight-android.ps1" "$bundle/preflight.ps1"
cp "$repo_root/scripts/install.cmd" "$bundle/install.cmd"
cp "$repo_root/scripts/supervisor.sh" "$bundle/supervisor.sh"
cp "$repo_root/scripts/install-recovery.sh" "$bundle/install-recovery.sh"
cp "$repo_root/scripts/install-device.sh" "$bundle/install-device.sh"
cp "$repo_root/scripts/preflight-device.sh" "$bundle/preflight-device.sh"
cp "$repo_root/third_party/HomeKitADK/LICENSE.md" "$bundle/LICENSE-HomeKitADK.md"
cp "$openssl_root/LICENSE.txt" "$bundle/LICENSE-OpenSSL.txt"
cp "$repo_root/docs/deployment.md" "$bundle/DEPLOYMENT.md"
cp "$repo_root/docs/auto-adoption.md" "$bundle/AUTO-ADOPTION.md"
cp "$repo_root/README.md" "$bundle/README.md"
chmod 0755 "$bundle/install.sh" "$bundle/preflight.sh" "$bundle/install.ps1" "$bundle/preflight.ps1"

tar -C "$work" -czf "$archive_tmp" "$(basename "$bundle")"
mv -f "$archive_tmp" "$output"
(
    cd "$out_dir"
    sha256sum "$(basename "$output")"
) > "$checksum_tmp"
mv -f "$checksum_tmp" "$output.sha256"
printf 'Created portable release bundle v%s: %s\n' "$version" "$output"
printf 'SHA-256: %s\n' "$(cut -d ' ' -f 1 "$output.sha256")"
printf 'This archive contains no gateway key, inventory, HomeKit pairing database, or owner config.\n'
