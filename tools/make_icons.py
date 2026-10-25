"""Draws the HUD widget's icons: mod/textures/hud/dk2ml_helmetcam_<menu|select|lock|action|nv>_<a|b>_<normal|hover>.dds and
the arc ring behind them (dk2ml_helmetcam_wheel_ring.dds).

The look follows the game's own HUD toggles (round dark badge, cream glyph, broken ring; hover = inverted). The "_b" state
is the active one and is drawn in the HUD's amber. Output: uncompressed 32-bit BGRA DDS, 104x104, like the game's
toggle_sussy_*.dds. Needs Pillow:  python tools/make_icons.py
"""
import math
import os
import struct

from PIL import Image, ImageDraw

SIZE = 104
SS = 4  # supersampling for smooth edges
CREAM = (240, 227, 204)
DARK = (30, 26, 26)
AMBER = (255, 162, 3)
GREEN = (60, 255, 60)
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'mod', 'textures', 'hud')


def px(v):
    return int(round(v * SS))


def draw_ring(d, color):
    # four arcs with gaps, like the game's badges
    c = SIZE / 2
    r = 44
    for k in range(4):
        start = k * 90 + 14
        d.arc([px(c - r), px(c - r), px(c + r), px(c + r)], start, start + 62, fill=color, width=px(3))


def glyph_select(d, fg, active):
    # a pointer arrow; passive = key cap beside it, active = lightning bolt
    pts = [(38, 28), (38, 62), (46, 55), (52, 68), (58, 65), (52, 52), (63, 52)]
    d.polygon([(px(x), px(y)) for x, y in pts], fill=fg)
    if active:
        bolt = [(70, 30), (62, 46), (68, 46), (63, 62), (76, 42), (69, 42), (74, 30)]
        d.polygon([(px(x), px(y)) for x, y in bolt], fill=fg)
    else:
        d.rounded_rectangle([px(62), px(34), px(78), px(50)], radius=px(3), outline=fg, width=px(3))


def glyph_lock(d, fg, active):
    # padlock; passive = open shackle, active = closed
    d.rounded_rectangle([px(32), px(48), px(72), px(76)], radius=px(5), fill=fg)
    if active:
        d.arc([px(38), px(24), px(66), px(60)], 180, 360, fill=fg, width=px(5))
        d.line([px(40), px(42), px(40), px(50)], fill=fg, width=px(5))
        d.line([px(64), px(42), px(64), px(50)], fill=fg, width=px(5))
    else:
        d.arc([px(38), px(24), px(66), px(60)], 180, 360, fill=fg, width=px(5))
        d.line([px(40), px(42), px(40), px(50)], fill=fg, width=px(5))
        d.line([px(64), px(40), px(64), px(34)], fill=fg, width=px(5))
    d.ellipse([px(48), px(57), px(56), px(65)], fill=DARK if fg != DARK else CREAM)


def glyph_action(d, fg, active):
    # crosshair; active = filled centre dot and brighter ticks
    c = SIZE / 2
    d.ellipse([px(c - 20), px(c - 20), px(c + 20), px(c + 20)], outline=fg, width=px(4))
    for dx, dy in ((0, -1), (0, 1), (-1, 0), (1, 0)):
        d.line([px(c + dx * 14), px(c + dy * 14), px(c + dx * 30), px(c + dy * 30)], fill=fg, width=px(4))
    if active:
        d.ellipse([px(c - 7), px(c - 7), px(c + 7), px(c + 7)], fill=fg)


def glyph_nv(d, fg, active, lens):
    # the goggles; passive = green lenses (overlay shown), active = dim lenses and a slash (overlay removed)
    d.ellipse([px(24), px(34), px(50), px(60)], outline=fg, width=px(4))
    d.ellipse([px(54), px(34), px(80), px(60)], outline=fg, width=px(4))
    d.line([px(50), px(47), px(54), px(47)], fill=fg, width=px(4))
    d.rounded_rectangle([px(40), px(62), px(64), px(72)], radius=px(2), outline=fg, width=px(3))
    d.ellipse([px(32), px(42), px(42), px(52)], fill=lens)
    d.ellipse([px(62), px(42), px(72), px(52)], fill=lens)
    if active:
        d.line([px(22), px(76), px(82), px(30)], fill=fg, width=px(5))


