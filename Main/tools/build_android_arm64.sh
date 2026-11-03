#!/bin/sh
# Recompile precompiled/arm64-v8a/libmathengine.so from the ground up
# (engine boots blank; hosts load precompiled/stdlib.mathc separately)
# and link with -lm -ldl -lc for Android Bionic.
set -eu

NDK_BIN="${1:-${ANDROID_NDK_TOOLCHAIN_BIN:-}}"
if [ -z "$NDK_BIN" ]; then
    for cand in \
        "${ANDROID_NDK_HOME:-}/toolchains/llvm/prebuilt/"*/bin \
        "${ANDROID_NDK_ROOT:-}/toolchains/llvm/prebuilt/"*/bin \
        "${ANDROID_HOME:-}/ndk/"*/toolchains/llvm/prebuilt/*/bin \
        /opt/android_sdk/ndk/*/toolchains/llvm/prebuilt/*/bin \
        "$(dirname "$(command -v aarch64-linux-android24-clang 2>/dev/null || command -v clang 2>/dev/null || true)")"
    do
        if [ -n "$cand" ] && [ -x "$cand/aarch64-linux-android24-clang" ]; then
            NDK_BIN="$cand"
            break
        fi
    done
fi
[ -n "$NDK_BIN" ] || { echo "error: cannot locate NDK toolchain bin (set ANDROID_NDK_TOOLCHAIN_BIN)" >&2; exit 1; }
echo "using NDK toolchain: $NDK_BIN"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
API=24
ABI=arm64-v8a
PRECOMPILED="$ROOT/precompiled"
OUT="$PRECOMPILED/$ABI"
TMP="$PRECOMPILED/.build-$ABI"
ARCH=aarch64
CC="$NDK_BIN/${ARCH}-linux-android${API}-clang"
LD="$NDK_BIN/lld"


# NOTE: the engine always boots blank; the stdlib image is loaded from disk
# by the host (no embedded k_stdlib_mathc anymore).
CFLAGS="--sysroot=$NDK_BIN/../sysroot -O2 -std=c11 -fPIC -fvisibility=hidden -fstack-protector-strong \
 -DANDROID -DMATH_ENGINE_SHARED=1 -D_FORTIFY_SOURCE=2 -ffunction-sections -fdata-sections"

INCS="-I$ROOT/Core/Headers -I$ROOT/Core/compile -I$ROOT/Core/jni -I$ROOT/Core/lib/delegate -I$ROOT/Core/lib/hardcode"

rm -rf "$TMP"; mkdir -p "$TMP" "$OUT"

OBJS=""
i=0
for src in \
    "$ROOT/Core/Code/"*.c \
    "$ROOT/Core/compile/"*.c \
    "$ROOT/Core/jni/"*.c \
    "$ROOT/Core/lib/delegate/"*.c \
    "$ROOT/Core/lib/hardcode/"*.c
do
    i=$((i+1))
    obj="$TMP/obj_$(printf '%02d' "$i")_$(basename "${src%.c}").o"
    "$CC" $CFLAGS $INCS -c "$src" -o "$obj"
    OBJS="$OBJS $obj"
    echo "cc  $(basename "$src")"
done

"$LD" -flavor gnu -m aarch64linux \
    -shared --sysroot="$NDK_BIN/../sysroot" \
    --gc-sections \
    -soname libmathengine.so \
    -o "$TMP/libmathengine.so" \
    -L"$NDK_BIN/../sysroot/usr/lib/$ARCH-linux-android/$API" \
    -lm -ldl -lc \
    $OBJS

"$NDK_BIN/llvm-strip" "$TMP/libmathengine.so"
mv "$TMP/libmathengine.so" "$OUT/libmathengine.so"
rm -rf "$TMP"
echo "built:  $OUT/libmathengine.so ($(ls -lh "$OUT/libmathengine.so" | awk '{print $5}'))"
