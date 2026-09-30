#!/usr/bin/env python3
"""Генерация App/Screen/splash.c из Icons/Designer.png (320x240, RGB565,
row-major, без альфа-канала — тот же формат, что у остальных растров в
App/Screen, см. sleep_icon.c/solder_icon.c). Не часть сборки, запускается
руками при смене картинки заставки."""
from PIL import Image

SRC = "Icons/Designer.png"
DST = "App/Screen/splash.c"
W, H = 320, 240

img = Image.open(SRC).convert("RGB")
assert img.size == (W, H), img.size

def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

pixels = list(img.getdata())
vals = [rgb565(r, g, b) for (r, g, b) in pixels]
assert len(vals) == W * H

lines = []
lines.append('/**')
lines.append(' * @file splash.c')
lines.append(f' * @brief Растр заставки при включении — RGB565, {W}x{H}, сгенерирован')
lines.append(f' *        из {SRC} ({W}x{H}, без альфа-канала) скриптом')
lines.append(' *        tools/gen_splash.py. Размер картинки СОВПАДАЕТ с экраном при')
lines.append(' *        DISPLAY_ROTATION_0 — без масштабирования и кропа.')
lines.append(' */')
lines.append('')
lines.append('#include "splash.h"')
lines.append('')
lines.append('const uint16_t SplashScreen_Bitmap[SPLASH_BITMAP_W * SPLASH_BITMAP_H] = {')
for i in range(0, len(vals), 12):
    chunk = vals[i:i+12]
    lines.append('    ' + ', '.join(f'0x{v:04X}' for v in chunk) + ',')
lines.append('};')
lines.append('')

with open(DST, 'w', encoding='utf-8', newline='\n') as f:
    f.write('\n'.join(lines))

print(f"Written {DST}: {len(vals)} px, {len(vals)*2} bytes")
