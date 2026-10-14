#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate Bloodborne's actual face-button textures in a retained asset view.

Original game/mod files are read-only. Only the four KG face textures and the
two dialog icons in MENU_Common_00091 are changed. No external imaging library
is needed: small vector glyphs are rasterized and encoded as BC7 mode 6.
TPF layout reference: SoulsFormatsNEXT, Formats/TPF/TPF.cs.
BC7 reference: Microsoft, BC7 Format Mode Reference (mode 6).
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib

from mods import make_link

VERSION = 1
PS = ('Cross', 'Circle', 'Square', 'Triangle')
NAMES = ('KG_Cancel', 'KG_OK', 'KG_R_L', 'KG_R_U')
ALLOWED = {*PS, 'A', 'B', 'X', 'Y', 'Bottom', 'Right', 'Left', 'Top'}
WEIGHTS = (0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64)


def unpack(data):
    if len(data) < 76 or data[:4] != b'DCX\0' or data[40:44] != b'DFLT':
        raise ValueError('Unsupported common.tpf.dcx compression; original glyphs retained')
    if data[68:72] != b'DCA\0' or data[72:76] != b'\0\0\0\x08':
        raise ValueError('Unsupported DCX header')
    size, compressed = struct.unpack_from('>II', data, 28)
    if size > 128 * 1024 * 1024 or compressed != len(data) - 76:
        raise ValueError('Invalid DCX size')
    decoder = zlib.decompressobj()
    raw = decoder.decompress(data[76:], size + 1)
    if len(raw) != size or not decoder.eof or decoder.unused_data:
        raise ValueError('Invalid DCX stream')
    return bytearray(raw)


def textures(raw):
    if raw[:4] != b'TPF\0' or raw[12:16] != bytes((4, 3, 1, 0)):
        raise ValueError('Unsupported TPF platform/encoding')
    count, = struct.unpack_from('<I', raw, 8)
    if not 1 <= count <= 1024 or 16 + count * 36 > len(raw):
        raise ValueError('Invalid texture count')
    result = {}
    for i in range(count):
        fields = struct.unpack_from('<IIBBBBHHIIIII', raw, 16 + i * 36)
        off, size, fmt, kind, mips, flags, w, h, layers, tile, name, extra, dxgi = fields
        if extra or off + size > len(raw) or name < 16 + count * 36 or name >= len(raw):
            raise ValueError('Unsupported or malformed TPF texture')
        end = name
        while end + 2 <= len(raw) and end - name < 256 and raw[end:end+2] != b'\0\0':
            end += 2
        if raw[end:end+2] != b'\0\0':
            raise ValueError('Invalid texture name')
        label = raw[name:end].decode('utf-16le')
        if label in result:
            raise ValueError('Duplicate texture name')
        result[label] = dict(offset=off, size=size, format=fmt, kind=kind, mips=mips,
                             flags=flags, width=w, height=h, layers=layers, tile=tile, dxgi=dxgi)
    return result


