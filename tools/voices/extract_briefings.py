#!/usr/bin/env python3
"""Extract StarCraft PC campaign briefing voices for the StarCraft 64 port.

Input: campaign folders extracted with CascView from your own StarCraft PC
install (free from Blizzard), one per language:
    <source>/<lang>/campaign/<race>/<mission>/staredit/scenario.chk
    <source>/<lang>/campaign/<race>/<mission>/staredit/wav/*.wav

For each mission the briefing script (CHK section MBRF) gives the ordered
transmissions: portrait slot, text, WAV and duration. Output, per language:
    <out>/<lang>/voices.json                     ordered briefing script per mission
    <out>/<lang>/<race>/<mission>/<wav>.pcm      24 kHz mono s16le (ffmpeg)

Nothing here is distributed: every user runs it on their own game files.
"""
import argparse
import json
import shutil
import struct
import subprocess
import sys
from pathlib import Path

# Briefing trigger action types (MBRF), as in StarEdit.
WAIT, PLAY_WAV, TEXT, OBJECTIVES, SHOW_PORTRAIT, HIDE_PORTRAIT, SPEAKING, TRANSMISSION = 1, 2, 3, 4, 5, 6, 7, 8
TRIGGER_SIZE, CONDITIONS, ACTIONS, ACTION_SIZE = 2400, 16 * 20, 64, 32


def sections(chk: bytes):
    """CHK: [name:4][size:u32][data]; a later section of the same name wins."""
    out, pos = {}, 0
    while pos + 8 <= len(chk):
        name = chk[pos:pos + 4].decode('latin-1')
        size = struct.unpack_from('<i', chk, pos + 4)[0]
        pos += 8
        if size < 0:  # some protected maps use negative sizes to overlap sections
            pos += size
            continue
        out[name] = chk[pos:pos + size]
        pos += size
    return out


def strings(sec: dict):
    """STRx (Remastered, 32-bit offsets) or STR (16-bit). IDs are 1-based."""
    if 'STRx' in sec:
        data, wide = sec['STRx'], True
    elif 'STR ' in sec:
        data, wide = sec['STR '], False
    else:
        return lambda i: ''
    count = struct.unpack_from('<I' if wide else '<H', data, 0)[0]
    step = 4 if wide else 2
    offsets = [struct.unpack_from('<I' if wide else '<H', data, step + i * step)[0] for i in range(count)]

    def get(index: int) -> str:
        if index <= 0 or index > count:
            return ''
        start = offsets[index - 1]
        end = data.find(b'\0', start)
        raw = data[start:end if end >= 0 else len(data)]
        for encoding in ('utf-8', 'cp1252', 'latin-1'):
            try:
                return raw.decode(encoding)
            except UnicodeDecodeError:
                continue
        return ''
    return get


def briefing(chk: bytes):
    """Ordered briefing events from MBRF, trigger by trigger, action by action."""
    sec = sections(chk)
    get = strings(sec)
    data = sec.get('MBRF', b'')
    events = []
    for t in range(len(data) // TRIGGER_SIZE):
        base = t * TRIGGER_SIZE + CONDITIONS
        for a in range(ACTIONS):
            o = base + a * ACTION_SIZE
            location, text, wav, time, slot, number = struct.unpack_from('<IIIIII', data, o)
            kind = data[o + 26]
            if kind == 0:
                break
            if kind in (PLAY_WAV, TRANSMISSION):
                events.append({'type': 'transmission' if kind == TRANSMISSION else 'wav',
                               'slot': slot, 'wav': get(wav), 'ms': time,
                               'text': get(text) if kind == TRANSMISSION else ''})
            elif kind == TEXT:
                events.append({'type': 'text', 'text': get(text), 'ms': time})
    return events


def convert(src: Path, dst: Path) -> int:
    """Any WAV codec ffmpeg reads -> 48 kHz mono s16le raw. Returns samples."""
    dst.parent.mkdir(parents=True, exist_ok=True)
    # 24 kHz: the originals are 22 kHz; the player interpolates to 48 kHz.
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', str(src), '-ac', '1', '-ar', '24000',
                    '-f', 's16le', str(dst)], check=True)
    return dst.stat().st_size // 2


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--source', required=True, type=Path, help='folder with <lang>/campaign subfolders')
    parser.add_argument('--out', required=True, type=Path, help='output folder (copy to sdmc:/switch/StarCraft64/voices)')
    parser.add_argument('--lang', action='append', help='only these languages (default: all found)')
    args = parser.parse_args()
    if not shutil.which('ffmpeg'):
        sys.exit('ffmpeg not found: sudo apt install -y ffmpeg')
    langs = args.lang or sorted(p.name for p in args.source.iterdir() if (p / 'campaign').is_dir())
    if not langs:
        sys.exit(f'No <lang>/campaign folders under {args.source}')
    for lang in langs:
        campaign = args.source / lang / 'campaign'
        missions, wavs, missing = {}, 0, 0
        for chk_path in sorted(campaign.glob('*/*/staredit/scenario.chk')):
            mission_dir = chk_path.parent.parent
            key = f'{mission_dir.parent.name}/{mission_dir.name}'
            events = briefing(chk_path.read_bytes())
            if not any(e['type'] in ('transmission', 'wav') for e in events):
                continue
            wav_dir = chk_path.parent / 'wav'
            # By name without extension: maps cite .wav, some languages ship .ogg.
            available = {p.stem.lower(): p for p in wav_dir.glob('*')} if wav_dir.is_dir() else {}
            for event in events:
                if event['type'] not in ('transmission', 'wav') or not event['wav']:
                    continue
                name = event['wav'].replace('\\', '/').split('/')[-1].lower()
                src = available.get(Path(name).stem)
                if not src:
                    event['missing'] = True
                    missing += 1
                    continue
                stem = Path(name).stem
                samples = convert(src, args.out / lang / key / f'{stem}.pcm')
                event['file'] = f'{key}/{stem}.pcm'
                event['samples'] = samples
                wavs += 1
            missions[key] = events
        out_json = args.out / lang / 'voices.json'
        out_json.parent.mkdir(parents=True, exist_ok=True)
        out_json.write_text(json.dumps({'language': lang, 'sample_rate': 24000, 'missions': missions},
                                       ensure_ascii=False, indent=1))
        print(f'{lang}: {len(missions)} briefings, {wavs} voice files, {missing} missing -> {out_json}')


if __name__ == '__main__':
    main()
