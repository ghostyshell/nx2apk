#!/usr/bin/env python3
# Xbox button prompts: patch files (dm/android/texpatch.c) that paste Xbox A / B / X / Y buttons over the game's Switch button pictures.
# The buttons are Kenney's Input Prompts (CC0, https://kenney.nl/assets/input-prompts), kept in dm/third_party/kenney_input_prompts; the game's textures never leave
# the phone: a patch only holds the Xbox picture and where it goes (the game profile's prompts.tsv), and the phone blends it over the texture as it is uploaded.
# Usage: prompts.py <prompts.tsv> <out dir>          Standard library only.
import os, struct, sys, zlib

ICONS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../third_party/kenney_input_prompts/xbox_button_color_{}.png')
SCALE = 1.25   # the Xbox button's diameter over the Switch button's (covers its glow)

def png_rgba(data):
    """8-bit PNG (palette, grey, RGB or RGBA, non-interlaced) → (w, h, rows of RGBA bytes)"""
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    i, idat, pal, trns = 8, b'', None, b''
    while i < len(data):
        n, t = struct.unpack('>I4s', data[i:i + 8]); c = data[i + 8:i + 8 + n]; i += 12 + n
        if t == b'IHDR': w, h, bd, ct, _, _, il = struct.unpack('>IIBBBBB', c)
        elif t == b'PLTE': pal = c
        elif t == b'tRNS': trns = c
        elif t == b'IDAT': idat += c
    assert bd == 8 and il == 0, 'only 8-bit non-interlaced PNGs'
    bpp = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ct]; raw = zlib.decompress(idat); stride = w * bpp
    rows, prev, p = [], bytearray(stride), 0
    for _ in range(h):
        f = raw[p]; line = bytearray(raw[p + 1:p + 1 + stride]); p += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0; b = prev[x]; c = prev[x - bpp] if x >= bpp else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line; out = bytearray()
        for x in range(w):
            if ct == 3: k = line[x]; out += pal[3 * k:3 * k + 3] + bytes([trns[k] if k < len(trns) else 255])
            elif ct == 6: out += line[4 * x:4 * x + 4]
            elif ct == 2: out += line[3 * x:3 * x + 3] + b'\xff'
            elif ct == 0: out += bytes([line[x]] * 3) + b'\xff'
            else: out += bytes([line[2 * x]] * 3) + bytes([line[2 * x + 1]])
        rows.append(out)
    return w, h, rows

def resize(w, h, rows, n):
    """box-filter the picture to n x n, premultiplied so transparent edges stay clean"""
    out = []
    for y in range(n):
        y0, y1 = y * h / n, (y + 1) * h / n; line = bytearray()
        for x in range(n):
            x0, x1 = x * w / n, (x + 1) * w / n; acc = [0.0] * 4; tot = 0.0
            for sy in range(int(y0), min(h, int(y1 + 0.999))):
                wy = min(y1, sy + 1) - max(y0, sy)
                for sx in range(int(x0), min(w, int(x1 + 0.999))):
                    wt = wy * (min(x1, sx + 1) - max(x0, sx)); px = rows[sy][4 * sx:4 * sx + 4]; a = px[3] / 255
                    acc[0] += px[0] * a * wt; acc[1] += px[1] * a * wt; acc[2] += px[2] * a * wt; acc[3] += a * wt; tot += wt
            a = acc[3] / tot if tot else 0
            line += bytes([round(acc[k] / acc[3]) if acc[3] else 0 for k in range(3)] + [round(a * 255)])
        out.append(bytes(line))
    return out

def main():
    tsv, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    icons = {k.upper(): png_rgba(open(ICONS.format(k), 'rb').read()) for k in 'abxy'}
    by_hash = {}
    for ln in open(tsv):
        if ln.startswith('#') or not ln.strip(): continue
        h, w, ht, k, x, y, r = ln.split('\t')[:7]
        by_hash.setdefault((h, int(w), int(ht)), (int(w), int(ht), []))[2].append((k, float(x), float(y), float(r)))
    sized = {}
    written = 0
    for (h, _, _), (w, ht, buttons) in by_hash.items():
        rects = []
        for k, x, y, r in buttons:
            n = max(8, round(2 * r * SCALE))
            if (k, n) not in sized: sized[(k, n)] = resize(*icons[k], n)
            x0, y0 = max(0, round(x - n / 2)), max(0, round(y - n / 2))
            if x0 + n > w or y0 + n > ht: print(f'skipped {k} at {x:.0f},{y:.0f} in {h}: outside the texture'); continue
            rects.append((x0, y0, n, n, b''.join(sized[(k, n)])))
        with open(os.path.join(out, f'{h}_L0_{w}x{ht}.patch'), 'wb') as f:
            f.write(b'DMTP' + struct.pack('<I', len(rects)))
            for x0, y0, rw, rh, px in rects: f.write(struct.pack('<4I', x0, y0, rw, rh) + px)
        written += len(rects)
    print(f'Xbox prompts: {written} buttons in {len(by_hash)} textures')

main()
