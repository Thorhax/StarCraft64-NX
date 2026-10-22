"""In-mission transmission voices for StarCraft 64 from the user's PC campaign maps.

usage: python3 -I extract_ingame_voices.py <n64 extracted dir> <pc campaign dir> <voices dir>

The N64 campaign maps (BOLT .chk leaves) keep the PC "Transmission" trigger
actions (type 7) with their text and duration but no WAV. Each N64 map is
paired with the PC mission whose transmissions match it best, each N64
transmission with the PC one of the same text, and that PC WAV is converted to
the briefing format (24 kHz mono s16le). Writes <voices>/enUS/ingame.json:
{sample_rate, missions: {"Terran/terran01": [{text, file, ms}]}} with the N64
text, which the shell matches against the transmission shown in game.
"""
import json, re, struct, subprocess, sys
from pathlib import Path

def sections(data):
    out, o = {}, 0
    while o + 8 <= len(data):
        name = data[o:o + 4].decode('latin-1')
        size = struct.unpack_from('<i', data, o + 4)[0]
        if size < 0 or o + 8 + size > len(data):
            size = max(0, len(data) - o - 8)
        out[name] = data[o + 8:o + 8 + size]  # a later section replaces an earlier one
        o += 8 + size
    return out

def strings(sec):
    count = struct.unpack_from('<H', sec)[0]
    out = {}
    for i in range(count):
        off = struct.unpack_from('<H', sec, 2 + 2 * i)[0]
        if off and off < len(sec):
            end = sec.find(b'\0', off)
            out[i + 1] = sec[off:end if end >= 0 else len(sec)].decode('latin-1')
    return out

def transmissions(path):
    """(text string index, text, wav path, ms) for every Transmission action."""
    s = sections(path.read_bytes())
    if 'TRIG' not in s or 'STR ' not in s:
        return []
    table, trig, out = strings(s['STR ']), s['TRIG'], []
    for t in range(len(trig) // 2400):
        base = t * 2400 + 320
        for a in range(64):
            _, text, wav, ms, _, _, _, kind = struct.unpack_from('<IIIIIIHB', trig, base + a * 32)
            if kind == 0:
                break
            if kind == 7 and text in table:
                out.append((text, table[text], table.get(wav, ''), ms))
    return out

def words(text):
    # The PC text opens with a padded speaker line ("   RAYNOR\r\n   Howdy..."); N64 drops it.
    lines = [l for l in text.replace('\r', '').split('\n') if l.strip()]
    if len(lines) > 1 and lines[0].strip().isupper() and len(lines[0].split()) <= 3:
        lines = lines[1:]
    return {w for w in re.findall(r'[a-z0-9]{3,}', ' '.join(lines).lower())}

def similarity(a, b):
    return len(a & b) / max(1, min(len(a), len(b))) if a and b else 0.0

def main():
    n64_dir, pc_dir, voices = map(Path, sys.argv[1:4])
    pc = {}
    for chk in sorted(pc_dir.glob('*/*/staredit/scenario.chk')):
        key = f'{chk.parents[2].name}/{chk.parents[1].name}'
        if 'tutorial' in key:
            continue
        tx = [(words(text), wav, ms) for _, text, wav, ms in transmissions(chk) if wav and 'blank' not in wav.lower()]
        if tx:
            pc[key] = (chk.parent / 'wav', tx)
    chosen = {}  # key -> (score, n64 path, mapping)
    for chk in sorted(n64_dir.glob('*/*.chk')):
        n64 = transmissions(chk)
        if not n64:
            continue
        best = None
        for key, (wav_dir, tx) in pc.items():
            mapping, total = {}, 0.0
            for index, text, _, ms in n64:
                w = words(text)
                score, wav, pc_ms = max(((similarity(w, pw), pwav, pms) for pw, pwav, pms in tx), default=(0, '', 0))
                total += score
                if score >= 0.6:
                    mapping[index] = (wav, ms, text)
            score = total / len(n64)
            if best is None or score > best[0]:
                best = (score, key, mapping)
        score, key, mapping = best
        if score >= 0.5 and (key not in chosen or score > chosen[key][0]):
            chosen[key] = (score, chk, mapping)
    missions, converted, missing = {}, 0, 0
    out_root = voices / 'enUS'
    for key, (score, chk, mapping) in sorted(chosen.items()):
        wav_dir = pc[key][0]
        entries = []
        for index, (wav, ms, text) in sorted(mapping.items()):
            name = wav.replace('\\', '/').split('/')[-1]
            src = next((p for p in wav_dir.iterdir() if p.name.lower() == name.lower()), None)
            if not src:
                missing += 1
                continue
            rel = f'{key}/ingame/{src.stem.lower()}.pcm'
            dst = out_root / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            if not dst.exists():
                subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', str(src), '-ac', '1', '-ar', '24000',
                                '-f', 's16le', str(dst)], check=True)
                converted += 1
            entries.append({'text': text, 'file': rel, 'ms': ms})
        missions[key] = entries
        print(f'{key}: {chk.parent.name}/{chk.name} score {score:.2f}, {len(entries)} voiced transmissions')
    (out_root / 'ingame.json').write_text(json.dumps({'language': 'enUS', 'sample_rate': 24000,
                                                      'missions': missions}, indent=1) + '\n')
    print(f'{len(missions)} missions, {sum(len(m) for m in missions.values())} transmissions, '
          f'{converted} converted, {missing} missing')

if __name__ == '__main__':
    main()
