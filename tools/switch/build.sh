#!/usr/bin/env bash
# Switch build inside the official devkitPro Docker image. Logs: local/build/switch/build.log
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p local/build/switch
python3 tools/switch/prepare_runtime.py
if [ -f local/icons/icon-1024.png ] && [ -x .venv/bin/python ]; then
  .venv/bin/python -c "from PIL import Image; Image.open('local/icons/icon-1024.png').convert('RGB').resize((256, 256), Image.LANCZOS).save('local/icons/switch-icon.jpg', quality=95)"
  echo "Switch icon: local/icons/switch-icon.jpg"
fi
DOCKER=docker; $DOCKER info >/dev/null 2>&1 || DOCKER="sudo docker"
# Mesa for Horizon (EGL/GL/Vulkan, imported by tools/switch/import_portlibs.sh).
PORTLIBS=()
[ -d local/portlibs-switch/lib ] && PORTLIBS=(-v "$PWD/local/portlibs-switch":/opt/devkitpro/portlibs/switch:ro)
$DOCKER run --rm -u "$(id -u):$(id -g)" -v "$PWD":/src "${PORTLIBS[@]}" -w /src devkitpro/devkita64 bash -c '
  export PATH=/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:$PATH
  cmake -S native/switch -B local/build/switch -G "Unix Makefiles" \
    -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake -DCMAKE_BUILD_TYPE=Release
  cmake --build local/build/switch -j"$(nproc)" -- -k
' 2>&1 | tee local/build/switch/build.log
