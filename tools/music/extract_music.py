"""PC StarCraft music for StarCraft64-NX from the user's own CD install.

usage: STORMLIB=/path/libstorm.so python3 -I extract_music.py <StarCraft dir> <music dir>

Reads music\\*.wav from CampnSC.dat / CampnBW.dat (the CD INSTALL.EXE archives,
copied by a classic CD install), StarDat.mpq / BrooDat.mpq, or a loose music\\
folder, and writes Ogg Vorbis files (22 kHz stereo, as on the CD) that
native/shell/music.cpp plays in place of the N64 streams. Brood War's CD stores
each new race theme in front of the original one (its terran1.wav is the Brood
War Terran theme followed by StarCraft's terran1); the Brood War part is cut out
as bwterran / bwzerg / bwprotoss where the original theme starts. Needs ffmpeg
with libvorbis and numpy.
"""
import os, subprocess, sys, tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'voices'))
import numpy as np

TRACKS = ['title', 'tvict', 'tdefeat', 'zvict', 'zdefeat', 'pvict', 'pdefeat',
          'terran1', 'terran2', 'terran3', 'zerg1', 'zerg2', 'zerg3', 'protoss1', 'protoss2', 'protoss3']
BROOD = {'terran1': 'bwterran', 'zerg1': 'bwzerg', 'protoss1': 'bwprotoss'}
RATE = 22050

def sources(game):
    """{track: {'sc': wav bytes, 'bw': wav bytes}} from every archive or folder found."""
    found = {}
    loose = next((p for p in game.iterdir() if p.name.lower() == 'music' and p.is_dir()), None)
    if loose:
        for f in loose.iterdir():
            if f.suffix.lower() == '.wav' and f.stem.lower() in TRACKS:
                found.setdefault(f.stem.lower(), {})['sc'] = f.read_bytes()
    archives = [('CampnSC.dat', 'sc'), ('StarDat.mpq', 'sc'), ('CampnBW.dat', 'bw'), ('BrooDat.mpq', 'bw')]
    present = {p.name.lower(): p for p in game.iterdir()}
    if any(name.lower() in present for name, _ in archives):
        import mpq
        for name, edition in archives:
            path = present.get(name.lower())
            if not path: continue
            h = mpq.open_archive(path)
            for track in TRACKS:
                data = mpq.read(h, f'music\\{track}.wav')
                if data: found.setdefault(track, {}).setdefault(edition, data)
    return found

def pcm(wav):
    """Mono float samples at 22 kHz, for locating the join."""
    out = subprocess.run(['ffmpeg', '-v', 'error', '-i', '-', '-ac', '1', '-ar', str(RATE), '-f', 's16le', '-'],
                         input=wav, capture_output=True, check=True).stdout
    return np.frombuffer(out, np.int16).astype(np.float32)

def join(bw, sc):
    """Seconds into the Brood War file where the original theme starts."""
    a, b = pcm(bw), pcm(sc)
    probe = b[RATE * 5:RATE * 13]  # 8 s from 5 s into the original
    n = 1 << int(np.ceil(np.log2(len(a) + len(probe))))
    corr = np.fft.irfft(np.fft.rfft(a, n) * np.conj(np.fft.rfft(probe, n)), n)[:len(a) - len(probe)]
    window = np.concatenate([[0], np.cumsum(a.astype(np.float64) ** 2)])
    energy = np.sqrt(np.maximum(window[len(probe):len(probe) + len(corr)] - window[:len(corr)], 0)) + 1e-9
    score = corr / energy / (np.sqrt((probe * probe).sum()) + 1e-9)
    at = int(np.argmax(score))
    return (at - RATE * 5) / RATE, float(score[at])

def encode(wav, target, start=None, end=None):
    with tempfile.NamedTemporaryFile(suffix='.wav') as f:
        f.write(wav); f.flush()
        cut = (['-ss', f'{start:.3f}'] if start else []) + (['-to', f'{end:.3f}'] if end else [])
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', f.name, *cut, '-c:a', 'libvorbis', '-q:a', '5',
                        str(target)], check=True)

def main():
    game, out = Path(sys.argv[1]), Path(sys.argv[2])
    found = sources(game)
    if not found: sys.exit(f'no StarCraft music found in {game}')
    out.mkdir(parents=True, exist_ok=True)
    written = []
    for track in TRACKS:
        editions = found.get(track, {})
        original = editions.get('sc')
        if track in BROOD and 'bw' in editions:
            if original and editions['bw'] != original:
                seconds, score = join(editions['bw'], original)
                if score > 0.8 and seconds > 30:
                    encode(editions['bw'], out / f'{BROOD[track]}.ogg', end=seconds)
                    written.append(f'{BROOD[track]} (first {seconds:.1f} s of the Brood War {track})')
                else:
                    print(f'{track}: Brood War join not found (score {score:.2f}); bw theme skipped')
            elif not original:
                original = editions['bw']  # only the Brood War CD: its file still ends with the original
        source = original or editions.get('bw')
        if source:
            encode(source, out / f'{track}.ogg')
            written.append(track)
    for name in written: print('  ' + name)
    size = sum(f.stat().st_size for f in out.glob('*.ogg'))
    print(f'{len(written)} tracks, {size / 1e6:.1f} MB in {out}')

if __name__ == '__main__':
    main()
