# StarCraft 64 — Nintendo Switch port (v1.4)

Native **Nintendo Switch (Horizon / Atmosphère)** port of StarCraft 64, with
widescreen missions, the PC cinematics and the PC voice acting.

This project continues **[gabryboy12-ai's StarCraft64-NX](https://github.com/gabryboy12-ai/StarCraft64-NX)**,
the original Switch port (v0.5–v1.3: libnx shell, GPU renderer, asynchronous
graphics, voiced briefings), which is itself built on
**[n0madic's StarCraft 64 static recompilation](https://github.com/n0madic/StarCraft64-recomp)**.
Their full commit history is kept in this repository. Thank you both.

> **No game files are included.** This repository contains source code only.
> You need your own legally obtained StarCraft 64 (USA) cartridge dump
> (`.z64`, SHA-1 `a36d91b5aac7bb95df0d7de5f9814f5270e25c2b`). The ROM, the
> recompiled game code and every game-derived file (icons, movies and voices
> included) are generated on your machine and are never committed or distributed.

---

## What's new in v1.4

- **Widescreen missions** (optional): 416×240 instead of 320×240, shown at
  exactly 3× (1248×720). About 30% more of the map side to side; fog of war,
  camera, cursor and touch cover the full width. The HUD follows the
  wider screen: counters and command hints at the right edge, Advisor and
  transmission boxes, pause menu and objectives centred. Menus and briefings
  stay 4:3. Turn it on with an empty `widescreen` file (see
  [Option files](#option-files)).
- **PC cinematics**: the Blizzard logo and the StarCraft intro replace the N64
  opening; each campaign's cinematics play before its briefings and after the
  victories that unlock them, once per session. A, B, + or a tap skips.
  Taken from your own classic StarCraft CD install (section 9).
- **Voiced in-mission transmissions**: 508 transmissions in 54 missions play
  the PC voice acting, matched automatically by their text (section 8).
- **All cheats unlocked** on the Cheats page from the start.
- **Louder game audio** (×4 with a soft limiter), so music and effects sit
  with the voices, movies and other Switch software; adjustable.
- **Much faster saving and loading**: FlashRAM writes are collected and
  written to the SD card once.
- **Faster graphics thread**: no more 8 MiB copies per frame.
- Option files also work when Windows saves them with a `.txt` extension.

## Status

Fully playable, with a GPU renderer and the work spread over the CPU cores.
Measured on a Switch launched as a Sphaira 4-core forwarder:

| Screen | Resolution | v0.5 (CPU renderer) | v1.2 (GPU renderer) |
|---|---|---|---|
| Missions | 320×240 | ~22 fps | ~27 fps (the original game caps at 30) |
| Main menu | 640×480 | ~22 fps | ~26 fps |
| Briefings | 640×480 | ~13 fps | ~17 fps |
| Intro | 640×480 | ~50 fps | ~50 fps |

- **GPU renderer**: Mesa for Horizon (nouveau, OpenGL 4.3). The fragment shader
  reproduces the software rasterizer's integer rules and was validated
  byte-for-byte against it (thousands of frames, 0 differing pixels). Frames it
  does not model fall back to the software renderer automatically.
- **Asynchronous graphics**, as on the real RCP: the game thread only snapshots
  RDRAM and keeps running; display-list interpretation, GPU rendering and the
  RDP stores run on other cores before the RDP completion is signalled.
- **Low clocks**: playable down to 612 MHz CPU / 152 MHz GPU (lower frame
  rate); 920 MHz CPU / 192 MHz GPU is a stable battery-saving setting.
- **Voiced briefings** (v1.3): the original N64 briefings are text-only;
  the port plays the PC voice acting from your own (free) StarCraft PC files,
  in English, Italian, Spanish or Portuguese. Missions and transmissions are
  matched automatically from the briefing text.
- Brood War loading screens: RDP FILL mode (`SETFILLCOLOR`/`FILLRECTANGLE`)
- Native audio (libnx `audout`), FlashRAM saves on the SD card
- Joy-Con / Pro Controller and handheld touch screen (tap to point, drag to
  select, two-finger pan, instant touch cursor)

| Core | Work |
|---|---|
| 0 | presentation, VI/timer, graphics thread (display lists, RDP stores) |
| 1 | the game (StarCraft 64 is single-threaded) and the RDRAM snapshot |
| 2 | OpenGL thread (texture planning, uploads, readback) |
| 3 | system; raster helper when a frame falls back to software |

**Next:** HD graphics from the PC version (the N64 sprites are half-size
copies of the PC ones), translated briefing and interface text.

### Controls

| Switch | N64 / game |
|---|---|
| A / B | A / B |
| + | Start |
| L / R | L / R |
| ZL / ZR | Z |
| D-pad | D-pad |
| Left stick | Analog stick |
| Right stick | C buttons |
| − | Command panel |

---

## Building the Switch NRO

Tested on Windows 11 + WSL2 (Ubuntu). The Switch toolchain runs in the official
`devkitpro/devkita64` Docker image.

### 1. Base environment (WSL Ubuntu)

```bash
sudo apt install -y build-essential clang lld cmake ninja-build git unzip curl wget \
  python3 python3-venv python3-pip openjdk-21-jdk binutils-mips-linux-gnu docker.io
sudo usermod -aG docker $USER   # then: wsl --shutdown, reopen Ubuntu
docker pull devkitpro/devkita64
git clone https://github.com/Thorhax/StarCraft64-NX.git
cd StarCraft64-NX
python3 -m venv .venv
.venv/bin/pip install spimdisasm==1.42.4 pyelftools numpy pillow
```

### 2. Pinned dependencies

```bash
mkdir -p third_party && cd third_party
pin() { git clone -q "$1" "$2" && git -C "$2" checkout -q "$3"; }
pin https://github.com/N64Recomp/N64Recomp        N64Recomp        ffb39cdad1da5de07eaaa48bd1db4a89a7986771
pin https://github.com/N64Recomp/N64ModernRuntime N64ModernRuntime cdf5abbd5026fef5c364c676e4667c45e42b6863
pin https://github.com/shygoo/n64sym              n64sym           ccf4600f3389f1a84bde23339225cf372fdf7712
pin https://github.com/decompals/ultralib         ultralib         e24c836796df4bf520ff8b11a5c9d2cea3a66cbd
git -C N64Recomp submodule update --init --recursive
git -C N64ModernRuntime submodule update --init --recursive
cd ..
```

### 3. libultra inventory (needs your ROM)

```bash
ROM="StarCraft 64 (USA).z64"     # your own dump, placed in the repo root
make -C third_party/n64sym -j$(nproc)
mkdir -p local/reports/libultra
third_party/n64sym/bin/n64sym "$ROM" -s -f splat -o local/reports/libultra/n64sym-rom.splat.txt
(cd third_party/ultralib && make VERSION=J TARGET=libgultra_rom COMPARE=0 setup &&
 for v in I J K L; do
   make VERSION=$v TARGET=libgultra_rom COMPARE=0 MDEBUG_FILES= AR="$PWD/tools/gcc/ar" -j$(nproc)
   make VERSION=$v TARGET=libultra_rom COMPARE=0 -j$(nproc)
 done)
# The resident code is a byte copy of ROM 0x1000.. at RAM 0x400; the script verifies it.
python3 - <<'PY'
import os
rom = open("StarCraft 64 (USA).z64", "rb").read()
ram = bytearray(8 << 20); seg = rom[0x1000:0xD6640]; ram[0x400:0x400 + len(seg)] = seg
os.makedirs("local/captures/synth", exist_ok=True)
open("local/captures/synth/rdram.bin", "wb").write(ram)
PY
.venv/bin/python tools/research_libultra/inventory.py "$ROM" \
  --n64sym third_party/n64sym --recompiler third_party/N64Recomp --runtime third_party/N64ModernRuntime \
  --reference-root third_party/ultralib/build --candidates local/reports/libultra/n64sym-rom.splat.txt \
  --candidate-displacement 0x7FFFF400 --scan-start 0x1000 --scan-end 0xD6640 --displacement 0x7FFFF400 \
  --dump local/captures/synth/rdram.bin --output local/reports/libultra-resident
```

### 4. Mesa for Horizon (GPU renderer)

Install NaGaa95's Mesa SDK for Switch (EGL, OpenGL, Vulkan/NVK) in your
Windows devkitPro, then import it into the build (it is merged with the
Docker image's portlibs and mounted at build time):

```bash
./tools/switch/import_portlibs.sh /mnt/c/devkitPro/portlibs/switch
```

Without it the NRO still builds and runs with the software renderer.

### 5. Recompile, prepare the runtime, build

```bash
make recompile ROM="$ROM"      # also adds the widescreen switch (tools/recompile/apply_widescreen.py)
python3 tools/recompile/patch_runtime.py third_party/N64ModernRuntime local/deps/N64ModernRuntime
python3 tools/recompile/patch_host_runtime.py local/deps/N64ModernRuntime
python3 -c "import sys; sys.path.insert(0,'tools/desktop'); import patch_shell_runtime as p; from pathlib import Path; p.prepare(Path('local/deps/N64ModernRuntime'), Path('local/deps/N64ModernRuntime-shell'))"
python3 tools/desktop/bootstrap.py
make icon ROM="$ROM" PYTHON=.venv/bin/python   # optional: NRO/forwarder icon from the title art
./tools/switch/build.sh                         # -> local/build/switch/StarCraft64.nro
```

### 6. Install on the SD card

1. Create the folder `switch/StarCraft64/` on the SD card.
2. Copy `local/build/switch/StarCraft64.nro` into it.
3. Copy your ROM into the same folder and rename it to **`rom.z64`**.

Never launch it from the album (applet mode has too little memory and CPU).
Use one of the two methods below. Logs (`sc64.log`) and, after a crash, a
`crash.txt` report are written to `switch/StarCraft64/`.

### 7a. Sphaira forwarder (recommended: icon on the HOME menu, 4 cores)

A forwarder installs a HOME-menu icon that launches the NRO as a full
application. Sphaira forwarders can also grant the 4th CPU core, which this
port uses for an extra raster thread (~10-20% faster menus).

1. Install [Sphaira](https://github.com/ITotalJustice/sphaira) and open it.
2. In its homebrew list select **StarCraft 64** (the icon is the NRO's own).
3. Open the entry's options and choose **Install Forwarder**.
4. In the forwarder settings enable **4 cores** (and the 39-bit address space
   if offered), then confirm.
5. Start the game from the new HOME-menu icon. `sc64.log` should report
   `raster helper 1 on core 3`.

After updating the NRO (for example to get the icon), uninstall the old
forwarder and create it again: a forwarder keeps the icon it was made with.

### 7b. hbmenu title takeover

Hold **R** while opening any installed game, then pick StarCraft 64 in hbmenu.
This uses 3 cores: fully playable, slightly slower menus.

### 8. Voiced briefings (optional)

StarCraft 64's briefings have no voice acting. The port can play the PC
version's, taken from **your own StarCraft PC install** (free from Blizzard);
no audio is distributed with this project.

1. **Install StarCraft** with the Battle.net app. In *Game Settings*, set both
   the **text language** and the **spoken language** you want; each language
   is downloaded only when selected (English is always needed, see below).
2. **Extract the campaign maps** with [CascView](http://www.zezula.net/en/casc/main.html):
   open the StarCraft folder (the one with `.build.info`) and extract, for each
   language, its `campaign` folder into `<folder>/<code>/campaign`:

   | Language | CascView path | Destination |
   |---|---|---|
   | English | `SD/campaign` | `CascView/enUS/campaign` |
   | Italian | `locales/itIT/Assets/SD/campaign` | `CascView/itIT/campaign` |
   | Spanish | `locales/esES/Assets/SD/campaign` | `CascView/esES/campaign` |
   | Portuguese | `locales/ptBR/Assets/SD/campaign` | `CascView/ptBR/campaign` |

   Tick *Local Files Only* in CascView to check that a language is really
   downloaded before extracting it.
3. **Convert** (WSL; reads each map's briefing script and converts the voices
   to 24 kHz, about 220 MB per language):

   ```bash
   sudo apt install -y ffmpeg
   python3 tools/voices/extract_briefings.py --source /mnt/c/Users/<you>/Desktop/CascView --out local/voices
   ```

   It prints one line per language, e.g.
   `itIT: 77 briefings, 437 voice files, 0 missing`.
4. **Copy** `local/voices` to the SD card as `switch/StarCraft64/voices`.
   The `enUS` folder is **always required**: briefings are recognised by their
   English text, whatever language you listen to.
5. **Choose the language**: create the text file
   `switch/StarCraft64/voices/language` containing one code — `enUS`
   (default when the file is missing), `itIT`, `esES` or `ptBR`. The language
   is read at start-up: restart the game after changing it.

`sc64.log` shows `voices: 65 campaign briefings, language itIT` at start-up and
one `voices: briefing matched <mission> ...` line per briefing. While a voice
plays, the music is lowered.

**In-mission transmissions** (English) come from the same `enUS/campaign`
folder, paired with the N64 maps extracted from your ROM:

```bash
python3 tools/bolt_extract.py "$ROM" local/extracted/sc64-usa
python3 tools/voices/extract_ingame_voices.py local/extracted/sc64-usa \
  /mnt/c/Users/<you>/Desktop/CascView/enUS/campaign local/voices
```

It writes `local/voices/enUS/ingame.json` and the voice files next to the
briefing ones; copy `local/voices` to the SD card again. `sc64.log` then shows
`voices: 508 in-mission transmissions`.

If you only have the classic CD version (1.16), `tools/voices/extract_campaign.py`
extracts the same campaign layout from its `CampnSC.dat` / `CampnBW.dat`
(needs StormLib, see below).

### 9. PC cinematics (optional)

The movies come from the **classic StarCraft / Brood War CD install** (the
Remastered edition does not include them): the folder with `StarCraft.exe`,
`BroodWar.exe`, `CampnSC.dat` and `CampnBW.dat`. The extractor uses
[StormLib](https://github.com/ladislav-zezula/StormLib) to read those archives:

```bash
git clone https://github.com/ladislav-zezula/StormLib && cmake -S StormLib -B StormLib/build -DBUILD_SHARED_LIBS=ON && cmake --build StormLib/build -j
STORMLIB=$PWD/StormLib/build/libstorm.so python3 tools/voices/extract_movies.py /mnt/c/path/to/StarCraft local/movies
```

Copy `local/movies` (28 `.smk` files, about 580 MB) to the SD card as
`switch/StarCraft64/movies`. `sc64.log` shows `movie: 28 movies installed, enabled`.

### Option files

Empty files in `switch/StarCraft64/` change the port's behaviour without
rebuilding. A `.txt` extension is fine (`widescreen.txt` works like `widescreen`).

| File | Effect |
|---|---|
| `widescreen` | widescreen missions (416×240); `sc64.log` shows `widescreen: 416x240 missions` |
| `nomovies` | no PC cinematics, even when installed |
| `novoices` | no voice playback, even when the voice files are installed |
| `lockcheats` | keep the original cheat unlocks |
| `volume` | not empty: one number, the game volume factor (default `4`; `1` = original level) |
| `softraster` | CPU renderer (v0.5 behaviour), GPU unused |
| `nogl` | no Mesa at all |
| `glcheck` | renders every frame on GPU and CPU, logs any difference (slow; for testing) |

## Android APK from Linux/WSL

Upstream targets macOS; on Linux the Android build only needs Linux paths
(already applied in `tools/android/build.py`) and an Android SDK with NDK r29
and Build-Tools 36. See [docs/android.md](docs/android.md) and
[docs/upstream-README.md](docs/upstream-README.md).

## What changed for Switch

- `native/switch/` — libnx shell (framebuffer video, HID pads + touch, crash
  handler, 2 MiB thread stacks), SD platform layer, CMake profile, and the GPU
  renderer: `gl_raster.*` (DrawList rasterizer: per-axis texture addressing
  tables on the CPU, integer fragment shader, linear SSBO framebuffer,
  byte-budgeted texture cache) and `gl_device.*` (one surfaceless GL thread)
- `native/shell/session.*` — asynchronous graphics submission (Switch) with
  the original synchronous path kept for Android/desktop, GPU hook and the
  `glcheck` validation mode
- `native/shell/voices.*` — voiced briefings: reads the N64 briefing script
  and page from RDRAM, matches missions/transmissions against the PC text,
  streams the voice from the SD card and mixes it over the ducked game audio;
  `tools/voices/extract_briefings.py` converts the user's PC campaign maps
- `native/shell/voices.*` (v1.4) — in-mission transmissions: the open message
  box's text is matched against `ingame.json`
- `native/shell/movie.*` and `native/switch/app.cpp` (v1.4) — FFmpeg movie
  player paced by the wall clock, and the cinematic schedule
- `native/runtime/widescreen.hpp`, `tools/recompile/apply_widescreen.py` (v1.4)
  — runtime widescreen switch for the recompiled 1P code; dialogs are moved
  when they open (`sc64_ws_dialog` in `ui_feedback.cpp`)
- `native/runtime/flash_device.*` (v1.4) — deferred FlashRAM writes
- `native/gfx_core/` — RDP FILL mode (Brood War loading screens)
- `tools/switch/` — Docker build, Mesa portlibs import, runtime derivation with
  asserted `__SWITCH__` patches (semaphores, memory, thread cores/priorities,
  early RSP completion, no code mods), `addr2line` helper
- Shared fixes that also help other platforms: word-swapped RDRAM snapshot,
  cross-frame S2DEX background cache, FlashRAM saves on FAT, touch cursor warp
  and faster two-finger camera pan

## Credits and license

- [n0madic/StarCraft64-recomp](https://github.com/n0madic/StarCraft64-recomp) —
  the recompilation, runtime integration and renderer this port builds on
- [N64Recomp / N64ModernRuntime](https://github.com/N64Recomp), devkitPro /
  libnx, decompals ultralib, shygoo n64sym, miniaudio, SDL
- [NaGaa95/mesa-switch](https://github.com/NaGaa95/mesa-switch) — Mesa for
  Horizon (nouveau OpenGL, NVK) that the GPU renderer runs on
- [gabryboy12-ai/StarCraft64-NX](https://github.com/gabryboy12-ai/StarCraft64-NX) —
  **gabryboy12-ai**, creator of the original Switch port (v0.5–v1.3) this
  project continues: libnx shell, GPU renderer, asynchronous graphics,
  voiced briefings
- v1.4 (widescreen, cinematics, in-mission voices and the rest): Thorhax
- [StormLib](https://github.com/ladislav-zezula/StormLib) (movie extraction),
  [FFmpeg](https://ffmpeg.org) (movie playback)

Source code: MIT (see [LICENSE](LICENSE)), except files noted there.
StarCraft is a trademark of Blizzard Entertainment; StarCraft 64 and its
assets belong to their respective owners. This project is not affiliated with
or endorsed by Blizzard Entertainment or Nintendo.