def glyph_menu(d, fg, active, hover):
    # the main icon: a helmet with a camera lens; active (the cam is open) = amber with a red recording dot
    d.pieslice([px(28), px(26), px(76), px(74)], 180, 360, fill=fg)
    d.rounded_rectangle([px(24), px(50), px(80), px(60)], radius=px(4), fill=fg)
    inner = CREAM if not hover else DARK
    d.ellipse([px(43), px(34), px(61), px(52)], fill=(DARK if not hover else CREAM))
    d.ellipse([px(46), px(37), px(58), px(49)], outline=inner, width=px(2))
    d.ellipse([px(50), px(41), px(54), px(45)], fill=(255, 60, 60) if active else inner)
    d.rounded_rectangle([px(38), px(66), px(66), px(72)], radius=px(2), fill=fg)


def make_ring():
    # the translucent band behind the arc: the top half of an annulus around the main icon (R = 200, buttons 96 px)
    w, h = 520, 260
    img = Image.new('RGBA', (w * 2, h * 2), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx, cy = w, h * 2 - 2
    for r, color, width in ((2 * 255, (240, 227, 204, 40), 2 * 110), (2 * 255, (240, 227, 204, 90), 4), (2 * 145, (240, 227, 204, 90), 4)):
        # outer edge radius r, drawn as an arc of the given pixel width, inwards from r
        d.arc([cx - r, cy - r, cx + r, cy + r], 195, 345, fill=color, width=width)
    return img.resize((w, h), Image.LANCZOS)


def make(control, active, hover):
    img = Image.new('RGBA', (SIZE * SS, SIZE * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    c = SIZE / 2
    disc, ring = (CREAM, CREAM) if hover else (DARK, CREAM)
    glow = (240, 227, 204, 60)
    d.ellipse([px(c - 40), px(c - 40), px(c + 40), px(c + 40)], fill=glow)
    d.ellipse([px(c - 37), px(c - 37), px(c + 37), px(c + 37)], fill=disc)
    draw_ring(d, ring)
    fg = DARK if hover else (AMBER if active else CREAM)
    if control == 'menu':
        glyph_menu(d, fg, active, hover)
    elif control == 'select':
        glyph_select(d, fg, active)
    elif control == 'lock':
        glyph_lock(d, fg, active)
    elif control == 'action':
        glyph_action(d, fg, active)
    else:
        lens = (110, 110, 110) if active else GREEN
        glyph_nv(d, fg, active, DARK if hover and not active else lens)
    return img.resize((SIZE, SIZE), Image.LANCZOS)


def write_dds(path, img):
    w, h = img.size
    # flags and linear size as in the game's own uncompressed icons (toggle_sussy_*.dds)
    header = b'DDS ' + struct.pack('<7I', 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000, h, w, w * h * 4, 0, 0)
    header += b'\0' * 44  # 11 reserved dwords
    header += struct.pack('<8I', 32, 0x41, 0, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    header += struct.pack('<5I', 0x1000, 0, 0, 0, 0)
    # The game reads the rows of uncompressed DDS bottom-first (OpenGL order), so the picture goes in upside down to come
    # out upright in the HUD.
    img = img.transpose(Image.FLIP_TOP_BOTTOM)
    r, g, b, a = img.split()
    bgra = Image.merge('RGBA', (b, g, r, a)).tobytes()
    with open(path, 'wb') as f:
        f.write(header + bgra)


def main():
    os.makedirs(OUT, exist_ok=True)
    count = 0
    for control in ('menu', 'select', 'lock', 'action', 'nv'):
        for state, active in (('a', False), ('b', True)):
            for look, hover in (('normal', False), ('hover', True)):
                write_dds(os.path.join(OUT, 'dk2ml_helmetcam_%s_%s_%s.dds' % (control, state, look)), make(control, active, hover))
                count += 1
    write_dds(os.path.join(OUT, 'dk2ml_helmetcam_wheel_ring.dds'), make_ring())
    print('wrote %d icons and the arc ring to %s' % (count, os.path.normpath(OUT)))


if __name__ == '__main__':
    main()
