#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import sys
import re
import os
import glob
import msvcrt

class Glyph:
    __slots__ = ("enc", "w", "h", "xoff", "yoff", "dwx", "rows", "row_bytes")
    def __init__(self):
        self.enc = self.w = self.h = self.xoff = self.yoff = self.dwx = 0
        self.rows = []
        self.row_bytes = 0

def ask_yes_no(question):
    print(f"{question} [y/n] ", end="", flush=True)
    while True:
        key = msvcrt.getch().decode('utf-8', errors='ignore').lower()
        if key in ('y', '1', '+'):
            print("y")
            return True
        elif key in ('n', '0', '-'):
            print("n")
            return False
        elif key == '\x1b':
            sys.exit(0)

def find_single_bdf():
    files = glob.glob("*.bdf")
    if not files:
        print("Error: No BDF files found!"); sys.exit(1)
    files.sort()
    return files[0]

def parse_bdf(path):
    gl = {}
    ascent = descent = 8
    g = None
    in_bitmap = False
    hex_re = re.compile(r'^[0-9A-Fa-f]+$')
    with open(path, "rb") as f:
        for raw_line in f:
            line = raw_line.decode("latin-1").rstrip("\n")
            s = line.strip()
            if not s: continue
            t = s.split()
            k = t[0]
            if k == "FONT_ASCENT" and len(t) >= 2: ascent = int(t[1])
            elif k == "FONT_DESCENT" and len(t) >= 2: descent = int(t[1])
            elif k == "STARTCHAR": g = Glyph(); in_bitmap = False
            elif k == "ENCODING" and g: g.enc = int(t[1])
            elif k == "DWIDTH" and g: g.dwx = int(t[1])
            elif k == "BBX" and g:
                g.w = int(t[1]); g.h = int(t[2])
                g.xoff = int(t[3]); g.yoff = int(t[4])
                g.row_bytes = (g.w + 7) // 8
            elif k == "BITMAP" and g: in_bitmap = True
            elif k == "ENDCHAR" and g:
                if g.enc >= 0: gl[g.enc] = g
                g = None; in_bitmap = False
            elif in_bitmap and g:
                h = t[0]
                if not hex_re.match(h): continue
                if len(h) % 2: h = "0" + h
                row = bytes.fromhex(h)
                if g.row_bytes and len(row) != g.row_bytes:
                    row = row[:g.row_bytes] + b"\x00" * max(0, g.row_bytes - len(row))
                g.rows.append(row)
    return gl, ascent, descent

def build_custom_arrays(gl, selected_codes, ascent, descent):
    # top/height считаются ТОЛЬКО по глифам, реально попадающим в экспорт
    # (selected_codes), а не по всему исходному BDF-файлу и не по глобальным
    # FONT_ASCENT/FONT_DESCENT — иначе, например, digit-only шрифт наследует
    # ascent/descent всей гарнитуры (буквы вроде p/g/y с descenders) и
    # резервирует под них место, которого в выбранном наборе нет.
    selected_glyphs = [gl[cp] for cp in selected_codes if cp in gl]
    top    = max((g.yoff + g.h for g in selected_glyphs), default=ascent)
    bottom = min((g.yoff for g in selected_glyphs), default=-descent)
    height = top - min(bottom, 0)
    bytes_per_col = (height + 7) // 8
    widths, xoff, yoff, dwx, off, g_h, lut = [], [], [], [], [], [], []
    bitmap = bytearray()
    for cp in sorted(selected_codes):
        g = gl.get(cp)
        if not g: continue
        lut.append(cp); off.append(len(bitmap)); widths.append(g.w); xoff.append(g.xoff)
        yoff.append(top - (g.yoff + g.h)); dwx.append(g.dwx)
        r_top, r_bot = 0, -1
        for r in range(g.h):
            row_bits = any(bdf_row_bit(g, r, c) for c in range(g.w))
            if row_bits:
                if r_top == 0 and r_bot == -1: r_top = r
                r_bot = r
        g_h.append((r_bot - r_top + 1) if r_bot >= r_top else 0)
        for c in range(g.w):
            col = [0] * bytes_per_col
            for r in range(g.h):
                if bdf_row_bit(g, r, c): col[r // 8] |= (1 << (7 - (r % 8)))
            bitmap.extend(col)
    return widths, xoff, yoff, dwx, off, bytes(bitmap), height, g_h, lut

def bdf_row_bit(g, r, c):
    j = c // 8; m = 0x80 >> (c % 8)
    return 1 if (j < len(g.rows[r]) and (g.rows[r][j] & m)) else 0

def format_arr(name, data, tp):
    out = [f"static const {tp} {name}[] = {{"]
    for i in range(0, len(data), 12):
        chunk = data[i:i+12]
        line = ", ".join(f"0x{v:02X}" if tp == "uint8_t" else str(v) for v in chunk)
        out.append(f"  {line},")
    out.append("};")
    return "\n".join(out)

def main():
    bdf_file = find_single_bdf()
    gl, asc, dsc = parse_bdf(bdf_file)
    print(f"\nBDF: {bdf_file}\nSelect mode:")
    
    if ask_yes_no("  Только цифры?"):
        sel = [32, 45] + list(range(48, 59)); suf = "_dig"
    elif ask_yes_no("  Кириллица?"):
        sel = [32] + list(range(48, 59)) + list(range(1040, 1104)); suf = "_cyr"
    else:
        print("  Using default Latin (32-126)"); sel = list(range(32, 127)); suf = ""

    codes = sorted([c for c in sel if c in gl])
    w, xo, yo, dw, off, bmap, h, gh, lut = build_custom_arrays(gl, codes, asc, dsc)
    
    f_name = os.path.splitext(os.path.basename(bdf_file))[0] + suf
    src = [f"#include \"fonts.h\"\n", format_arr("font_bitmap", bmap, "uint8_t"),
           format_arr("font_widths", w, "uint8_t"), format_arr("font_offsets", off, "uint16_t"),
           format_arr("font_xoffset", xo, "int8_t"), format_arr("font_yoffset", yo, "int8_t"),
           format_arr("font_dwidth", dw, "uint8_t"), format_arr("font_gh", gh, "uint8_t"),
           format_arr("font_lut", lut, "uint16_t")]
    
    src.append(
    f"const font_t {f_name} = {{\n"
    f"    .bitmap          = font_bitmap,\n"
    f"    .widths          = font_widths,\n"
    f"    .offsets         = font_offsets,\n"
    f"    .xoffset         = font_xoffset,\n"
    f"    .yoffset         = font_yoffset,\n"
    f"    .dwidth          = font_dwidth,\n"
    f"    .glyph_heights   = font_gh,\n"
    f"    .lut             = font_lut,\n"
    f"    .lut_size        = {len(lut)},\n"
    f"    .height          = {h},\n"
    f"    .first_char      = {lut[0] if lut else 0},\n"
    f"    .last_char       = {lut[-1] if lut else 0},\n"
    f"    .storage_type    = 0\n"
    f"}};"
)
    
    with open(f"{f_name}.c", "w", encoding="utf-8") as f: f.write("\n\n".join(src))
    print(f"Saved {f_name}.c ({len(lut)} symbols)")

if __name__ == "__main__": main()