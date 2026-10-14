"""Synthetic archive tests; no copyrighted game assets required or removed."""
from paths import ROOT
import importlib.util
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import zlib


def module(name, file):
    spec = importlib.util.spec_from_file_location(name, ROOT / file)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


glyphs = module('button_glyphs', 'scripts/button_glyphs.py')
launcher = module('bb_run', 'run.py')


def pack(raw):
    compressed = zlib.compress(raw)
    header = bytearray(76)
    header[:4], header[40:44], header[68:72] = b'DCX\0', b'DFLT', b'DCA\0'
    struct.pack_into('>II', header, 28, len(raw), len(compressed))
    struct.pack_into('>I', header, 72, 8)
    return bytes(header) + compressed


def fixture():
    names = (*glyphs.NAMES, 'MENU_Common_00091', 'UNTOUCHED')
    raw = bytearray(16 + 36 * len(names))
    raw[:4], raw[12:16] = b'TPF\0', bytes((4, 3, 1, 0))
    struct.pack_into('<I', raw, 8, len(names))
    for i, name in enumerate(names):
        name_off = len(raw)
        raw.extend(name.encode('utf-16le') + b'\0\0')
        w, h = (1024, 512) if i == 4 else (32, 32)
        offset = len(raw)
        raw.extend(bytes([30 + i]) * (w * h))
        struct.pack_into('<IIBBBBHHIIIII', raw, 16 + i * 36,
                         offset, w*h, 102, 0, 1, 0, w, h, 1, 13, name_off, 0, 98)
    return pack(raw)


class GlyphTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = fixture()
        cls.patched, cls.ranges = glyphs.patch_archive(cls.source, ('B', 'A', 'Y', 'X'))

    def test_only_prompt_blocks_change(self):
        old, new = glyphs.unpack(self.source), glyphs.unpack(self.patched)
        self.assertEqual(len(old), len(new))
        self.assertEqual(len(self.ranges), 384)
        for start, end in self.ranges:
            new[start:end] = old[start:end]
        self.assertEqual(new, old)

    def test_playstation_permutation_copies_original_glyphs(self):
        original = glyphs.unpack(self.source)
        entries = glyphs.textures(original)
        changed, _ = glyphs.patch_archive(self.source, tuple(reversed(glyphs.PS)))
        changed = glyphs.unpack(changed)
        for source, target in zip(reversed(glyphs.NAMES), glyphs.NAMES):
            a, b = entries[source]['offset'], entries[target]['offset']
            self.assertEqual(original[a:a+1024], changed[b:b+1024])

    def test_rejects_unknown_or_truncated_archive(self):
        for data in (b'', self.source[:-1], b'ZZZ' + self.source[3:]):
            with self.subTest(size=len(data)), self.assertRaises(ValueError):
                glyphs.patch_archive(data, ('B', 'A', 'Y', 'X'))
        raw = glyphs.unpack(self.source)
        struct.pack_into('<H', raw, 16 + 12, 64)  # incompatible glyph width
        with self.assertRaises(ValueError):
            glyphs.patch_archive(pack(raw), ('B', 'A', 'Y', 'X'))

    def test_retained_view_preserves_source_and_siblings(self):
        root = Path(tempfile.mkdtemp(prefix='glyph-test-', dir=ROOT / 'out'))
        game = root / 'game'
        menu = game / 'dvdroot_ps4/menu'
        menu.mkdir(parents=True)
        (game / 'eboot.bin').write_bytes(b'original executable')
        (menu / 'other.file').write_bytes(b'original sibling')
        source = menu / 'common.tpf.dcx'
        source.write_bytes(self.source)
        with patch.object(Path, 'unlink', side_effect=AssertionError('No deletion')):
            view = glyphs.build(game, root / 'views', ('B', 'A', 'Y', 'X'))
            again = glyphs.build(game, root / 'views', ('B', 'A', 'Y', 'X'))
        self.assertNotEqual(view, again)
        self.assertTrue(view.exists())
        self.assertEqual(source.read_bytes(), self.source)
        self.assertEqual((view / 'dvdroot_ps4/menu/common.tpf.dcx').read_bytes(), self.patched)
        self.assertEqual((view / 'dvdroot_ps4/menu/other.file').read_bytes(), b'original sibling')
        self.assertEqual((view / 'eboot.bin').read_bytes(), b'original executable')
        self.assertEqual(glyphs.build(game, root / 'unused', glyphs.PS), game)

    def test_optional_script_failure_can_fall_back(self):
        failed = subprocess.CompletedProcess(['script'], 1, '')
        with patch.object(launcher.subprocess, 'run', return_value=failed):
            with self.assertRaises(subprocess.CalledProcessError):
                launcher.run_script('button_glyphs.py', capture=True, optional=True)
            with self.assertRaises(SystemExit):
                launcher.run_script('prepare.py', capture=True)

    def test_launcher_uses_generated_view_and_retains_mod_view_on_failure(self):
        root = Path(tempfile.mkdtemp(prefix='glyph-launch-test-', dir=ROOT / 'out'))
        game, mods, generated = (root / name for name in ('game', 'mods', 'glyphs'))
        for folder in (game, mods, generated):
            folder.mkdir()
            (folder / 'eboot.bin').write_bytes(b'fixture')
        probe = root / 'bb-probe'
        probe.touch()
        (root / ('bb-controller-info' + launcher.EXE)).touch()
        for failed, expected in ((False, generated), (True, mods)):
            def script(name, *args, **kwargs):
                if name == 'mods.py':
                    return str(mods)
                if name == 'button_glyphs.py':
                    self.assertEqual(args[0], mods)
                    self.assertTrue(kwargs['optional'])
                    if failed:
                        raise subprocess.CalledProcessError(1, ['glyphs'])
                    return str(generated)
                return ''
            # BB_FORCE_PATCHES: no eboot.elf check (the fixture has no game code).
            env = dict(BB_DATA_DIR=str(root / 'data'), BB_PROBE=str(probe), BB_GAME_DIR=str(game),
                       BB_FORCE_PATCHES='1')
            with self.subTest(failed=failed), patch.dict(os.environ, env, clear=True), \
                 patch.object(launcher.sys, 'platform', 'darwin'), \
                 patch.object(launcher.sys, 'argv', ['run.py']), \
                 patch.object(launcher.os, 'chdir'), \
                 patch.object(launcher, 'run_script', side_effect=script), \
                 patch.object(launcher, 'install_pipeline_list'), \
                 patch.object(launcher.shutil, 'rmtree', side_effect=AssertionError('No deletion')), \
                 patch.object(launcher.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0)) as run:
                self.assertEqual(launcher.main(), 0)
                command = run.call_args.args[0]
                self.assertEqual(command[command.index('--app0') + 1], str(expected))
                self.assertTrue(mods.exists())


if __name__ == '__main__':
    unittest.main()
