"""Copy the classic StarCraft cinematics (smk\\*.smk) out of the CD install archives.

usage: STORMLIB=/path/libstorm.so python3 -I extract_movies.py <StarCraft dir> <out dir>
CampnSC.dat / CampnBW.dat are the CD INSTALL.EXE files (no listfile), so the
names come from the game executables' own smk\\ string table.
"""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mpq

source, out = sys.argv[1], sys.argv[2]
names = set()
for exe in ('StarCraft.exe', 'BroodWar.exe'):
    path = os.path.join(source, exe)
    if os.path.exists(path):
        names.update(m.decode() for m in re.findall(rb'smk\\[A-Za-z0-9_]+\.smk', open(path, 'rb').read()))
os.makedirs(out, exist_ok=True)
found = {}
for archive in ('CampnSC.dat', 'CampnBW.dat', 'StarDat.mpq', 'BrooDat.mpq'):
    path = os.path.join(source, archive)
    if not os.path.exists(path):
        continue
    h = mpq.open_archive(path)
    for name in sorted(names - found.keys()):
        data = mpq.read(h, name)
        if data:
            target = os.path.join(out, name.split('\\')[1].lower())
            with open(target, 'wb') as f:
                f.write(data)
            found[name] = archive
            print(f'{archive}: {name} {len(data)}')
print(f'{len(found)} of {len(names)} movies extracted to {out}')