def block_offset(x, y, width):
    """PS4 BC7 block address: 8x8-block Morton microtiles."""
    morton = sum(((x >> j) & 1) << (2*j) | ((y >> j) & 1) << (2*j+1) for j in range(3))
    return (((y // 8) * ((width // 4 + 7) // 8) + x // 8) * 64 + morton) * 16


def bc7_alpha(alpha):
    """One antialiased white glyph block, transparent -> opaque mode-6 endpoints.

    Endpoint P-bits are shared across RGBA. RGB therefore ranges 234..235 while
    alpha spans exactly 0..255. Flip endpoints when the anchor's high bit is set.
    """
    assert len(alpha) == 16
    indices = [min(range(16), key=lambda i: abs(((WEIGHTS[i]*255+32)//64)-a)) for a in alpha]
    flip = indices[0] >= 8
    if flip:
        indices = [15-i for i in indices]
    bits, used = 64, 7
    def put(value, count):
        nonlocal bits, used
        bits |= value << used
        used += count
    for _ in range(3):
        put(117, 7)
        put(117, 7)
    put(127 if flip else 0, 7)
    put(0 if flip else 127, 7)
    put(1 if flip else 0, 1)
    put(0 if flip else 1, 1)
    for i, value in enumerate(indices):
        put(value, 3 if i == 0 else 4)
    assert used == 128
    return bits.to_bytes(16, 'little')


def glyph_alpha(label):
    # Hand-drawn vector letter strokes in a normalized 12x16 central area.
    paths = {
        'A': [[(0, 1), (.5, 0), (1, 1)], [(.22, .58), (.78, .58)]],
        'B': [[(0, 1), (0, 0), (.58, 0), (.88, .12), (1, .25), (.85, .4),
               (.55, .48), (0, .48)], [(.55, .48), (.9, .6), (1, .76),
                                      (.85, .94), (.58, 1), (0, 1)]],
        'X': [[(0, 0), (1, 1)], [(1, 0), (0, 1)]],
        'Y': [[(0, 0), (.5, .48), (1, 0)], [(.5, .48), (.5, 1)]],
        'Top': [[(.5, 1), (.5, 0)], [(0, .4), (.5, 0), (1, .4)]],
    }
    if label in ('Bottom', 'Left', 'Right'):
        paths[label] = []
        for path in paths['Top']:
            if label == 'Bottom': path = [(x, 1-y) for x, y in path]
            elif label == 'Left': path = [(y, x) for x, y in path]
            else: path = [(1-y, x) for x, y in path]
            paths[label].append(path)
    segments = []
    for path in paths[label]:
        points = [(10 + x*12, 8 + y*16) for x, y in path]
        segments.extend(zip(points, points[1:]))
    def distance(x, y, a, b):
        dx, dy = b[0]-a[0], b[1]-a[1]
        t = max(0, min(1, ((x-a[0])*dx+(y-a[1])*dy)/(dx*dx+dy*dy)))
        return math.hypot(x-a[0]-t*dx, y-a[1]-t*dy)
    alpha = []
    for y in range(32):
        for x in range(32):
            covered = 0
            for sy in range(4):
                for sx in range(4):
                    px, py = x+(sx+.5)/4, y+(sy+.5)/4
                    on = abs(math.hypot(px-16, py-16)-13) < .8
                    if not on: on = any(distance(px, py, a, b) < .95 for a, b in segments)
                    covered += on
            alpha.append(round(covered*255/16))
    return alpha


def glyph_blocks(label):
    alpha = glyph_alpha(label)
    return [bc7_alpha([alpha[(by*4+y)*32+bx*4+x] for y in range(4) for x in range(4)])
            for by in range(8) for bx in range(8)]


def patch_archive(data, labels):
    if len(labels) != 4 or any(label not in ALLOWED for label in labels):
        raise ValueError('Expected four supported physical face-button labels')
    raw = unpack(data)
    entries = textures(raw)
    for name, w, h in [(n, 32, 32) for n in NAMES] + [('MENU_Common_00091', 1024, 512)]:
        t = entries.get(name)
        if not t or (t['width'], t['height'], t['format'], t['dxgi'], t['mips'],
                     t['layers'], t['kind'], t['flags'], t['tile'], t['size']) != (
                         w, h, 102, 98, 1, 1, 0, 0, 13, w*h):
            raise ValueError(f'Unsupported prompt texture {name}; original assets retained')
    original = bytes(raw)
    blocks = {}
    for label in set(labels):
        if label in PS:
            off = entries[NAMES[PS.index(label)]]['offset']
            blocks[label] = [original[off+block_offset(x, y, 32):off+block_offset(x, y, 32)+16]
                             for y in range(8) for x in range(8)]
        else:
            blocks[label] = glyph_blocks(label)
    changed = []
    def write(name, label, x=0, y=0):
        t = entries[name]
        for by in range(8):
            for bx in range(8):
                off = t['offset'] + block_offset(x//4+bx, y//4+by, t['width'])
                raw[off:off+16] = blocks[label][by*8+bx]
                changed.append((off, off+16))
    for name, label in zip(NAMES, labels):
        write(name, label)
    # Separate hard-wired dialog sprites. Other atlas blocks remain byte-exact.
    write('MENU_Common_00091', labels[1], 660, 120)
    write('MENU_Common_00091', labels[0], 692, 120)
    packed = zlib.compress(raw, 9)
    header = bytearray(data[:76])
    struct.pack_into('>II', header, 28, len(raw), len(packed))
    return bytes(header)+packed, changed


def asset_view(game, target, out):
    """Construct the three edited directory levels directly: no unlink/cleanup."""
    game = Path(game).resolve()
    view = Path(tempfile.mkdtemp(prefix='glyph-game-', dir=out))
    def level(source, destination, parts):
        for entry in source.iterdir():
            link = destination / entry.name
            if entry.name == parts[0]:
                if len(parts) == 1:
                    make_link(link, target)
                else:
                    link.mkdir()
                    level(entry, link, parts[1:])
            else:
                make_link(link, entry)
    level(game, view, ('dvdroot_ps4', 'menu', 'common.tpf.dcx'))
    return view


def build(game, out, labels):
    game, out = Path(game).resolve(), Path(out).resolve()
    if tuple(labels) == PS:
        return game
    source = game/'dvdroot_ps4/menu/common.tpf.dcx'
    data = source.read_bytes()
    key = hashlib.sha256(data + ','.join(labels).encode() + str(VERSION).encode()).hexdigest()
    folder = out/'button-glyphs'/key
    target = folder/'common.tpf.dcx'
    if not target.is_file():
        patched, _ = patch_archive(data, labels)
        folder.mkdir(parents=True, exist_ok=True)
        with target.open('xb') as f: f.write(patched)
        (folder/'manifest.json').write_text(json.dumps(dict(version=VERSION, labels=labels,
            source_sha256=hashlib.sha256(data).hexdigest(), output_sha256=hashlib.sha256(patched).hexdigest()), indent=2)+'\n')
    out.mkdir(parents=True, exist_ok=True)
    return asset_view(game, target, out)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('game', type=Path)
    p.add_argument('--out', type=Path, required=True)
    selection = p.add_mutually_exclusive_group(required=True)
    selection.add_argument('--labels')
    selection.add_argument('--helper', type=Path)
    a = p.parse_args()
    labels = a.labels
    if a.helper:
        result = subprocess.run([str(a.helper.resolve())], capture_output=True, text=True,
                                timeout=10, check=True)
        labels = next(line.split('=', 1)[1] for line in result.stdout.splitlines()
                      if line.startswith('BB_CONTROLLER_LABELS='))
    view = build(a.game, a.out, labels.split(','))
    print(f'Controller glyphs: {labels} (original game files retained)', file=sys.stderr)
    print(view)


if __name__ == '__main__':
    main()
