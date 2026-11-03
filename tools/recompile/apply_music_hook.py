"""PC music hook for the N64Recomp output (local/generated/game).

usage: python3 -I tools/recompile/apply_music_hook.py <generated game dir>   (after apply_widescreen.py)

The game streams its music from BOLT directory 015 as IMA ADPCM, decoded per
voice by 0x80094CDC (a0 = voice; +8 ROM address, +0x4C/+0x50 left/right
volume, 16.16). On entry the decoder now asks sc64_music_voice (native/runtime)
whether the shell plays a PC track for that stream; if so the two volume loads
read 0, so the voice keeps its stream position but adds nothing to the mix.
Without PC music installed the hook always answers 0.
  80094CDC entry   sc64_music_voice(rdram, voice)
  80094EA4         left volume load
  80094EB0         right volume load
"""
import re, sys
from pathlib import Path

DECODER = 'RECOMP_FUNC void sc64_resident_80094CDC(uint8_t* rdram, recomp_context* ctx) {'
LOADS = {0x80094EA4: 'ctx->r4 = MEM_W(ctx->r16, 0X4C);', 0x80094EB0: 'ctx->r3 = MEM_W(ctx->r16, 0X50);'}
DECL = 'int sc64_music_voice(unsigned char* rdram, unsigned int voice);\n'

def main():
    root = Path(sys.argv[1]); entry = 0; loads = 0
    for path in sorted(root.glob('*.c')):
        text = path.read_text()
        if DECODER not in text: continue
        if 'sc64_music_voice' in text: sys.exit('music: hook already applied')
        lines = text.split('\n')
        for i, line in enumerate(lines):
            if line == DECODER:
                lines[i] += ' const int sc64_music_mute = sc64_music_voice(rdram, (uint32_t)ctx->r4);'
                entry += 1; continue
            m = re.match(r'\s*// 0x([0-9A-F]{8}):', line)
            if m and int(m.group(1), 16) in LOADS and i + 1 < len(lines):
                load = LOADS[int(m.group(1), 16)]
                if lines[i + 1].strip() == load:
                    register = load.split(' = ')[0]
                    lines[i + 1] += f' if (sc64_music_mute) {register} = 0;'
                    loads += 1
        path.write_text(DECL + '\n'.join(lines))
    if entry != 1 or loads != len(LOADS):
        sys.exit(f'music: anchors changed ({entry} entry hooks, {loads} volume loads)')
    print('music: decoder hook, 2 volume loads')

if __name__ == '__main__':
    main()
