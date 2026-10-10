#!/usr/bin/env python3
"""Control a diagnostic run through BB_PAD_FILE and capture its rendered output.

Launch with BB_PAD_FILE=DIR/pad.txt, BB_FRAME_DUMP_SEQUENCE=DIR/capture-sequence.txt,
and BB_DUMP_DIR=DIR/captures. Captures read the renderer, not the desktop.
"""
import argparse
import pathlib
import re
import struct
import time
import zlib


def capture(directory):
    sequence = directory / 'capture-sequence.txt'
    captures = directory / 'captures'
    previous = set(captures.glob('output_*.raw'))
    sequence.write_text(str(int(sequence.read_text() or '0') + 1))
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        for path in set(captures.glob('output_*.raw')) - previous:
            width, height = map(int, re.search(r'_(\d+)x(\d+)\.raw$', path.name).groups())
            if path.stat().st_size != width * height * 4:
                continue
            pixels = bytearray(path.read_bytes())
            pixels[0::4], pixels[2::4] = pixels[2::4], pixels[0::4]
            def chunk(kind, data):
                return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
            rows = b''.join(b'\0' + pixels[y*width*4:(y+1)*width*4] for y in range(height))
            png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0))
                   + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))
            output = path.with_suffix('.png')
            output.write_bytes(png)
            print(output.resolve(), flush=True)
            return
        time.sleep(0.1)
    raise SystemExit('No completed frame capture within 20 seconds; inspect the game log.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=pathlib.Path)
    parser.add_argument('action', choices=['press', 'capture', 'release', 'spin'])
    parser.add_argument('buttons', nargs='*')
    parser.add_argument('--seconds', type=float, default=0.25)
    args = parser.parse_args()
    pad = args.directory / 'pad.txt'
    if args.action == 'capture':
        capture(args.directory)
    elif args.action == 'release':
        pad.write_text('')
    else:
        controls = 'rx=220 ry=128 lx=128 ly=128' if args.action == 'spin' else ' '.join(args.buttons)
        try:
            pad.write_text(controls)
            time.sleep(max(0, args.seconds))
        finally:
            pad.write_text('')


if __name__ == '__main__':
    main()
