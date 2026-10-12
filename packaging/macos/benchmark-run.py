#!/usr/bin/env python3
"""Launch an isolated saved-game camera benchmark; keep every log and capture."""
import argparse
import datetime
import ctypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time
from process_metrics import ResourceMonitor

CAMERA_SCRIPT = Path(__file__).with_name('camera-benchmark.py')


class BsdInfo(ctypes.Structure):
    # Darwin sys/proc_info.h: PROC_PIDTBSDINFO, including the microsecond start identity.
    _fields_ = [(name, ctypes.c_uint32) for name in
                'flags status xstatus pid ppid uid gid ruid rgid svuid svgid reserved'.split()] + [
        ('comm', ctypes.c_char * 16), ('name', ctypes.c_char * 32)] + [
        (name, ctypes.c_uint32) for name in 'nfiles pgid jobc tdev tpgid nice'.split()] + [
        ('start_sec', ctypes.c_uint64), ('start_usec', ctypes.c_uint64)]


def process_identity(pid):
    """Read identity without signalling; None means absent or unverifiable."""
    if sys.platform == 'darwin':
        lib = ctypes.CDLL('/usr/lib/libproc.dylib', use_errno=True)
        lib.proc_pidinfo.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_uint64,
                                    ctypes.c_void_p, ctypes.c_int]
        lib.proc_pidpath.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32]
        info = BsdInfo()
        if lib.proc_pidinfo(pid, 3, 0, ctypes.byref(info), ctypes.sizeof(info)) != ctypes.sizeof(info):
            return None
        path = ctypes.create_string_buffer(4096)
        lib.proc_pidpath(pid, path, len(path))
        return {'pid': pid, 'ppid': info.ppid, 'pgid': info.pgid,
                'start': [info.start_sec, info.start_usec],
                'path': os.fsdecode(path.value), 'zombie': info.status == 5,
                'name': os.fsdecode(info.name or info.comm)}
    if sys.platform.startswith('linux'):
        try:
            stat = Path(f'/proc/{pid}/stat').read_text()
            tail = stat[stat.rfind(')') + 2:].split()
            try:
                path = os.readlink(f'/proc/{pid}/exe')
            except OSError:
                path = ''
            return {'pid': pid, 'ppid': int(tail[1]), 'pgid': int(tail[2]),
                    'start': tail[19], 'path': path,
                    'zombie': tail[0] == 'Z', 'name': stat[stat.find('(') + 1:stat.rfind(')')]}
        except (OSError, ValueError, IndexError):
            return None
    raise RuntimeError('Renderer lifecycle tracking requires macOS or Linux')


class RendererTracker:
    def __init__(self, game, binary, run, isolated=False):
        self.game, self.binary, self.run = game, str(binary.resolve()), run
        self.isolated = isolated
        self.parent = process_identity(game.pid)
        self.renderer = None
        self.failure = None
        self.cleanup_result = None

    def check(self, record_failure=True):
        if self.renderer is None:
            # An isolated run can also identify an orphan by its original process group
            # and unique snapshot path if bb-probe exited before the first sample.
            parent = process_identity(self.game.pid)
            parent_matches = bool(parent and self.parent and parent['start'] == self.parent['start'])
            if parent_matches or self.isolated:
                listing = subprocess.run(['ps', '-axo', 'pid=,ppid='], capture_output=True,
                                         text=True, check=True).stdout
                for row in listing.splitlines():
                    fields = row.split()
                    if len(fields) != 2 or int(fields[1]) not in (self.game.pid, 1):
                        continue
                    candidate = process_identity(int(fields[0]))
                    if not candidate:
                        continue
                    direct = parent_matches and candidate['ppid'] == self.game.pid
                    orphan = (self.isolated and candidate['ppid'] == 1
                              and candidate['pgid'] == self.game.pid)
                    if not (direct or orphan):
                        continue
                    if candidate['path'] == self.binary:
                        self.renderer = candidate
                        break
                    # A child that failed before the first sample can already be a zombie;
                    # Darwin may no longer provide its executable path. Never signal it.
                    if record_failure and candidate['zombie'] and candidate['name'] == 'bb-render':
                        self.failure = 'Renderer exited during startup before identity capture'
            if self.renderer:
                (self.run / 'renderer-process.json').write_text(json.dumps(self.renderer, indent=2) + '\n')
        if self.renderer and record_failure:
            current = process_identity(self.renderer['pid'])
            if (not current or current['start'] != self.renderer['start'] or current['zombie']
                    or current['path'] != self.binary):
                self.failure = 'Renderer exited before benchmark completion'
        if record_failure and self.game.poll() == 3:
            self.failure = 'bb-probe reported renderer failure (exit 3)'
        return self.failure

    def cleanup(self):
        """Stop only the captured run-owned renderer, allowing orphan adoption by init."""
        if not self.renderer:
            # A very short run can spawn the renderer between the final check and
            # parent termination. Discovery here must not label our own stop a crash.
            self.check(record_failure=False)
        if not self.renderer:
            self.cleanup_result = 'No live renderer identity captured'
            return
        for sig, grace in ((signal.SIGTERM, 2), (signal.SIGKILL, 2)):
            current = process_identity(self.renderer['pid'])
            if not current or current['zombie']:
                self.cleanup_result = 'Renderer exited'
                return
            if (current['start'] != self.renderer['start'] or current['path'] != self.binary
                    or current['ppid'] not in (self.game.pid, 1)):
                self.cleanup_result = 'Identity changed; refused to signal'
                return
            try:
                os.kill(current['pid'], sig)
            except ProcessLookupError:
                self.cleanup_result = 'Renderer exited'
                return
            deadline = time.monotonic() + grace
            while time.monotonic() < deadline:
                current = process_identity(self.renderer['pid'])
                if not current or current['zombie'] or current['start'] != self.renderer['start']:
                    self.cleanup_result = 'Renderer stopped'
                    return
                time.sleep(.05)
        self.cleanup_result = 'Renderer did not exit after cleanup signals'
        self.failure = self.failure or self.cleanup_result


