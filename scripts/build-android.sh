#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/.." && pwd)
version=$(cat "$repo_root/VERSION")
if [[ ! "$version" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]]; then
    printf 'VERSION must contain one SemVer MAJOR.MINOR.PATCH value.\n' >&2
    exit 2
fi
ndk_root=${ANDROID_NDK_HOME:-}
openssl_root=${OPENSSL_SOURCE:-}
mdns_library=${MDNS_LIBRARY:-}
openssl_jobs=${OPENSSL_JOBS:-4}
if [[ -z "$ndk_root" || -z "$openssl_root" || -z "$mdns_library" ]]; then
    printf 'Set ANDROID_NDK_HOME, OPENSSL_SOURCE, and MDNS_LIBRARY.\n' >&2
    exit 2
fi

toolchain="$ndk_root/toolchains/llvm/prebuilt/linux-x86_64/bin"
cc="$toolchain/armv7a-linux-androideabi25-clang"
cxx="$toolchain/armv7a-linux-androideabi25-clang++"
ar="$toolchain/llvm-ar"
out="$repo_root/build/android-armv7"
mkdir -p "$out/obj" "$out/lib"

hap_overlay_rel="build/android-armv7/homekitadk-overlay"
hap_overlay="$repo_root/$hap_overlay_rel"
hap_submodule="$repo_root/third_party/HomeKitADK"
rm -rf "$hap_overlay"
mkdir -p "$hap_overlay/PAL/Crypto/OpenSSL" "$hap_overlay/Tests"
git -C "$hap_submodule" show HEAD:PAL/Crypto/OpenSSL/HAPOpenSSL.c \
    > "$hap_overlay/PAL/Crypto/OpenSSL/HAPOpenSSL.c"
git -C "$hap_submodule" show HEAD:Tests/HAPCryptoTest.c > "$hap_overlay/Tests/HAPCryptoTest.c"
if ! git -C "$repo_root" apply --unidiff-zero --directory="$hap_overlay_rel" \
    "$repo_root/patches/homekitadk-openssl-short-nonce.patch"; then
    printf 'Could not apply the pinned HomeKitADK OpenSSL nonce patch. Check the submodule revision.\n' >&2
    exit 1
fi

if [[ ! -f "$openssl_root/Makefile" ]]; then
    (cd "$openssl_root" && ANDROID_NDK_ROOT="$ndk_root" PATH="$toolchain:$PATH" \
        ./Configure android-arm -D__ANDROID_API__=25 no-shared no-ssl no-tests no-apps no-docs \
        no-dso no-engine no-async no-comp)
fi
ANDROID_NDK_ROOT="$ndk_root" PATH="$toolchain:$PATH" make -C "$openssl_root" \
    -j"$openssl_jobs" build_generated libcrypto.a
cp "$openssl_root/libcrypto.a" "$out/lib/"

common=( -Os -fPIC -ffunction-sections -fdata-sections -DIP=1 -DBLE=0
    "-DIHG2_ARM_VERSION=\"$version\""
    -DHAP_ENABLE_DEVELOPMENT_ONLY_CODE=1 -DHAVE_MFI_HW_AUTH=0 -DHAP_LOG_LEVEL=1
    -D_POSIX_C_SOURCE=200809L
    -include netinet/in.h
    -I"$repo_root/include" -I"$repo_root/third_party/mdns"
    -I"$repo_root/third_party/android-compat"
    -I"$repo_root/third_party/nlohmann_json/include"
    -I"$repo_root/third_party/HomeKitADK" -I"$repo_root/third_party/HomeKitADK/HAP"
    -I"$repo_root/third_party/HomeKitADK/External/HTTP"
    -I"$repo_root/third_party/HomeKitADK/External/JSON"
    -I"$repo_root/third_party/HomeKitADK/External/Base64"
    -I"$repo_root/third_party/HomeKitADK/PAL"
    -I"$repo_root/third_party/HomeKitADK/PAL/POSIX"
    -I"$repo_root/third_party/HomeKitADK/PAL/Crypto/MbedTLS"
    -I"$repo_root/third_party/HomeKitADK/Applications/Lightbulb"
    -I"$openssl_root/include" )

