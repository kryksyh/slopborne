"""CPU-only split benchmark lifecycle fixtures. All fixture files are retained."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
HARNESS = ROOT / 'packaging/macos/benchmark-run.py'
sys.path.insert(0, str(HARNESS.parent))
spec = importlib.util.spec_from_file_location('benchmark_run', HARNESS)
benchmark = importlib.util.module_from_spec(spec)
spec.loader.exec_module(benchmark)


@unittest.skipUnless(sys.platform == 'darwin' or sys.platform.startswith('linux'), 'POSIX fixtures')
class RendererLifecycleTest(unittest.TestCase):
    def setUp(self):
        parent = ROOT / 'out/macos/benchmark-renderer-tests'
        parent.mkdir(parents=True, exist_ok=True)
        self.directory = Path(tempfile.mkdtemp(dir=parent))
        self.children = []
        self.renderer = self.directory / 'bb-render'
        # Real executable identity checks, without compiling or running renderer code.
        # Homebrew's bin/python launcher can re-exec its framework executable in
        # descendants. Copy the native running image so identity stays the snapshot.
        self.native_python = Path(benchmark.process_identity(os.getpid())['path'])
        shutil.copy2(self.native_python, self.renderer)

    def tearDown(self):
        for child in self.children:
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()

    def launch(self, *args):
        child = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.children.append(child)
        return child

    def wait_until(self, condition):
        deadline = time.monotonic() + 5
        while not condition():
            self.assertLess(time.monotonic(), deadline, 'Fixture condition timed out')
            time.sleep(.02)

    def parent(self, lifetime=60):
        code = ('import subprocess,time; '
                f'subprocess.Popen([{str(self.renderer)!r}, "-c", "import time;time.sleep({lifetime})"]); '
                'time.sleep(60)')
        game = self.launch(sys.executable, '-c', code)
        tracker = benchmark.RendererTracker(game, self.renderer, self.directory)
        self.wait_until(lambda: (tracker.check(), tracker.renderer)[1])
        self.addCleanup(tracker.cleanup)
        return game, tracker

    def test_cleanup_only_run_child_after_parent_exit(self):
        unrelated = self.launch(str(self.renderer), '-c', 'import time;time.sleep(60)')
        game, tracker = self.parent()
        game.terminate()
        game.wait(timeout=3)
        tracker.cleanup()
        self.assertIn(tracker.cleanup_result, ('Renderer stopped', 'Renderer exited'))
        identity = benchmark.process_identity(tracker.renderer['pid'])
        self.assertTrue(identity is None or identity['zombie'])
        self.assertIsNone(unrelated.poll(), 'Same binary with a different parent must survive')

    def test_early_renderer_death_stops_controller_while_parent_lives(self):
        game, tracker = self.parent(lifetime=.3)
        started = time.monotonic()
        result = benchmark.run_controller([sys.executable, '-c', 'import time;time.sleep(60)'], game, tracker)
        self.assertNotEqual(result, 0)
        self.assertLess(time.monotonic() - started, 3)
        self.assertIn('Renderer exited', tracker.failure)
        self.assertIsNone(game.poll())

    def test_orphan_discovery_after_parent_exited_before_first_sample(self):
        child_pid_file = self.directory / 'child-pid'
        code = ('import subprocess;from pathlib import Path;'
                f'p=subprocess.Popen([{str(self.renderer)!r}, "-c", "import time;time.sleep(60)"]);'
                f'Path({str(child_pid_file)!r}).write_text(str(p.pid))')
        game = subprocess.Popen([sys.executable, '-c', code], start_new_session=True)
        self.children.append(game)
        game.wait(timeout=3)
        tracker = benchmark.RendererTracker(game, self.renderer, self.directory, isolated=True)
        self.addCleanup(tracker.cleanup)
        tracker.cleanup()
        self.assertEqual(tracker.renderer['pid'], int(child_pid_file.read_text()))
        self.assertEqual(tracker.renderer['ppid'], 1)
        self.assertIn(tracker.cleanup_result, ('Renderer stopped', 'Renderer exited'))
        self.assertIsNone(tracker.failure)

    def test_changed_identity_is_never_signalled(self):
        game, tracker = self.parent()
        original = tracker.renderer['start']
        tracker.renderer['start'] = 'a different process start identity'
        with patch.object(benchmark.os, 'kill', side_effect=AssertionError('must not signal')):
            tracker.cleanup()
        self.assertEqual(tracker.cleanup_result, 'Identity changed; refused to signal')
        tracker.renderer['start'] = original
        self.assertIsNone(game.poll())

    def run_harness(self, boot, camera_code='import time; time.sleep(60)\n', stop_after=True):
        for name in ('prepared', 'profile', 'game', 'save'):
            (self.directory / name).mkdir()
        (self.directory / 'prepared/boot-linked.bin').write_text(boot)
        for name in ('prepared/content.bin', 'profile/patches.bin', 'profile/bbport.ini', 'game/eboot.bin'):
            (self.directory / name).write_text('')
        (self.directory / 'save/fixture').write_text('preserve me')
        camera = self.directory / 'camera.py'
        camera.write_text(camera_code)
        loader = ('import importlib.util,sys;from pathlib import Path;'
                  f'sys.path.insert(0,{str(HARNESS.parent)!r});'
                  f's=importlib.util.spec_from_file_location("benchmark",{str(HARNESS)!r});'
                  'm=importlib.util.module_from_spec(s);s.loader.exec_module(m);'
                  f'm.CAMERA_SCRIPT=Path({str(camera)!r});sys.exit(m.main())')
        args = [sys.executable, '-c', loader, '--binary', str(self.native_python),
                '--render-binary', str(self.renderer), '--out', str(self.directory / 'runs')]
        if stop_after:
            args.append('--stop-after')
        for name in ('prepared', 'profile', 'game', 'save'):
            args.extend(['--' + name, str(self.directory / name)])
        started = time.monotonic()
        result = subprocess.run(args, capture_output=True, text=True, timeout=10)
        (self.directory / 'harness-output.txt').write_text(result.stdout + result.stderr)
        self.assertLess(time.monotonic() - started, 5)
        run, = (self.directory / 'runs').iterdir()
        return result, run

    def test_full_harness_snapshots_and_stop_after_does_not_hide_exit_three(self):
        result, run = self.run_harness('raise SystemExit(3)\n')
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        config = json.loads((run / 'configuration.json').read_text())
        self.assertEqual(config['command'][0], str(run / 'bb-probe'))
        self.assertEqual(config['environment']['BB_RENDER_BIN'], str(run / 'bb-render'))
        self.assertEqual(config['resource_metrics_scope'], 'guest process only; excludes native renderer')
        self.assertEqual((run / 'bb-render').read_bytes(), self.renderer.read_bytes())
        self.assertEqual((run / 'bb-probe').read_bytes(), self.native_python.read_bytes())
        failure = json.loads((run / 'process-failure.json').read_text())
        self.assertEqual(failure['exit_code'], 3)
        self.assertIn('exit 3', failure['renderer_failure'])
        self.assertEqual((self.directory / 'save/fixture').read_text(), 'preserve me')

    def test_full_harness_success_cleans_leftover_without_false_failure(self):
        boot = ('import os,subprocess,time\n'
                'child=subprocess.Popen([os.environ["BB_RENDER_BIN"], "-c", "import time;time.sleep(60)"])\n'
                'print("Fixture renderer PID:", child.pid, flush=True)\n'
                'time.sleep(60)\n')
        camera = ('import sys,time\nfrom pathlib import Path\n'
                  'deadline=time.monotonic()+4\n'
                  'while not (Path(sys.argv[1])/"renderer-process.json").exists():\n'
                  '    if time.monotonic()>deadline: raise SystemExit(2)\n'
                  '    time.sleep(.02)\n')
        result, run = self.run_harness(boot, camera)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        lifecycle = json.loads((run / 'renderer-lifecycle.json').read_text())
        self.assertIsNone(lifecycle['failure'])
        self.assertIn(lifecycle['cleanup'], ('Renderer stopped', 'Renderer exited'))
        child = benchmark.process_identity(lifecycle['renderer']['pid'])
        self.assertTrue(child is None or child['zombie'])
        self.assertFalse((run / 'process-failure.json').exists())

    def test_controller_failure_stops_split_run_without_stop_after(self):
        boot = ('import os,subprocess,time\n'
                'subprocess.Popen([os.environ["BB_RENDER_BIN"], "-c", "import time;time.sleep(60)"])\n'
                'time.sleep(60)\n')
        camera = ('import sys,time\nfrom pathlib import Path\n'
                  'deadline=time.monotonic()+4\n'
                  'while not (Path(sys.argv[1])/"renderer-process.json").exists():\n'
                  '    if time.monotonic()>deadline: raise SystemExit(9)\n'
                  '    time.sleep(.02)\n'
                  'raise SystemExit(2)\n')
        result, run = self.run_harness(boot, camera, stop_after=False)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        lifecycle = json.loads((run / 'renderer-lifecycle.json').read_text())
        self.assertIn(lifecycle['cleanup'], ('Renderer stopped', 'Renderer exited'))
        self.assertIsNone(benchmark.process_identity(int((run / 'pid.txt').read_text())))

    def test_success_without_observed_renderer_is_failure(self):
        result, run = self.run_harness('import time;time.sleep(60)\n', 'pass\n', stop_after=False)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        failure = json.loads((run / 'process-failure.json').read_text())
        self.assertIn('without observing', failure['renderer_failure'])
        self.assertIsNone(benchmark.process_identity(int((run / 'pid.txt').read_text())))


if __name__ == '__main__':
    unittest.main()
