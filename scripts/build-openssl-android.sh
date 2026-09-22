#!/usr/bin/env bash
# Build OpenSSL 3.x for Android arm64-v8a using the NDK toolchain.
# Target API 24 (Android 7.0) — matches Qt 6.5's Android floor.
#
# Requires:
#   ANDROID_NDK_ROOT pointing at the NDK root
#   OpenSSL source extracted at deps/openssl-src (or cloned fresh)
#
# Output: deps/openssl-android-arm64/{include,lib}/
set -euo pipefail

cd "$(dirname "$0")/.."

: "${ANDROID_NDK_ROOT:?ANDROID_NDK_ROOT must be set}"
OUT="$(pwd)/deps/openssl-android-arm64"
SRC="$(pwd)/deps/openssl-src"

# 3.5.7 (current LTS branch): 3.0.13 was affected by CVE-2025-15467
# (CMS AuthEnvelopedData stack overflow) and CVE-2026-45447
# (PKCS7_verify UAF). Regenerate deps/openssl-android-arm64 by
# re-running this script after bumping.
OPENSSL_VERSION="3.5.7"
# Re-fetch when the checked-out source doesn't match OPENSSL_VERSION —
# a bare `[ ! -d "$SRC" ]` guard would silently keep building a stale
# version after a bump. deps/ is gitignored, so this only touches the
# dev machine's cache.
if [ ! -f "$SRC/.bsfchat-openssl-version" ] \
   || [ "$(cat "$SRC/.bsfchat-openssl-version" 2>/dev/null)" != "$OPENSSL_VERSION" ]; then
    echo "Fetching OpenSSL ${OPENSSL_VERSION} source…"
    rm -rf "$SRC"
    mkdir -p deps
    curl -sL "https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}/openssl-${OPENSSL_VERSION}.tar.gz" \
        | tar -xz -C deps
    mv "deps/openssl-${OPENSSL_VERSION}" "$SRC"
    echo "$OPENSSL_VERSION" > "$SRC/.bsfchat-openssl-version"
fi

cd "$SRC"
# Clean any prior state to avoid cross-arch contamination.
make clean 2>/dev/null || true
rm -f configdata.pm

# The NDK's prebuilt toolchain lives under a host-triplet directory.
# This used to be hard-coded to darwin-x86_64, which meant the script
# only ever worked on the dev Mac and died on the Linux CI runner with
# a PATH pointing at a directory that does not exist. Detect it.
# (Apple Silicon still uses darwin-x86_64 — the NDK ships one universal
# host toolchain under that name, there is no darwin-arm64 dir.)
case "$(uname -s)" in
    Darwin) NDK_HOST_TAG="darwin-x86_64" ;;
    Linux)  NDK_HOST_TAG="linux-x86_64" ;;
    *) echo "Unsupported host $(uname -s) for the Android NDK" >&2; exit 1 ;;
esac
NDK_TC="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/$NDK_HOST_TAG"
[ -d "$NDK_TC" ] || { echo "NDK toolchain not found at $NDK_TC" >&2; exit 1; }

# Put the NDK's prebuilt llvm toolchain on PATH — OpenSSL's Configure
# hard-codes the `android-arm64` target to look for NDK-style clang.
export PATH="$NDK_TC/bin:$PATH"
export ANDROID_NDK_HOME="$ANDROID_NDK_ROOT"

# getconf rather than `sysctl -n hw.ncpu` (macOS-only) or `nproc`
# (GNU-only): POSIX, and present on both hosts.
NCPU="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

# Shared libs required — Qt on Android pulls libssl/libcrypto via
# androiddeployqt's --extra-libs, which only accepts .so files.
# The OpenSSL "android-arm64" target emits libssl.so.3/libcrypto.so.3
# by default; Qt expects the 3-suffix names.
./Configure android-arm64 \
    -D__ANDROID_API__=24 \
    --prefix="$OUT" \
    --openssldir="$OUT/ssl" \
    shared no-tests no-dso

make -j"$NCPU" build_libs >/dev/null
make install_dev >/dev/null

# Re-link the .so files from the static archives, dropping OpenSSL's
# OPENSSL_3.0.0 version script and giving each .so a Qt-compatible
# SONAME with the `_3` suffix. Two reasons:
#
#  1. Qt for Android's androiddeployqt expects exactly libssl_3.so /
#     libcrypto_3.so (Qt Network looks for that name at runtime).
#  2. OpenSSL's default android-arm64 build stamps the versioned
#     symbol table referencing "libcrypto.so" — after we rename the
#     file to libcrypto_3.so Android's dynamic linker can't find the
#     versioned DT_NEEDED target and refuses to load libssl_3.so with
#     `cannot find "BN_ucmp" from verneed[0]`.
#
# Re-linking from the .a archives with `--whole-archive` sidesteps
# both: no version script is applied, so the resulting .so has no
# .gnu.version_r entries referring to the old unqualified filenames.
# OpenSSL picks its libdir per target; android-arm64 uses lib, but a
# lib64 install would leave CMakeLists.txt and androiddeployqt (both of
# which hard-code $OUT/lib) looking at nothing. Normalise.
if [ ! -d "$OUT/lib" ] && [ -d "$OUT/lib64" ]; then
    ln -sfn lib64 "$OUT/lib"
fi
cd "$OUT/lib"
CC="$NDK_TC/bin/aarch64-linux-android24-clang"

"$CC" -shared \
    -Wl,-soname,libcrypto_3.so \
    -Wl,--no-undefined \
    -Wl,--whole-archive libcrypto.a -Wl,--no-whole-archive \
    -o libcrypto_3.so

"$CC" -shared \
    -Wl,-soname,libssl_3.so \
    -Wl,--no-undefined \
    -Wl,--whole-archive libssl.a -Wl,--no-whole-archive \
    -L. -l:libcrypto_3.so \
    -o libssl_3.so

echo "OpenSSL built at: $OUT"
echo "  libssl_3.so + libcrypto_3.so with Qt-compatible SONAMEs"
