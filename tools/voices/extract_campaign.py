"""Extract classic StarCraft 1.x campaign maps + briefing WAVs from the CD install
archives (INSTALL.EXE, here CampnSC.dat / CampnBW.dat) into the CascView-style
layout tools/voices/extract_briefings.py reads:
    <out>/enUS/campaign/<Race>/<mission>/staredit/{scenario.chk, wav/*.wav}"""
import ctypes, struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent)); import mpq
mpq.lib.SFileHasFile.argtypes = [mpq.H, ctypes.c_char_p]
RACES = {'terran': 'Terran', 'zerg': 'Zerg', 'protoss': 'Protoss',
         'expterran': 'EXPTerran', 'expzerg': 'EXPZerg', 'expprotoss': 'EXPProtoss'}

def str_table(chk):
    pos, sec = 0, {}
    while pos + 8 <= len(chk):
        name, size = chk[pos:pos+4], struct.unpack_from('<i', chk, pos+4)[0]; pos += 8
        if size < 0: pos += size; continue
        sec[name] = chk[pos:pos+size]; pos += size
    data = sec.get(b'STR ', b'')
    if not data: return []
    count = struct.unpack_from('<H', data)[0]; out = []
    for i in range(count):
        off = struct.unpack_from('<H', data, 2 + 2*i)[0]
        end = data.find(b'\0', off); out.append(data[off:end].decode('cp1252', 'replace'))
    return out

def main(install, out):
    install, out = Path(install), Path(out)
    total = {'maps': 0, 'wavs': 0, 'missing': 0}
    for archive in ('CampnSC.dat', 'CampnBW.dat'):
        h = mpq.open_archive(install / archive)
        for race, race_dir in RACES.items():
            base = race.removeprefix('exp')
            for n in range(0, 13):
                for suf in ('', 'a', 'b', 'c'):
                    mission = f'{base}{n:02d}{suf}'
                    root = f'campaign\\{race}\\{mission}\\'
                    chk = mpq.read(h, root + 'staredit\\scenario.chk')
                    if chk is None: continue
                    dst = out / 'enUS' / 'campaign' / race_dir / mission / 'staredit'
                    (dst / 'wav').mkdir(parents=True, exist_ok=True)
                    (dst / 'scenario.chk').write_bytes(chk); total['maps'] += 1
                    for s in {s for s in str_table(chk) if s.lower().endswith('.wav')}:
                        data = mpq.read(h, root + s)
                        if data is None:
                            print(f'missing {archive}:{root}{s}'); total['missing'] += 1; continue
                        (dst / 'wav' / s.replace('\\', '/').split('/')[-1]).write_bytes(data)
                        total['wavs'] += 1
    print(total)

if __name__ == '__main__':
    main(*sys.argv[1:3])
