#!/usr/bin/env bash
# Usage: tools/switch/addr2line.sh 0x1234 [0x5678 ...]  (elf offsets from crash.txt)
set -euo pipefail
cd "$(dirname "$0")/../.."
DOCKER=docker; $DOCKER info >/dev/null 2>&1 || DOCKER="sudo docker"
$DOCKER run --rm -v "$PWD":/src -w /src devkitpro/devkita64 \
  /opt/devkitpro/devkitA64/bin/aarch64-none-elf-addr2line -f -C -i -p -e local/build/switch/StarCraft64.elf "$@"
