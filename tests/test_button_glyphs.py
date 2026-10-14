"""Synthetic archive tests; no copyrighted game assets required or removed."""
from paths import ROOT
import contextlib
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


# Test folders go under out/ (kept for inspection), which a fresh checkout lacks.
(ROOT / 'out').mkdir(exist_ok=True)

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


    def test_launcher_cleanup_removes_link_views_and_keeps_the_game(self):
        """Outside macOS the launcher deletes the mod and glyph views after the game. They are
        links into the game folder (on Windows without Developer Mode: junctions and hard
        links), so the real rmtree must remove them without touching a single target."""
        root = Path(tempfile.mkdtemp(prefix='glyph-cleanup-test-', dir=ROOT / 'out'))
        game = root / 'game'
        files = {'eboot.bin': b'eboot', 'dvdroot_ps4/menu/common.tpf.dcx': b'menu',
                 'dvdroot_ps4/menu/other.gfx': b'gfx', 'dvdroot_ps4/map/m10.bin': b'map'}
        for name, data in files.items():
            (game / name).parent.mkdir(parents=True, exist_ok=True)
            (game / name).write_bytes(data)
        patched = root / 'patched.dcx'
        patched.write_bytes(b'patched')
        # Windows: the fallback links (junctions, hard links), as without Developer Mode.
        no_symlinks = (patch.object(Path, 'symlink_to', side_effect=OSError('no symlink privilege'))
                       if os.name == 'nt' else contextlib.nullcontext())
        with no_symlinks:
            mod_view = root / 'mod-view'
            mod_view.mkdir()
            for entry in game.iterdir():
                glyphs.make_link(mod_view / entry.name, entry)
            glyph_view = glyphs.asset_view(mod_view, patched, root)
        if os.name == 'nt':
            self.assertFalse((mod_view / 'dvdroot_ps4').is_symlink())  # a junction
            self.assertEqual(os.stat(game / 'eboot.bin').st_nlink, 3)  # game, mod and glyph views
        probe = root / 'bb-probe'
        probe.touch()
        (root / ('bb-controller-info' + launcher.EXE)).touch()
        views = {'mods.py': str(mod_view), 'button_glyphs.py': str(glyph_view)}

        def script(name, *args, **kwargs):
            return views.get(name, '')
        env = dict(BB_DATA_DIR=str(root / 'data'), BB_PROBE=str(probe), BB_GAME_DIR=str(game),
                   BB_FORCE_PATCHES='1')
        with patch.dict(os.environ, env, clear=True), \
             patch.object(launcher.sys, 'platform', 'win32' if os.name == 'nt' else 'linux'), \
             patch.object(launcher.sys, 'argv', ['run.py']), \
             patch.object(launcher.os, 'chdir'), \
             patch.object(launcher, 'run_script', side_effect=script), \
             patch.object(launcher, 'install_pipeline_list'), \
             patch.object(launcher.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0)) as run:
            self.assertEqual(launcher.main(), 0)
            command = run.call_args.args[0]
            self.assertEqual(command[command.index('--app0') + 1], str(glyph_view))
        self.assertFalse(glyph_view.exists())
        self.assertFalse(mod_view.exists())
        for name, data in files.items():
            self.assertEqual((game / name).read_bytes(), data, name)
        self.assertEqual(patched.read_bytes(), b'patched')


if __name__ == '__main__':
    unittest.main()
