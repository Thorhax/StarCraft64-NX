#!/usr/bin/env bash
# Switch build inside the official devkitPro Docker image. Logs: local/build/switch/build.log
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p local/build/switch
python3 tools/switch/prepare_runtime.py
DOCKER=docker; $DOCKER info >/dev/null 2>&1 || DOCKER="sudo docker"
$DOCKER run --rm -u "$(id -u):$(id -g)" -v "$PWD":/src -w /src devkitpro/devkita64 bash -c '
  export PATH=/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:$PATH
  cmake -S native/switch -B local/build/switch -G "Unix Makefiles" \
    -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake -DCMAKE_BUILD_TYPE=Release
  cmake --build local/build/switch -j"$(nproc)" -- -k
' 2>&1 | tee local/build/switch/build.log
