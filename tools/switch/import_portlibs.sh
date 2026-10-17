#!/usr/bin/env bash
# Copies the Mesa-for-Horizon portlibs (NaGaa95/mesa-switch: loaderless Vulkan/NVK,
# EGL, OpenGL/GLES) from the Windows devkitPro install into local/portlibs-switch.
# tools/switch/build.sh mounts that folder over the Docker image's portlibs.
# Usage: tools/switch/import_portlibs.sh [/mnt/c/devkitPro/portlibs/switch]
set -euo pipefail
cd "$(dirname "$0")/../.."
SOURCE="${1:-/mnt/c/devkitPro/portlibs/switch}"
DEST=local/portlibs-switch
[ -d "$SOURCE" ] || { echo "Not found: $SOURCE (pass the portlibs/switch path)" >&2; exit 1; }
for lib in libvulkan.a libEGL.a libGLESv2.a; do
  [ -f "$SOURCE/lib/$lib" ] || { echo "Missing $SOURCE/lib/$lib: is mesa-switch installed there?" >&2; exit 1; }
done
rm -rf "$DEST"; mkdir -p "$DEST"
# Base: the Docker image's own portlibs (build.sh mounts DEST over them), then
# the Mesa SDK on top.
DOCKER=docker; $DOCKER info >/dev/null 2>&1 || DOCKER="sudo docker"
$DOCKER run --rm -u "$(id -u):$(id -g)" -v "$PWD/$DEST":/out devkitpro/devkita64 \
  cp -a /opt/devkitpro/portlibs/switch/. /out/
cp -a "$SOURCE/." "$DEST/"
# OpenGLConfig.cmake requires libelf whenever libvulkan.a is present, but the
# NVK/Zink link references no libelf symbol (verified: links with an empty
# archive). Provide an empty one when the SDK ships without it.
if [ ! -f "$DEST/lib/libelf.a" ]; then
  $DOCKER run --rm -u "$(id -u):$(id -g)" -v "$PWD/$DEST":/p devkitpro/devkita64 \
    /opt/devkitpro/devkitA64/bin/aarch64-none-elf-ar rcs /p/lib/libelf.a
  echo "libelf.a: not in the SDK, created an empty placeholder archive"
fi
echo "Imported $(find "$DEST/lib" -name '*.a' | wc -l) static libraries into $DEST"
ls "$DEST/lib/cmake" 2>/dev/null || true
for f in OpenGL/OpenGLConfig.cmake Vulkan/VulkanConfig.cmake; do
  [ -f "$DEST/lib/cmake/$f" ] && { echo "== $f"; cat "$DEST/lib/cmake/$f"; }
done
