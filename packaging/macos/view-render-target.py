#!/usr/bin/env python3
"""Decode format-tagged renderer readbacks to PNG; retain the original raw data.

HDR previews use per-channel Reinhard mapping followed by sRGB encoding. Statistics
refer to the original linear data, so bright values and NaNs remain diagnosable.
Requires numpy. Usage: view-render-target.py captures/image_*.raw
"""
import argparse
import json
from pathlib import Path
import re
import struct
import zlib

import numpy as np


def unpack_ufloat(bits, mantissa_bits):
    mantissa = bits & ((1 << mantissa_bits) - 1)
    exponent = bits >> mantissa_bits
    value = np.ldexp(1.0 + mantissa / (1 << mantissa_bits), exponent.astype('i4') - 15)
    value = np.where(exponent == 0, np.ldexp(mantissa.astype('f4'), 1 - 15 - mantissa_bits), value)
    return np.where(exponent == 31, np.where(mantissa == 0, np.inf, np.nan), value)


def decode(path):
    match = re.search(r'_(R8G8B8A8Unorm|R8G8B8A8Srgb|B10G11R11UfloatPack32|R16G16B16A16Sfloat)_\d+_(\d+)x(\d+)\.raw$', path.name)
    if not match:
        raise ValueError(f'Unsupported readback format: {path.name}')
    fmt, width, height = match.groups()
    width, height = int(width), int(height)
    hdr = fmt in ('B10G11R11UfloatPack32', 'R16G16B16A16Sfloat')
    if fmt == 'R16G16B16A16Sfloat':
        rgb = np.fromfile(path, dtype='<f2').reshape(height, width, 4)[..., :3].astype('f4')
    elif fmt == 'B10G11R11UfloatPack32':
        packed = np.fromfile(path, dtype='<u4').reshape(height, width)
        rgb = np.stack([unpack_ufloat(packed & 2047, 6),
                        unpack_ufloat((packed >> 11) & 2047, 6),
                        unpack_ufloat(packed >> 22, 5)], axis=-1)
    else:
        rgb = np.fromfile(path, dtype='u1').reshape(height, width, 4)[..., :3] / 255.0
    stats = dict(file=str(path), format=fmt, nan=int(np.isnan(rgb).sum()),
                 inf=int(np.isinf(rgb).sum()), negative=int((rgb < 0).sum()),
                 above_one=int((rgb > 1).sum()))
    rgb = np.nan_to_num(rgb, nan=0, posinf=65504, neginf=0).clip(0)
    if hdr:
        rgb = rgb / (1 + rgb)
        rgb = np.where(rgb <= 0.0031308, 12.92 * rgb, 1.055 * rgb ** (1 / 2.4) - 0.055)
    pixels = np.round(rgb.clip(0, 1) * 255).astype('u1')

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    rows = b''.join(b'\0' + row.tobytes() for row in pixels)
    png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))
    output = path.with_suffix('.preview.png')
    output.write_bytes(png)
    stats['preview'] = str(output)
    return stats


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('files', type=Path, nargs='+')
    args = parser.parse_args()
    for path in args.files:
        print(json.dumps(decode(path)))