sources=()
while IFS= read -r -d '' file; do sources+=("$file"); done < <(
    find "$repo_root/third_party/HomeKitADK/HAP" \
         "$repo_root/third_party/HomeKitADK/External/HTTP" \
         "$repo_root/third_party/HomeKitADK/External/JSON" \
         "$repo_root/third_party/HomeKitADK/External/Base64" \
         -name '*.c' -print0)
while IFS= read -r -d '' file; do sources+=("$file"); done < <(
    find "$repo_root/third_party/HomeKitADK/PAL" -maxdepth 1 -name '*.c' -print0)
while IFS= read -r -d '' file; do sources+=("$file"); done < <(
    find "$repo_root/third_party/HomeKitADK/PAL/POSIX" -name '*.c' -print0)
sources+=( "$hap_overlay/PAL/Crypto/OpenSSL/HAPOpenSSL.c"
           "$repo_root/third_party/HomeKitADK/Applications/Lightbulb/DB.c"
           "$repo_root/src/homekit/switch_db.c"
           "$repo_root/src/homekit/app_callbacks.c"
           "$repo_root/src/homekit/bridge_main.c"
           "$repo_root/src/log_writer.c"
           "$repo_root/src/platform/android_wordexp.c" )

objects=()
for file in "${sources[@]}"; do
    relative=${file#"$repo_root/"}
    object="$out/obj/${relative//\//_}.o"
    "$cc" -std=gnu11 -Wall -Wextra -Werror -Wno-error=deprecated-declarations "${common[@]}" -c "$file" -o "$object"
    objects+=("$object")
done

"$cxx" -std=c++20 -Os -fPIC -ffunction-sections -fdata-sections -fno-rtti \
    -I"$repo_root/include" -I"$repo_root/third_party/nlohmann_json/include" \
    -I"$openssl_root/include" -c "$repo_root/src/vendor_protocol/protocol.cpp" -o "$out/obj/vendor_protocol.o"
"$cxx" -std=c++20 -Os -fPIC -ffunction-sections -fdata-sections -fno-rtti \
    -I"$repo_root/include" -I"$repo_root/third_party/nlohmann_json/include" \
    -c "$repo_root/src/gateway_socket.cpp" -o "$out/obj/gateway_socket.o"
"$cxx" -std=c++20 -Os -fPIC -ffunction-sections -fdata-sections -fno-rtti -pthread \
    -I"$repo_root/include" -I"$repo_root/third_party/nlohmann_json/include" \
    -c "$repo_root/src/runtime.cpp" -o "$out/obj/runtime.o"
"$cxx" -std=c++20 -Os -fPIC -ffunction-sections -fdata-sections -fno-rtti \
    -I"$repo_root/include" -I"$repo_root/third_party/nlohmann_json/include" \
    -c "$repo_root/src/panel_config.cpp" -o "$out/obj/panel_config.o"
"$cxx" -std=c++20 -Os -fPIC -ffunction-sections -fdata-sections -fno-rtti -pthread \
    -I"$repo_root/include" -c "$repo_root/src/lock_presence.cpp" -o "$out/obj/lock_presence.o"

"$cxx" -pthread -Wl,--gc-sections -Wl,--no-undefined -pie -static-libstdc++ \
    "${objects[@]}" "$out/obj/vendor_protocol.o" "$out/obj/gateway_socket.o" "$out/obj/runtime.o" \
    "$out/obj/panel_config.o" "$out/obj/lock_presence.o" \
    "$out/lib/libcrypto.a" "$mdns_library" -lm -ldl -o "$out/ihg2-arm-bridge"
file "$out/ihg2-arm-bridge"
