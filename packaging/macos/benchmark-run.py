#!/usr/bin/env python3
"""Launch an isolated saved-game camera benchmark; keep every log and capture."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--prepared', type=Path, required=True, help='Directory containing boot-linked.bin and content.bin')
p.add_argument('--profile', type=Path, required=True, help='Directory containing patches.bin and bbport.ini')
p.add_argument('--game', type=Path, required=True)
p.add_argument('--save', type=Path, required=True, help='User data to copy into this test')
p.add_argument('--binary', type=Path, default=Path('out/macos/gpu-performance/bb-probe'))
p.add_argument('--out', type=Path, default=Path('out/macos/benchmarks'))
p.add_argument('--seconds', type=float, default=45)
p.add_argument('--idle-seconds', type=float, default=0)
p.add_argument('--move-seconds', type=float, default=0)
p.add_argument('--timeout', type=float, default=300)
p.add_argument('--env', action='append', default=[], metavar='KEY=VALUE')
p.add_argument('--stop-after', action='store_true')
p.add_argument('--wait-for-exit', action='store_true', help='Keep the launcher attached after the benchmark')
a = p.parse_args()
for f in [a.prepared/'boot-linked.bin', a.prepared/'content.bin', a.profile/'patches.bin', a.profile/'bbport.ini', a.game/'eboot.bin', a.binary]:
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
command = [str(a.binary.resolve()), str(a.prepared.resolve()/'boot-linked.bin'), '--content-profile',
           str(a.prepared.resolve()/'content.bin'), '--patches', str(a.profile.resolve()/'patches.bin'),
           '--app0', str(a.game.resolve()), '--user', str(run/'user'), '--timeout', '0']
(run/'configuration.json').write_text(json.dumps({
    'command': command,
    'environment': {k: v for k, v in env.items() if k.startswith(('BB_', 'MVK_', 'MTL_'))},
    'binary_sha256': hashlib.sha256(a.binary.read_bytes()).hexdigest(),
    'patches_sha256': hashlib.sha256((a.profile/'patches.bin').read_bytes()).hexdigest(),
    'settings': (a.profile/'bbport.ini').read_text(),
}, indent=2)+'\n')
log = run/'startup.log'
with log.open('xb') as stream:
    game = subprocess.Popen(command, env=env, stdout=stream, stderr=subprocess.STDOUT)
(run/'pid.txt').write_text(str(game.pid))
print('Run:', run, 'PID:', game.pid, flush=True)
try:
    result = subprocess.run([sys.executable, str(Path(__file__).with_name('camera-benchmark.py')), str(run), str(log),
                             '--seconds', str(a.seconds), '--timeout', str(a.timeout), '--move-seconds', str(a.move_seconds), '--idle-seconds', str(a.idle_seconds)])
finally:
    (run/'pad.txt').write_text('')
    if a.stop_after and game.poll() is None:
        game.terminate()
        try: game.wait(timeout=10)
        except subprocess.TimeoutExpired:
            print('Game did not exit after SIGTERM; PID:', game.pid, flush=True)
if a.wait_for_exit and not a.stop_after and game.poll() is None:
    print('Benchmark finished; game remains open. PID:', game.pid, flush=True)
    game.wait()
if not a.stop_after and game.poll() not in (None, 0):
    failure = {'exit_code': game.returncode, 'time': datetime.datetime.now().isoformat(),
               'benchmark_exit_code': result.returncode, 'log': str(log)}
    (run/'process-failure.json').write_text(json.dumps(failure, indent=2)+'\n')
    print('Game exited with an error:', failure, flush=True)
    sys.exit(1)
sys.exit(result.returncode)
