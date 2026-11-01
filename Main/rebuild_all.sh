#!/bin/sh
# Clean and recompile both precompiled/stdlib.mathc (~239 KB) and
# precompiled/arm64-v8a/libmathengine.so (~287 KB, separated stdlib) from scratch.
set -eu
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

echo "1. Cleaning previous builds..."
rm -rf DebugBuild ReleaseBuild AndroidBuild precompiled/stdlib.mathc precompiled/arm64-v8a/libmathengine.so

echo "2. Compiling Library/*.math -> precompiled/stdlib.mathc..."
make stdlib

echo "3. Cross-compiling Core/ -> precompiled/arm64-v8a/libmathengine.so (~287 KB)..."
./tools/build_android_arm64.sh "$@"

echo "4. Updating patches/manifest.json..."
python3 ./tools/generate_patch_manifest.py

echo "✅ Done! Artifacts in precompiled/:"
ls -lh precompiled/stdlib.mathc precompiled/arm64-v8a/libmathengine.so
