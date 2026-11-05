# Installer: build your own SD card

The installer turns **your own** StarCraft 64 cartridge dump and **your own**
classic StarCraft PC install into a ready-to-copy `switch/StarCraft64/` folder:
the Switch NRO, built on your computer, plus the PC graphics, music, cinematics
and voices. Nothing game-related is downloaded or published: the installer is
only tools, and everything it produces stays on your computer.

## What you need

- **Docker**: [Docker Desktop](https://docs.docker.com/desktop/) on Windows or
  macOS, or Docker on Linux. About 15 GB of free disk space.
- **Your StarCraft 64 (USA) ROM** (`.z64`, `.v64` or `.n64`): the installer
  checks it is the supported dump (SHA-1 of the `.z64`:
  `a36d91b5aac7bb95df0d7de5f9814f5270e25c2b`).
- Optional, for the PC graphics, music, cinematics and voices: your **classic
  StarCraft / Brood War 1.16** install, the folder that contains `StarDat.mpq`,
  `BrooDat.mpq`, `CampnSC.dat` and `CampnBW.dat`. Without it the game plays with
  the N64 graphics and music and silent briefings.
- This repository: download it (*Code* → *Download ZIP*, then unzip) or
  `git clone https://github.com/Thorhax/StarCraft64-NX.git`.

## Run it

**Windows** (PowerShell, in the repository folder):

```powershell
powershell -ExecutionPolicy Bypass -File tools\installer\install.ps1 `
    -Rom "C:\Games\StarCraft 64 (USA).z64" -StarCraft "C:\Games\StarCraft"
```

Run it without `-Rom` / `-StarCraft` and it asks for the paths.

**Linux, macOS, Windows WSL** (in the repository folder):

```bash
tools/installer/install.sh --rom "StarCraft 64 (USA).z64" --starcraft /path/to/StarCraft
```

The first run builds the installer image (30-60 minutes: it compiles Mesa, the
recompiler and the N64 SDK references); later runs reuse it. Building the game
then takes another 20-40 minutes. Options:

| Option | Effect |
|---|---|
| `--out <folder>` / `-Out <folder>` | where to write the SD card files (default `sd-card`) |
| `--no-widescreen` / `-NoWidescreen` | 4:3 missions (default: widescreen) |

## Copy it to the Switch

Copy the `switch` folder from the output folder to the root of your SD card
(merge it with the existing `switch` folder), then launch StarCraft 64 as
described in the README (*Sphaira forwarder* or *hbmenu title takeover*). Keep
the output folder private: it contains your ROM and files from your StarCraft
install.

## What it does

Inside Docker, `tools/installer/build_inside.sh` follows the README's manual
build steps:

1. Normalises the ROM's byte order and checks its SHA-1.
2. Builds the libultra inventory against the pinned N64 SDK references.
3. Recompiles the game with N64Recomp and adds this port's hooks
   (`make recompile`), then prepares the runtime.
4. Builds the NRO with devkitPro and Mesa for Horizon (GPU renderer); the icon
   is cropped from your ROM's title screen.
5. Extracts the HD graphics and animated portraits, the PC soundtrack, the PC
   cinematics and the briefing and in-mission voices from your StarCraft files
   (each part is skipped when its source files are missing).

Logs of every step are in the output folder's `logs/` (and `image-build.log` for
the image). The image is defined by `tools/installer/Dockerfile`; every source it
downloads is pinned to a commit or tag, and `.dockerignore` keeps ROMs and
private files out of it.
