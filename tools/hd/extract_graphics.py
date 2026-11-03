"""PC StarCraft graphics for StarCraft64-NX's HD missions, from the user's own install.

usage: STORMLIB=/path/libstorm.so python3 -I extract_graphics.py <StarCraft dir> <hd dir>

Copies, under lower-case names, what native/shell/hd.cpp draws missions with:
  arr/images.dat, arr/images.tbl          Brood War image table (same IDs as the N64 game)
  tileset/<era>.cv5/.vx4/.vr4/.wpe        the eight terrain tilesets
  unit/**.grp                             every image graphic images.dat names
from BrooDat.mpq (Brood War) over StarDat.mpq, then Patch_rt.mpq, as the game loads them.
"""
import os, struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'voices'))
import mpq

TILESETS = ['badlands', 'platform', 'install', 'ashworld', 'jungle', 'desert', 'ice', 'twilight']

def main():
    game, out = Path(sys.argv[1]), Path(sys.argv[2])
    present = {p.name.lower(): p for p in game.iterdir()}
    archives = [mpq.open_archive(present[n]) for n in ('patch_rt.mpq', 'broodat.mpq', 'stardat.mpq') if n in present]
    if not archives: sys.exit(f'no StarDat.mpq / BrooDat.mpq in {game}')

    def read(name):
        for h in archives:
            data = mpq.read(h, name)
            if data: return data
        return None

    def save(name, data):
        target = out / name.replace('\\', '/').lower()
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        return len(data)

    total = files = 0
    for name in ('arr\\images.dat', 'arr\\images.tbl'):
        data = read(name)
        if not data: sys.exit(f'{name} missing')
        total += save(name, data); files += 1
    for era in TILESETS:
        for ext in ('cv5', 'vx4', 'vr4', 'wpe'):
            data = read(f'tileset\\{era}.{ext}')
            if data: total += save(f'tileset\\{era}.{ext}', data); files += 1
            else: print(f'tileset {era}.{ext} missing')
    images = (out / 'arr/images.dat').read_bytes()
    table = (out / 'arr/images.tbl').read_bytes()
    count = struct.unpack_from('<H', table)[0]
    names = []
    for i in range(count):
        offset = struct.unpack_from('<H', table, 2 + 2 * i)[0]
        names.append(table[offset:table.index(b'\0', offset)].decode('latin-1'))
    grps = sorted({struct.unpack_from('<I', images, 4 * i)[0] for i in range(999)})
    missing = []
    for index in grps:
        if not 0 < index <= len(names): continue
        name = 'unit\\' + names[index - 1]
        data = read(name)
        if data: total += save(name, data); files += 1
        else: missing.append(name)
    if missing: print(f'{len(missing)} graphics missing, e.g. {missing[:3]}')
    print(f'{files} files, {total / 1e6:.1f} MB in {out}')

if __name__ == '__main__':
    main()
