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

if [ ! -d "$SRC" ]; then
    echo "Fetching OpenSSL source…"
    mkdir -p deps
    curl -sL https://www.openssl.org/source/openssl-3.0.13.tar.gz \
        | tar -xz -C deps
    mv deps/openssl-3.0.13 "$SRC"
fi

cd "$SRC"
# Clean any prior state to avoid cross-arch contamination.
make clean 2>/dev/null || true
rm -f configdata.pm

# Put the NDK's prebuilt llvm toolchain on PATH — OpenSSL's Configure
# hard-codes the `android-arm64` target to look for NDK-style clang.
export PATH="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/darwin-x86_64/bin:$PATH"
export ANDROID_NDK_HOME="$ANDROID_NDK_ROOT"

# Shared libs required — Qt on Android pulls libssl/libcrypto via
# androiddeployqt's --extra-libs, which only accepts .so files.
# The OpenSSL "android-arm64" target emits libssl.so.3/libcrypto.so.3
# by default; Qt expects the 3-suffix names.
./Configure android-arm64 \
    -D__ANDROID_API__=24 \
    --prefix="$OUT" \
    --openssldir="$OUT/ssl" \
    shared no-tests no-dso

make -j"$(sysctl -n hw.ncpu)" build_libs >/dev/null
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
cd "$OUT/lib"
NDK_TC="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/darwin-x86_64"
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