def run_controller(command, game, tracker):
    controller = subprocess.Popen(command)
    try:
        while controller.poll() is None:
            failure = tracker.check() if tracker else None
            if failure or game.poll() is not None:
                controller.terminate()
                break
            time.sleep(.1)
        if tracker:
            tracker.check()
        try:
            return controller.wait(timeout=5)
        except subprocess.TimeoutExpired:
            controller.kill()
            return controller.wait()
    finally:
        if controller.poll() is None:
            controller.terminate()
            try:
                controller.wait(timeout=5)
            except subprocess.TimeoutExpired:
                controller.kill()
                controller.wait()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--prepared', type=Path, required=True, help='Directory containing boot-linked.bin and content.bin')
    p.add_argument('--profile', type=Path, required=True, help='Directory containing patches.bin and bbport.ini')
    p.add_argument('--game', type=Path, required=True)
    p.add_argument('--save', type=Path, required=True, help='User data to copy into this test')
    p.add_argument('--binary', type=Path, default=Path('out/macos/gpu-performance/bb-probe'))
    p.add_argument('--render-binary', type=Path, help='Snapshot both split binaries into this run and supervise bb-render')
    p.add_argument('--out', type=Path, default=Path('out/macos/benchmarks'))
    p.add_argument('--seconds', type=float, default=45)
    p.add_argument('--idle-seconds', type=float, default=0)
    p.add_argument('--move-seconds', type=float, default=0)
    p.add_argument('--timeout', type=float, default=300)
    p.add_argument('--startup-stall-seconds', type=float, default=45,
                   help='Abort startup if the game log stops advancing; 0 disables this check')
    p.add_argument('--env', action='append', default=[], metavar='KEY=VALUE')
    p.add_argument('--stop-after', action='store_true')
    p.add_argument('--wait-for-exit', action='store_true', help='Keep the launcher attached after the benchmark')
    p.add_argument('--captures', choices=('all', 'none'), default='all',
                   help='Disable readback/PNG captures for FPS comparisons after visual validation')
    p.add_argument('--ab-masks', help='Comma-separated BB_TOGGLE_FILE masks for warmed in-process A/B')
    p.add_argument('--ab-seconds', type=float, default=30)
    p.add_argument('--ab-settle', type=float, default=10)
    a = p.parse_args()
    setup_started = time.monotonic()
    for f in [a.prepared/'boot-linked.bin', a.prepared/'content.bin', a.profile/'patches.bin', a.profile/'bbport.ini', a.game/'eboot.bin', a.binary, *([a.render_binary] if a.render_binary else [])]:
        if not f.is_file(): p.error('Missing '+str(f))
    run = a.out.resolve()/datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    run.mkdir(parents=True)
    (run/'captures').mkdir()
    (run/'pad.txt').write_text('')
    (run/'capture-sequence.txt').write_text('0')
    shutil.copytree(a.save, run/'user')
    env = dict(os.environ, BB_GPU_USER_DIR=str(a.profile.resolve()/'gpu'), BB_PIPELINE_CACHE='1', BB_DUMP_SHADERS='0',
               BB_CONFIG=str(a.profile.resolve()/'bbport.ini'), BB_UPSCALER='off', BB_OBJECT_MOTION='0', BB_VBLANK_HZ='0',
               BB_FPS_LIMIT='30', BB_FRAME_STATS='1', BB_GPU_PROFILE='0', BB_GPU_LOG='error', MVK_CONFIG_LOG_LEVEL='1',
               MTL_DEBUG_LAYER='0', MTL_SHADER_VALIDATION='0', BB_FRAME_DUMP_AT='0', BB_CAPTURE_AT='0')
    for pair in a.env:
        key, value = pair.split('=', 1)
        env[key] = value
    Path(env['BB_GPU_USER_DIR']).mkdir(parents=True, exist_ok=True)
    # Automation controls are always unique to this run.
    env.update(BB_PAD_FILE=str(run/'pad.txt'), BB_FRAME_DUMP_SEQUENCE=str(run/'capture-sequence.txt'), BB_DUMP_DIR=str(run/'captures'))
    phase_args = []
    if a.ab_masks:
        masks = [int(value, 0) for value in a.ab_masks.split(',')]
        if any(mask < 0 or mask >= 1 << 64 for mask in masks):
            p.error('A/B masks must be unsigned 64-bit values')
        toggle_file = run/'toggles.txt'
        toggle_file.write_text(str(masks[0])+'\n')
        env['BB_TOGGLE_FILE'] = str(toggle_file)
        phase_args = ['--ab-masks', a.ab_masks, '--ab-seconds', str(a.ab_seconds),
                      '--ab-settle', str(a.ab_settle), '--toggle-file', str(toggle_file)]
    binary = a.binary.resolve()
    render_binary = None
    if a.render_binary:
        binary = run/'bb-probe'
        render_binary = run/'bb-render'
        shutil.copy2(a.binary, binary)
        shutil.copy2(a.render_binary, render_binary)
        env['BB_RENDER_BIN'] = str(render_binary)
    command = [str(binary), str(a.prepared.resolve()/'boot-linked.bin'), '--content-profile',
               str(a.prepared.resolve()/'content.bin'), '--patches', str(a.profile.resolve()/'patches.bin'),
               '--app0', str(a.game.resolve()), '--user', str(run/'user'), '--timeout', '0']
    (run/'configuration.json').write_text(json.dumps({
        'command': command,
        'environment': {k: v for k, v in env.items() if k.startswith(('BB_', 'MVK_', 'MTL_'))},
        'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
        'binary_source': str(a.binary.resolve()),
        'render_binary_source': str(a.render_binary.resolve()) if a.render_binary else None,
        'render_binary_sha256': hashlib.sha256(render_binary.read_bytes()).hexdigest() if render_binary else None,
        'resource_metrics_scope': ('guest process only; excludes native renderer'
                                   if render_binary else 'whole game process'),
        'patches_sha256': hashlib.sha256((a.profile/'patches.bin').read_bytes()).hexdigest(),
        'settings': (a.profile/'bbport.ini').read_text(),
    }, indent=2)+'\n')
    log = run/'startup.log'
    with log.open('xb') as stream:
        game = subprocess.Popen(command, env=env, stdout=stream, stderr=subprocess.STDOUT,
                                start_new_session=bool(a.render_binary))
    setup_seconds = time.monotonic() - setup_started
    launch_started = time.monotonic()
    (run/'pid.txt').write_text(str(game.pid))
    print('Run:', run, 'PID:', game.pid, flush=True)
    tracker = RendererTracker(game, render_binary, run, isolated=True) if render_binary else None
    result_code = 1
    pre_stop_code = None
    with ResourceMonitor(game.pid, run):
        try:
            result_code = run_controller([sys.executable, str(CAMERA_SCRIPT), str(run), str(log),
                                     '--seconds', str(a.seconds), '--timeout', str(a.timeout), '--startup-stall-seconds', str(a.startup_stall_seconds), '--move-seconds', str(a.move_seconds), '--idle-seconds', str(a.idle_seconds), '--captures', a.captures, *phase_args], game, tracker)
            if tracker and result_code == 0 and not tracker.renderer and not tracker.failure:
                tracker.failure = 'Split benchmark completed without observing its renderer'
            if (a.wait_for_exit and not a.stop_after and result_code == 0
                    and game.poll() is None and not (tracker and tracker.failure)):
                print('Benchmark finished; game remains open. PID:', game.pid, flush=True)
                while game.poll() is None:
                    if tracker and tracker.check():
                        break
                    time.sleep(.1)
        finally:
            (run/'pad.txt').write_text('')
            pre_stop_code = game.poll()
            if tracker:
                tracker.check()
            if (a.stop_after or (tracker and (tracker.failure or result_code != 0))) and game.poll() is None:
                game.terminate()
                try: game.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    print('Game did not exit after SIGTERM; PID:', game.pid, flush=True)
            # Without --stop-after/--wait-for-exit a successful benchmark intentionally
            # leaves its game open; its renderer is not a leftover.
            if tracker and (a.stop_after or tracker.failure or result_code != 0 or game.poll() is not None):
                tracker.cleanup()
            if tracker:
                (run/'renderer-lifecycle.json').write_text(json.dumps({
                    'renderer': tracker.renderer, 'failure': tracker.failure,
                    'cleanup': tracker.cleanup_result,
                }, indent=2)+'\n')
    (run/'phase-times.json').write_text(json.dumps({
        'setup_seconds': setup_seconds, 'launch_to_finish_seconds': time.monotonic() - launch_started,
        'captures': a.captures,
    }, indent=2)+'\n')
    if pre_stop_code not in (None, 0) or (tracker and tracker.failure):
        failure = {'exit_code': game.returncode, 'time': datetime.datetime.now().isoformat(),
                   'benchmark_exit_code': result_code, 'log': str(log),
                   'renderer_failure': tracker.failure if tracker else None}
        (run/'process-failure.json').write_text(json.dumps(failure, indent=2)+'\n')
        print('Game exited with an error:', failure, flush=True)
        return 1
    return result_code


if __name__ == '__main__':
    sys.exit(main())
