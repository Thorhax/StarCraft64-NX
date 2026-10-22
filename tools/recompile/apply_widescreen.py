"""Runtime-switchable 1P widescreen for the N64Recomp output (local/generated/game).

usage: python3 -I tools/recompile/apply_widescreen.py <generated game dir>   (run on a fresh recompile)

Replaces the 1P constants that fix the mission view at 320x240 with reads of
sc64_ws_* variables (native/runtime/widescreen.hpp). With the defaults the code
behaves exactly as recompiled; the shell sets them before the guest starts.
  80064674/800646F4  viewport width    (0x8002443C a0)
  800646BC/8006473C  mode width        (0x80091FA0 a3: frame buffers)
  800646D8/80064758  canvas width      (0x800243D0 a0: CI8 surface)
  after 800646D0/80064750  OSViMode copy at s0: width, X scale, field origins
  80024CFC           fog texture width (0x80066B98 a3)
  800246F8           1P cursor X limit (640-unit space)
  80008280           1P command canvas X (0x80066594 a1)
  80033A38           1P status line X (0x800AFBD0)
  8001B8F8 entry     dialog activation: sc64_ws_dialog moves 320-wide layouts
"""
import re, sys, glob
from pathlib import Path

EDITS = [  # (vram, original immediate, variable)
    (0x80064674, '0X140', 'sc64_ws_width'), (0x800646F4, '0X140', 'sc64_ws_width'),
    (0x800646BC, '0X140', 'sc64_ws_width'), (0x8006473C, '0X140', 'sc64_ws_width'),
    (0x800646D8, '0X140', 'sc64_ws_width'), (0x80064758, '0X140', 'sc64_ws_width'),
    (0x80024CFC, '0X30', 'sc64_ws_fog_width'),
    (0x800246F8, '0X260', 'sc64_ws_cursor_max'),
    (0x80008280, '0XB0', 'sc64_ws_command_x'), (0x80033A38, '0X56', 'sc64_ws_status_x'),
]
VI_CALLS = ('0x800646D0', '0x80064750')
DIALOG = 'RECOMP_FUNC void sc64_resident_8001B8F8(uint8_t* rdram, recomp_context* ctx) {'
DECL = ('extern int sc64_ws_width, sc64_ws_fog_width, sc64_ws_cursor_max, sc64_ws_command_x, sc64_ws_status_x;\n'
        'void sc64_ws_dialog(unsigned char* rdram, unsigned int dialog);\n')

def main():
    root = Path(sys.argv[1]); done = 0; vi = 0; dialog = 0
    for path in sorted(root.glob('*.c')):
        lines = path.read_text().split('\n'); changed = False
        for i, line in enumerate(lines):
            m = re.match(r'\s*// 0x([0-9A-F]{8}):', line)
            if not m or i + 1 >= len(lines): continue
            vram = int(m.group(1), 16)
            if vram == 0x8001B8F8 and DIALOG in lines[max(0, i - 4):i]:
                lines[i - 1] += ' if (sc64_ws_width != 320) sc64_ws_dialog(rdram, (uint32_t)ctx->r4);'
                dialog += 1; changed = True
            for v, old, var in EDITS:
                if v == vram and f'ADD32(0, {old});' in lines[i + 1]:
                    lines[i + 1] = lines[i + 1].replace(f'ADD32(0, {old});', f'ADD32(0, {var});'); done += 1; changed = True
            if m.group(1) in [c[2:].upper() for c in VI_CALLS] and 'jal' in line:
                for j in range(i + 1, i + 5):
                    if lines[j].strip() == 'sc64_resident_80091FA0(rdram, ctx);':
                        lines[j] += (' if (sc64_ws_width != 320) { MEM_W(0X8, ctx->r16) = sc64_ws_width;'
                                     ' MEM_W(0X20, ctx->r16) = (sc64_ws_width * 1024 + 320) / 640;'
                                     ' MEM_W(0X28, ctx->r16) = 2 * sc64_ws_width; MEM_W(0X3C, ctx->r16) = 2 * sc64_ws_width; }')
                        vi += 1; changed = True; break
        if changed:
            text = '\n'.join(lines)
            if DECL not in text: text = DECL + text
            path.write_text(text)
    # Each delay-slot instruction is emitted twice; every site must be found.
    if done < len(EDITS) or vi != len(VI_CALLS) or dialog != 1:
        sys.exit(f'widescreen: anchors changed ({done} immediates, {vi} mode writes, {dialog} dialog hooks)')
    print(f'widescreen: {done} immediates, {vi} video mode writes, dialog hook')

if __name__ == '__main__':
    main()
