#!/usr/bin/env bash
# StarCraft 64 NX installer (Linux, macOS, Windows WSL): builds the Switch NRO from your
# own ROM and extracts your own classic StarCraft PC files, all inside Docker on this
# computer. See docs/installer.md.
#
#   tools/installer/install.sh --rom "StarCraft 64 (USA).z64" [--starcraft /path/to/StarCraft]
#                              [--out sd-card] [--no-widescreen]
set -euo pipefail
cd "$(dirname "$0")/../.."

ROM="" PC="" OUT="sd-card" WIDESCREEN=1
while [ $# -gt 0 ]; do
    case "$1" in
        --rom) ROM="$2"; shift 2 ;;
        --starcraft) PC="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --no-widescreen) WIDESCREEN=0; shift ;;
        -h|--help) sed -n '2,8p' "$0"; exit 0 ;;
        *) echo "Unknown option: $1 (see --help)" >&2; exit 2 ;;
    esac
done
[ -n "$ROM" ] || { echo "Give your ROM with --rom (see --help)" >&2; exit 2; }
[ -f "$ROM" ] || { echo "ROM not found: $ROM" >&2; exit 2; }
[ -z "$PC" ] || [ -d "$PC" ] || { echo "StarCraft folder not found: $PC" >&2; exit 2; }
command -v docker >/dev/null || { echo "Docker is required: https://docs.docker.com/get-docker/" >&2; exit 2; }
docker info >/dev/null 2>&1 || { echo "Docker is installed but not running (start Docker Desktop, or the docker service)" >&2; exit 2; }

mkdir -p "$OUT"
abs() { (cd "$(dirname "$1")" && printf '%s/%s' "$PWD" "$(basename "$1")"); }

echo "Building the installer image (the first time takes 30-60 minutes; later runs reuse it)..."
docker build -f tools/installer/Dockerfile -t sc64-installer . >"$OUT/image-build.log" 2>&1 \
    || { tail -30 "$OUT/image-build.log" >&2; echo "Image build failed (full log: $OUT/image-build.log)" >&2; exit 1; }

MOUNTS=(-v "$(abs "$ROM"):/in/rom:ro" -v "$(abs "$OUT"):/out")
[ -z "$PC" ] || MOUNTS+=(-v "$(abs "$PC"):/in/starcraft:ro")
USER_ARGS=()
[ "$(uname -s)" = Linux ] && USER_ARGS=(-u "$(id -u):$(id -g)") # output owned by you, not root
docker run --rm "${USER_ARGS[@]}" "${MOUNTS[@]}" -e SC64_WIDESCREEN="$WIDESCREEN" sc64-installer
