#!/usr/bin/env python3
# Subspace: blank (fully transparent) replacements for parts of Brawl's versus fight HUD
# (info2/info_en.pac), loaded only in an online session and only as the player's NetPlay settings
# say (VideoCommon/HiresTextures.cpp):
#   - the fighter names (MenSelchrChrNmS.*) -> Data/Sys/Subspace/HudNames/RSBE01, when player names
#     replace them;
#   - the bar under them (InfFaceLine00) -> Data/Sys/Subspace/HudBar/RSBE01, when the port-coloured
#     stroke replaces it.
# Only blank images are written: nothing from the game's art.
#
# The names are Dolphin's (VideoCommon/TextureInfo.cpp CalculateTextureName): the textures have no
# palette, so a name is tex1_<w>x<h>_<XXH64 of the texture's data, seed 0>_<format>.
#
#   python3 Tools/orca/textures/sd-make-blank.py <disc>/DATA/files/info2/info_en.pac
import argparse
import os
import re
import struct
import sys
import zlib

M = (1 << 64) - 1
P1, P2, P3, P4, P5 = (0x9E3779B185EBCA87, 0xC2B2AE3D27D4EB4F, 0x165667B19E3779F9,
                      0x85EBCA77C2B2AE63, 0x27D4EB2F165667C5)


def rotl(x, r):
    return ((x << r) | (x >> (64 - r))) & M


def xxh64(data, seed=0):
    n = len(data)
    i = 0
    if n >= 32:
        v = [(seed + P1 + P2) & M, (seed + P2) & M, seed & M, (seed - P1) & M]
        while i + 32 <= n:
            for k in range(4):
                lane = struct.unpack_from('<Q', data, i + 8 * k)[0]
                v[k] = (rotl((v[k] + lane * P2) & M, 31) * P1) & M
            i += 32
        h = (rotl(v[0], 1) + rotl(v[1], 7) + rotl(v[2], 12) + rotl(v[3], 18)) & M
        for k in range(4):
            h ^= (rotl((v[k] * P2) & M, 31) * P1) & M
            h = (h * P1 + P4) & M
    else:
        h = (seed + P5) & M
    h = (h + n) & M
    while i + 8 <= n:
        k1 = (rotl((struct.unpack_from('<Q', data, i)[0] * P2) & M, 31) * P1) & M
        h = (rotl(h ^ k1, 27) * P1 + P4) & M
        i += 8
    if i + 4 <= n:
        h = (rotl(h ^ ((struct.unpack_from('<I', data, i)[0] * P1) & M), 23) * P2 + P3) & M
        i += 4
    while i < n:
        h = (rotl(h ^ ((data[i] * P5) & M), 11) * P1) & M
        i += 1
    h ^= h >> 33
    h = (h * P2) & M
    h ^= h >> 29
    h = (h * P3) & M
    h ^= h >> 32
    return h


def unlz77(src):
    # Nintendo LZ77 (type 0x10), as the .pac is stored on the disc.
    if src[:1] != b'\x10':
        return src
    size = src[1] | src[2] << 8 | src[3] << 16
    i, out = 4, bytearray()
    while len(out) < size:
        flags = src[i]
        i += 1
        for b in range(8):
            if len(out) >= size:
                break
            if flags & (0x80 >> b):
                a, c = src[i], src[i + 1]
                i += 2
                for _ in range((a >> 4) + 3):
                    out.append(out[-(((a & 0xF) << 8 | c) + 1)])
            else:
                out.append(src[i])
                i += 1
    return bytes(out)


def blank_png(w, h):
    def chunk(kind, body):
        return (struct.pack('>I', len(body)) + kind + body +
                struct.pack('>I', zlib.crc32(kind + body) & 0xFFFFFFFF))
    rows = b''.join(b'\0' + b'\0' * (4 * w) for _ in range(h))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b''))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument('pac')
    sys_orca = os.path.join(here, '..', '..', '..', 'Data', 'Sys', 'Orca')
    ap.add_argument('--names-out', default=os.path.join(sys_orca, 'HudNames', 'RSBE01'))
    ap.add_argument('--bar-out', default=os.path.join(sys_orca, 'HudBar', 'RSBE01'))
    args = ap.parse_args()
    d = unlz77(open(args.pac, 'rb').read())
    # Texel blocks (width, height) and bits per texel by GX format: I4, I8, IA4, IA8.
    blocks = {0: (8, 8, 4), 1: (8, 4, 8), 2: (8, 4, 8), 3: (4, 4, 16)}
    found = {}
    for m in re.finditer(b'TEX0', d):
        o = m.start()
        try:
            data_off, name_off = struct.unpack_from('>ii', d, o + 0x10)
            name = d[o + name_off:o + name_off + 64].split(b'\0')[0].decode('ascii')
            w, h, fmt = struct.unpack_from('>HHI', d, o + 0x1C)
        except (struct.error, UnicodeDecodeError):
            continue
        if name.startswith('MenSelchrChrNmS.'):
            # .000 is a 4x4 placeholder, not a name; other textures may share its data.
            if name == 'MenSelchrChrNmS.000':
                continue
            out = args.names_out
        elif name == 'InfFaceLine00':
            out = args.bar_out
        else:
            continue
        if fmt not in blocks or not w or not h:
            continue
        bw, bh, bits = blocks[fmt]
        # The base level only, in whole blocks.
        size = ((w + bw - 1) // bw * bw) * ((h + bh - 1) // bh * bh) * bits // 8
        stem = f'tex1_{w}x{h}_{xxh64(d[o + data_off:o + data_off + size]):016x}_{fmt}'
        found[stem] = (name, w, h, out)
    if not found:
        sys.exit('no textures found')
    for stem, (name, w, h, out) in sorted(found.items()):
        os.makedirs(out, exist_ok=True)
        with open(os.path.join(out, stem + '.png'), 'wb') as f:
            f.write(blank_png(w, h))
        print(f'{os.path.basename(os.path.dirname(os.path.normpath(out)))}/{stem}.png  {name}')
    print(f'{len(found)} written')


if __name__ == '__main__':
    main()
