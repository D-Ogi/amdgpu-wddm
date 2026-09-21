#!/bin/sh
# Build the E14 reference on the lab machine. Everything lands in /tmp/e14/build, which is RAM on
# the diagnostic stick: nothing is written to a disk.
#
#   sh build.sh              build from the directory this script lives in
#
# Needs: gcc, musl-dev, vulkan-loader-dev, vulkan-headers, glslang (glslangValidator).
set -eu

SRC=$(cd "$(dirname "$0")" && pwd)
OUT=${E14_OUT:-/tmp/e14/build}
mkdir -p "$OUT/shaders"

echo "== toolchain"
gcc --version | head -1
glslangValidator --version | head -2

echo "== shaders -> $OUT/shaders"
# Plain -V: SPIR-V 1.0 with Vulkan 1.0 semantics, which every Vulkan 1.1 implementation accepts and
# which keeps the binary as portable as the C file. The hashes below are what the Windows side must
# load: same bytes in, or the comparison proves nothing.
for f in "$SRC"/shaders/*.comp; do
	base=$(basename "$f" .comp)
	glslangValidator -V "$f" -o "$OUT/shaders/$base.spv"
done
sha256sum "$OUT"/shaders/*.spv | sed 's#'"$OUT"'/shaders/##'

echo "== vkcompute -> $OUT/vkcompute"
gcc -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter \
	-o "$OUT/vkcompute" "$SRC/vkcompute.c" -lvulkan -lm
ls -l "$OUT/vkcompute"
sha256sum "$OUT/vkcompute"

echo "== done"
