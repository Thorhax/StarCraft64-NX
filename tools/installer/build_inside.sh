#!/usr/bin/env bash
# Runs inside the installer image (tools/installer/Dockerfile). Builds the Switch NRO
# from the user's ROM, extracts the user's classic StarCraft PC files and writes a
# ready-to-copy SD card folder:
#   /in/rom          the user's StarCraft 64 (USA) dump (z64, v64 or n64 byte order)
#   /in/starcraft    optional: the classic StarCraft / Brood War 1.16 install
#                    (StarDat.mpq, BrooDat.mpq, CampnSC.dat, CampnBW.dat)
#   /out             receives switch/StarCraft64/ (and build logs in logs/)
# Options (environment): SC64_WIDESCREEN=1 (default) adds the widescreen option file;
# SC64_JOBS (default: all cores).
set -euo pipefail

ROM_IN=/in/rom
PC=/in/starcraft
OUT=/out
SD="$OUT/switch/StarCraft64"
LOGS="$OUT/logs"
JOBS="${SC64_JOBS:-$(nproc)}"
SRC=/work/src
ROM="$SRC/StarCraft 64 (USA).z64"
ROM_SHA1=a36d91b5aac7bb95df0d7de5f9814f5270e25c2b

step() { printf '\n==> %s\n' "$*"; }
fail() { printf '\nERROR: %s\n' "$*" >&2; exit 1; }
# Runs a long step quietly; its output goes to logs/<name>.log and is shown on failure.
quiet() {
    local name="$1"; shift
    if ! "$@" >"$LOGS/$name.log" 2>&1; then
        tail -40 "$LOGS/$name.log" >&2
        fail "$name failed (full log: logs/$name.log)"
    fi
}

[ -f "$ROM_IN" ] || fail "No ROM: mount your StarCraft 64 (USA) dump at /in/rom"
mkdir -p "$SD" "$LOGS"

step "Preparing the sources"
rm -rf "$SRC"; cp -a /opt/src-clean "$SRC"
ln -s /opt/third_party "$SRC/third_party"
ln -s /opt/venv "$SRC/.venv"
cd "$SRC"

step "Checking your ROM"
python3 - "$ROM_IN" "$ROM" <<'PY'
import sys
data = bytearray(open(sys.argv[1], "rb").read())
magic = bytes(data[:4])
if magic == b"\x37\x80\x40\x12":      # v64: byte-swapped halfwords
    data[0::2], data[1::2] = data[1::2], data[0::2]
elif magic == b"\x40\x12\x37\x80":    # n64: little-endian words
    for i in range(0, len(data) - 3, 4):
        data[i:i + 4] = data[i:i + 4][::-1]
elif magic != b"\x80\x37\x12\x40":
    sys.exit("Not an N64 ROM (unknown header)")
open(sys.argv[2], "wb").write(data)
PY
[ "$(sha1sum "$ROM" | cut -d' ' -f1)" = "$ROM_SHA1" ] \
    || fail "This is not the StarCraft 64 (USA) ROM this port supports (SHA-1 $ROM_SHA1 after byte-order normalisation)"
echo "StarCraft 64 (USA), verified"

step "libultra inventory (a few minutes)"
mkdir -p local/reports/libultra local/captures/synth
quiet n64sym third_party/n64sym/bin/n64sym "$ROM" -s -f splat -o local/reports/libultra/n64sym-rom.splat.txt
python3 - "$ROM" <<'PY'
import sys
rom = open(sys.argv[1], "rb").read()
ram = bytearray(8 << 20); seg = rom[0x1000:0xD6640]; ram[0x400:0x400 + len(seg)] = seg
open("local/captures/synth/rdram.bin", "wb").write(ram)
PY
quiet inventory .venv/bin/python tools/research_libultra/inventory.py "$ROM" \
    --n64sym third_party/n64sym --recompiler third_party/N64Recomp --runtime third_party/N64ModernRuntime \
    --reference-root third_party/ultralib/build --candidates local/reports/libultra/n64sym-rom.splat.txt \
    --candidate-displacement 0x7FFFF400 --scan-start 0x1000 --scan-end 0xD6640 --displacement 0x7FFFF400 \
    --dump local/captures/synth/rdram.bin --output local/reports/libultra-resident

