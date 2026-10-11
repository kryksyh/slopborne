#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Finds the game's GCN shader programs in its own files: the shader archives
(dvdroot_ps4/shader/*.shaderbnd.dcx: zlib-compressed BND4 containers) and the engine's own
shaders in eboot.bin (stored uncompressed). Identifies them by the hash the renderer uses at
run time (the OrbShdr binary info's shader_hash).

Usage: game_shaders.py <game folder> [--list] [--out <dir>]  (--out writes <hash>.bin files)
"""
import argparse
from pathlib import Path
import struct
import zlib

SIGNATURE = b'OrbShdr'
MOV_VCC_HI = 0xBEEB03FF  # first instruction of programs whose header says where the info is


def unpack_dcx(data):
    """DCX with DFLT compression: one zlib stream after the header."""
    if data[:4] != b'DCX\0' or data[0x28:0x2C] != b'DFLT':
        raise ValueError('not a DFLT DCX archive')
    start = data.find(b'\x78\xda', 0x2C)
    return zlib.decompress(data[start:])


def programs(blob):
    """(shader_hash, code bytes) for every program in an archive's contents. A program starts
    with s_mov_b32 vcc_hi, #imm whose immediate gives the distance to its OrbShdr info:
    found by searching back from the info (constants may sit between the code and the info)."""
    found = {}
    at = blob.find(SIGNATURE)
    while at >= 0:
        if len(blob) >= at + 28:
            bits, = struct.unpack_from('<I', blob, at + 8)
            length = bits >> 8
            shader_hash, = struct.unpack_from('<Q', blob, at + 16)
            start = at - 8
            limit = max(0, at - (1 << 22))
            while start >= limit:
                first, second = struct.unpack_from('<II', blob, start)
                if first == MOV_VCC_HI and start + (second + 1) * 8 == at:
                    break
                start -= 4
            if start >= limit and length and length % 4 == 0 and start + length <= at:
                found.setdefault(shader_hash, blob[start:start + length])
        at = blob.find(SIGNATURE, at + 1)
    return found


def game_programs(game):
    result = {name: ('eboot.bin', code)
              for name, code in programs((Path(game) / 'eboot.bin').read_bytes()).items()}
    for archive in sorted((Path(game) / 'dvdroot_ps4' / 'shader').glob('*.shaderbnd.dcx')):
        for shader_hash, code in programs(unpack_dcx(archive.read_bytes())).items():
            result.setdefault(shader_hash, (archive.name, code))
    return result


def main():
    p = argparse.ArgumentParser()
    p.add_argument('game', type=Path)
    p.add_argument('--list', action='store_true')
    p.add_argument('--out', type=Path)
    a = p.parse_args()
    found = game_programs(a.game)
    if a.out:
        a.out.mkdir(parents=True, exist_ok=True)
        for shader_hash, (_, code) in found.items():
            (a.out / f'{shader_hash:#018x}.bin').write_bytes(code)
    if a.list:
        for shader_hash, (archive, code) in sorted(found.items()):
            print(f'{shader_hash:#018x} {len(code):7d} {archive}')
    print(f'{len(found)} shader programs')


if __name__ == '__main__':
    main()
