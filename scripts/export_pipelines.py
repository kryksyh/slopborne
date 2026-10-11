#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exports the pipeline list the macOS app ships from a pipeline cache built by playing:
pipeline keys, shader specializations and the fetch shaders the game generated, never SPIR-V
(the game rebuilds it from the player's own game files). Pipelines it cannot rebuild (a vertex
shader permutation whose fetch shader was never stored) are skipped by the game at start.
Usage: export_pipelines.py <cache dir, e.g. user/cache/CUSA03173> <out dir>"""
import argparse
from pathlib import Path
import shutil


def main():
    p = argparse.ArgumentParser()
    p.add_argument('cache', type=Path)
    p.add_argument('out', type=Path)
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    counts = {'.key': 0, '.meta': 0, '.fetch': 0}
    for file in sorted(a.cache.iterdir()):
        if file.suffix in counts:
            shutil.copy2(file, a.out / file.name)
            counts[file.suffix] += 1
    print(f"{counts['.key']} pipelines, {counts['.meta']} shader permutations, "
          f"{counts['.fetch']} fetch shaders -> {a.out}")


if __name__ == '__main__':
    main()