step "Recompiling the game (10-30 minutes)"
quiet recompile make recompile ROM="$ROM" JOBS="$JOBS"

step "Preparing the runtime"
quiet runtime sh -c '
    python3 tools/recompile/patch_runtime.py third_party/N64ModernRuntime local/deps/N64ModernRuntime &&
    python3 tools/recompile/patch_host_runtime.py local/deps/N64ModernRuntime &&
    python3 -c "import sys; sys.path.insert(0, \"tools/desktop\"); import patch_shell_runtime as p; from pathlib import Path; p.prepare(Path(\"local/deps/N64ModernRuntime\"), Path(\"local/deps/N64ModernRuntime-shell\"))" &&
    python3 tools/switch/prepare_runtime.py'

step "Icon"
if make icon ROM="$ROM" PYTHON=.venv/bin/python >"$LOGS/icon.log" 2>&1 && [ -f local/icons/icon-1024.png ]; then
    .venv/bin/python -c "from PIL import Image; Image.open('local/icons/icon-1024.png').convert('RGB').resize((256, 256), Image.LANCZOS).save('local/icons/switch-icon.jpg', quality=95)"
    echo "from your ROM's title screen"
else
    echo "skipped (see logs/icon.log); the NRO gets the default icon"
fi

step "Building the Switch NRO (10-20 minutes)"
quiet nro sh -c "cmake -S native/switch -B local/build/switch -G 'Unix Makefiles' \
    -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake -DCMAKE_BUILD_TYPE=Release &&
    cmake --build local/build/switch -j$JOBS"
[ -f local/build/switch/StarCraft64.nro ] || fail "The build produced no NRO (logs/nro.log)"
grep -q "Mesa EGL/OpenGL enabled" "$LOGS/nro.log" || echo "warning: built without the GPU renderer (Mesa not found)"
cp local/build/switch/StarCraft64.nro "$SD/StarCraft64.nro"
cp "$ROM" "$SD/rom.z64"

if [ -d "$PC" ] && [ -n "$(ls -A "$PC" 2>/dev/null)" ]; then
    missing=""
    for f in StarDat.mpq BrooDat.mpq CampnSC.dat CampnBW.dat; do
        [ -f "$PC/$f" ] || missing="$missing $f"
    done
    [ -z "$missing" ] || echo "note: not in your StarCraft folder:$missing (the parts that need them are skipped)"

    if [ -f "$PC/StarDat.mpq" ] && [ -f "$PC/BrooDat.mpq" ]; then
        step "HD graphics and portraits"
        quiet hd .venv/bin/python -I tools/hd/extract_graphics.py "$PC" "$SD/hd"
    fi
    if [ -f "$PC/CampnSC.dat" ] && [ -f "$PC/CampnBW.dat" ]; then
        step "PC soundtrack"
        quiet music .venv/bin/python -I tools/music/extract_music.py "$PC" "$SD/music"
        step "PC cinematics"
        quiet movies .venv/bin/python -I tools/voices/extract_movies.py "$PC" "$SD/movies"
        step "PC voices (briefings and in-mission)"
        quiet campaign .venv/bin/python -I tools/voices/extract_campaign.py "$PC" local/campaign
        quiet briefings .venv/bin/python -I tools/voices/extract_briefings.py --source local/campaign --out "$SD/voices"
        quiet bolt .venv/bin/python -I tools/bolt_extract.py "$ROM" local/extracted/sc64-usa
        quiet ingame .venv/bin/python -I tools/voices/extract_ingame_voices.py local/extracted/sc64-usa \
            local/campaign/enUS/campaign "$SD/voices"
    fi
else
    echo
    echo "No StarCraft folder given: the NRO plays with the N64 graphics, music and silent briefings."
fi

[ "${SC64_WIDESCREEN:-1}" = 1 ] && : > "$SD/widescreen"

step "Done"
echo "Copy the 'switch' folder from your output folder to the root of the Switch SD card."
for d in hd music movies voices; do
    [ -d "$SD/$d" ] && printf '  %-7s %s files\n' "$d" "$(find "$SD/$d" -type f | wc -l)"
done
echo "  NRO     $(du -h "$SD/StarCraft64.nro" | cut -f1)"
echo "Keep this folder private: it contains your ROM and files from your StarCraft install."
