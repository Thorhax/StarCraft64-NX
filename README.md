# StarCraft 64 — Nintendo Switch port (v0.5)

Native **Nintendo Switch (Horizon / Atmosphère)** port of the
[StarCraft 64 static recompilation](https://github.com/n0madic/StarCraft64-recomp)
by n0madic, plus Linux/WSL build support for its Android APK.

> **No game files are included.** This repository contains source code only.
> You need your own legally obtained StarCraft 64 (USA) cartridge dump
> (`.z64`, SHA-1 `a36d91b5aac7bb95df0d7de5f9814f5270e25c2b`). The ROM, the
> recompiled game code and every game-derived file (icons included) are
> generated on your machine and are never committed or distributed.

---

## A message from the author

Hello everyone, let me introduce this port, the first one I have brought to
completion myself.

Even if quietly, my work has contributed to a lot of what you see around:
I laid the foundations for the Call of Duty: Black Ops port on Switch that
Mr. Erik is working on; I got GTA IV running on Switch for the first time, and
the port is now being completed by a colleague, DevZ; I translated the CafeOS
libraries to HOS with **coreinit-nx**, which has been and will be used for
Wii U to Switch ports; I am contributing to the **PS3recomp** project, working
on Linux aarch64, to make future PS3 game ports to Switch possible — first on
Linux and later, why not, on HOS too (you can find everything on my GitHub).

Now this port, and in the future many other projects I have in mind that only
time and resources keep me from reaching.

All of this, as the previous sentence suggests, unfortunately has a cost:

- The AI tool I use is paid monthly and lets these projects be completed in a
  reasonable amount of time;
- All these projects take time: a large part of my free time goes into them;
- I use my hardware every day for a lot of testing, some of it stressful, and
  sooner or later it will inevitably need maintenance.

I am very happy to work for the community, practically for free; however, I
have decided that at this point it is fair for me to be rewarded not only with
pride but also with a few cents. Together with this port I am therefore
announcing voluntary donations through **Buy Me a Coffee**: this way you can
show me your closeness and support in a tangible way, and help me cover the
costs and keep contributing.

Above a certain threshold, top supporters will get early access to my projects
and become official testers, with preview access to each project.

I also accept requests for private projects, for amounts to be agreed, after a
private consultation with me to assess feasibility, time and therefore any
costs.

Thank you for your understanding and your continued support.

☕ **Buy Me a Coffee:** _link coming soon_

<details>
<summary>Messaggio originale (italiano)</summary>

Salve a tutti, vi presento questo Port, il primo portato a termine dal sottoscritto.
Tuttavia anche se in silenzio, il mio lavoro ha contribuito a tanto di quello che vedete in giro:
Ho posto le basi per il Port di call of duty black ops sulla switch su cui sta lavorando il signor Erik, ho fatto partire per la prima volta GTA4 sulla switch, adesso il Port sta venendo completato da un collega: DevZ; Ho tradotto le librerie di cafeOS su HOS con coreinit-nx, che è stato utilizzato e sarà utilizzato per i Port dalla WiiU alla Switch; Sto contribuendo al progetto PS3recomp, lavorando su Linux aarch64, per permettere in futuro i Port dei giochi PS3 sulla switch, in un primo momento su Linux e successivamente perché no anche su HOS (Trovate tutto sul mio GitHub).
Adesso questo port, e in futuro tanti altri progetti che ho in mente e che solo il tempo e le risorse mi bloccano dal raggiungere.
Tutto questo purtroppo come anticipato nella frase precedente ha un costo:
- Lo strumento AI che utilizzo viene pagato mensilmente e permette che questi progetti vengano portati a termine in tempi ragionevoli;
- Tutti questi progetti richiedono tempo, una grande parte del mio tempo libero viene dedicata a questo.
- Utilizzo l'hardware tutti i giorni con molti test, anche stressanti, questo prima o poi inevitabilmente richiederà della manutenzione.

Io sono molto felice di lavorare per la community, praticamente gratis, tuttavia ho deciso che è giusto, a questo punto, che io venga retribuito, non solo con l'orgoglio, ma anche con qualche centesimo, infatti insieme a questo port annuncio l'apertura delle donazioni volontarie tramite buymeacoffee, in questo modo potete farmi sentire la vostra vicinanza e il vostro supporto anche in maniera tangibile e permettermi di mantenere i costi e continuare con il mio contributo.
A partire da una certa soglia, i migliori donatori avranno accesso anticipato ai miei progetti e diventeranno tester ufficiali avendo accesso al progetto in anteprima.
Inoltre per cifre da concordare accetto richieste di progetti privati, previa consultazione privata con il sottoscritto per valutarne la fattibilità, il tempo e quindi gli eventuali costi.
Grazie per la comprensione e il continuo supporto.

</details>

---

## Status (v0.5)

Fully playable. Measured on a Switch at stock clocks, launched as a Sphaira
4-core forwarder:

| Screen | Resolution | Frame rate |
|---|---|---|
| Missions | 320×240 | ~22 fps (the original game caps at 30) |
| Main menu | 640×480 | ~22 fps |
| Briefings | 640×480 | ~13 fps |

- Native audio (libnx `audout`), FlashRAM saves on the SD card
- Joy-Con / Pro Controller and handheld touch screen (tap to point, drag to
  select, two-finger pan, instant touch cursor)
- Software renderer spread over 4 cores (3 cores when launched from hbmenu)

**Next (v1.0):** GPU renderer through Mesa NVK, to raise menus/briefings to the
game's cap and allow lower clocks.

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
git clone https://github.com/gabryboy12-ai/StarCraft64-NX.git
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

### 4. Recompile, prepare the runtime, build

```bash
make recompile ROM="$ROM"
python3 tools/recompile/patch_runtime.py third_party/N64ModernRuntime local/deps/N64ModernRuntime
python3 tools/recompile/patch_host_runtime.py local/deps/N64ModernRuntime
python3 -c "import sys; sys.path.insert(0,'tools/desktop'); import patch_shell_runtime as p; from pathlib import Path; p.prepare(Path('local/deps/N64ModernRuntime'), Path('local/deps/N64ModernRuntime-shell'))"
python3 tools/desktop/bootstrap.py
make icon ROM="$ROM" PYTHON=.venv/bin/python   # optional: NRO/forwarder icon from the title art
./tools/switch/build.sh                         # -> local/build/switch/StarCraft64.nro
```

### 5. Install on the SD card

1. Create the folder `switch/StarCraft64/` on the SD card.
2. Copy `local/build/switch/StarCraft64.nro` into it.
3. Copy your ROM into the same folder and rename it to **`rom.z64`**.

Never launch it from the album (applet mode has too little memory and CPU).
Use one of the two methods below. Logs (`sc64.log`) and, after a crash, a
`crash.txt` report are written to `switch/StarCraft64/`.

### 6a. Sphaira forwarder (recommended: icon on the HOME menu, 4 cores)

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

### 6b. hbmenu title takeover

Hold **R** while opening any installed game, then pick StarCraft 64 in hbmenu.
This uses 3 cores: fully playable, slightly slower menus.

## Android APK from Linux/WSL

Upstream targets macOS; on Linux the Android build only needs Linux paths
(already applied in `tools/android/build.py`) and an Android SDK with NDK r29
and Build-Tools 36. See [docs/android.md](docs/android.md) and
[docs/upstream-README.md](docs/upstream-README.md).

## What changed for Switch

- `native/switch/` — libnx shell (framebuffer video, HID pads + touch, crash
  handler, 2 MiB thread stacks), SD platform layer, CMake profile
- `tools/switch/` — Docker build, runtime derivation with asserted
  `__SWITCH__` patches (semaphores, memory, thread cores/priorities, no code
  mods), `addr2line` helper
- Shared fixes that also help other platforms: word-swapped RDRAM snapshot,
  cross-frame S2DEX background cache, FlashRAM saves on FAT, touch cursor warp
  and faster two-finger camera pan

## Credits and license

- [n0madic/StarCraft64-recomp](https://github.com/n0madic/StarCraft64-recomp) —
  the recompilation, runtime integration and renderer this port builds on
- [N64Recomp / N64ModernRuntime](https://github.com/N64Recomp), devkitPro /
  libnx, decompals ultralib, shygoo n64sym, miniaudio, SDL
- Switch port: gabryboy12-ai

Source code: MIT (see [LICENSE](LICENSE)), except files noted there.
StarCraft is a trademark of Blizzard Entertainment; StarCraft 64 and its
assets belong to their respective owners. This project is not affiliated with
or endorsed by Blizzard Entertainment or Nintendo.
