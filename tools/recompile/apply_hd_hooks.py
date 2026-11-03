"""HD mission hooks for the N64Recomp output (local/generated/game).

usage: python3 -I tools/recompile/apply_hd_hooks.py <generated game dir>   (after apply_music_hook.py)

The 1P mission view is a CPU-drawn CI8 canvas: the camera renderer 0x8000ACE4
copies the terrain into it (0x800049D0), then drawImage 0x8012011C (mission
overlay) calls each image's render function. native/shell/hd.cpp redraws the
same frame at twice the resolution from the user's PC graphics:
  8000AE44  before the terrain copy   sc64_hd_terrain: a new HD frame, PC terrain
  80120270  before the render call    sc64_hd_image: the image's PC frame (s0 = image)
            after it                    sc64_hd_image_done: an image without a PC frame, at 2x
Without PC graphics installed both return at once.
"""
import sys
from pathlib import Path

SITES = [  # (function signature, instruction comment, call line, hook)
    ('RECOMP_FUNC void sc64_resident_8000ACE4(uint8_t* rdram, recomp_context* ctx) {',
     '// 0x8000AE44: jal         0x800049D0', 'sc64_resident_800049D0(rdram, ctx);', 'sc64_hd_terrain(rdram, ctx);'),
    ('RECOMP_FUNC void sc64_game_8012011C(uint8_t* rdram, recomp_context* ctx) {',
     '// 0x80120270: jalr        $v0', 'LOOKUP_FUNC(ctx->r2)(rdram, ctx);', 'sc64_hd_image(rdram, ctx);'),
]
AFTER = ('LOOKUP_FUNC(ctx->r2)(rdram, ctx);', 'sc64_hd_image_done(rdram, ctx);') # after the render call
DECL = ('void sc64_hd_terrain(uint8_t* rdram, recomp_context* ctx);\n'
        'void sc64_hd_image(uint8_t* rdram, recomp_context* ctx);\n'
        'void sc64_hd_image_done(uint8_t* rdram, recomp_context* ctx);\n')

def main():
    root = Path(sys.argv[1]); found = 0
    for path in sorted(root.glob('*.c')):
        text = path.read_text()
        sites = [s for s in SITES if s[0] in text]
        if not sites: continue
        if 'sc64_hd_' in text: sys.exit('hd: hooks already applied')
        lines = text.split('\n')
        for signature, comment, call, hook in sites:
            start = lines.index(signature)
            at = next(i for i in range(start, len(lines)) if lines[i].strip() == comment)
            for i in range(at, at + 6):
                if lines[i].strip() == call:
                    lines[i] = lines[i].replace(call, hook + ' ' + call)
                    if call == AFTER[0]: lines[i] += ' ' + AFTER[1]
                    found += 1; break
        text = '\n'.join(lines)
        # After the includes, where recomp_context is declared.
        head, sep, rest = text.partition('\n\n')
        path.write_text(head + sep + DECL + rest)
    if found != len(SITES):
        sys.exit(f'hd: anchors changed ({found} of {len(SITES)} hooks)')
    print('hd: terrain and image hooks')

if __name__ == '__main__':
    main()
