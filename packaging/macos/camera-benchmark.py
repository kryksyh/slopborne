#!/usr/bin/env python3
"""Enter Continue, wait for gameplay, and exercise the right stick of a diagnostic run.

Use a copied save and game-control.py's input/capture environment. This leaves the
process running and always releases the virtual controller when the test ends.
"""
import argparse
import datetime
import importlib.util
import json
import pathlib
import re
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('directory', type=pathlib.Path)
p.add_argument('log', type=pathlib.Path)
p.add_argument('--seconds', type=float, default=45)
p.add_argument('--timeout', type=float, default=240)
p.add_argument('--idle-seconds', type=float, default=0, help='Monitor idle gameplay after the input sequence')
p.add_argument('--move-seconds', type=float, default=0, help='Walk forward then backward for this many seconds each')
a = p.parse_args()
pattern = re.compile(r'Frame stats: ([\d.]+) FPS, worst frame ([\d.]+) ms.*?; (\d+) shader/pipeline compiles.*?, (\d+) draws/frame')
module = importlib.util.spec_from_file_location('game_control', pathlib.Path(__file__).with_name('game-control.py'))
control = importlib.util.module_from_spec(module)
module.loader.exec_module(control)
pad = a.directory/'pad.txt'
neutral = 'lx=128 ly=128 rx=128 ry=128'
started = time.monotonic()
last_press = -100
presses = 0
warm = 0
previous_count = 0
samples = []
baseline = 0
completed = False
error = None
faults = ('STOP:', 'Host fault', 'VK_ERROR_DEVICE_LOST', 'Lost VkDevice', 'Device lost during', 'Caused GPU Hang', 'VK_ERROR_OUT_OF_DEVICE_MEMORY', 'GPU semaphore wait failed')
try:
    while time.monotonic() - started < a.timeout:
        text = a.log.read_text(errors='replace')
        if any(fault in text for fault in faults):
            raise RuntimeError('Game faulted; see '+str(a.log))
        matches = list(pattern.finditer(text))
        gameplay = bool(matches and int(matches[-1][4]) > 250)
        if len(matches) != previous_count:
            previous_count = len(matches)
            if matches:
                m = matches[-1]
                warm = warm + 1 if gameplay and int(m[3]) == 0 else 0
                print('Waiting:', m[1]+' FPS', m[3]+' compiles', m[4]+' draws/frame', flush=True)
        if warm >= 2:
            break
        now = time.monotonic() - started
        if not gameplay and 'VideoOut: vblank' in text and now > 10 and now-last_press > 6 and presses < 8:
            pad.write_text('cross')
            time.sleep(0.35)
            pad.write_text(neutral)
            last_press = now
            presses += 1
            print('Menu Cross', presses, flush=True)
        time.sleep(0.5)
    else:
        raise RuntimeError('No warm gameplay before timeout')
    control.capture(a.directory)
    baseline = len(a.log.read_text(errors='replace'))
    print('Starting camera sweep', flush=True)
    started = time.monotonic()
    previous_axis = None
    while time.monotonic()-started < a.seconds:
        elapsed = time.monotonic()-started
        axis = 220 if elapsed < a.seconds/2 else 36
        if axis != previous_axis:
            pad.write_text(f'lx=128 ly=128 rx={axis} ry=128')
            previous_axis = axis
        text = a.log.read_text(errors='replace')[baseline:]
        if any(fault in text for fault in faults):
            raise RuntimeError('Game faulted during camera sweep')
        time.sleep(0.5)
    for axis in (48, 208) if a.move_seconds > 0 else ():
        print('Walking, left-stick y:', axis, flush=True)
        pad.write_text(f'lx=128 ly={axis} rx=128 ry=128')
        until = time.monotonic() + a.move_seconds
        while time.monotonic() < until:
            text = a.log.read_text(errors='replace')[baseline:]
            if any(fault in text for fault in faults):
                raise RuntimeError('Game faulted during movement')
            time.sleep(0.5)
    pad.write_text(neutral)
    control.capture(a.directory)
    if a.idle_seconds > 0:
        pad.write_text('')
        print('Monitoring idle gameplay for', a.idle_seconds, 'seconds', flush=True)
        until = time.monotonic() + a.idle_seconds
        previous_frame = a.log.read_text(errors='replace').rfind('Frame stats:')
        last_progress = time.monotonic()
        while time.monotonic() < until:
            text = a.log.read_text(errors='replace')
            if any(fault in text for fault in faults):
                raise RuntimeError('Game faulted after the input sequence')
            frame = text.rfind('Frame stats:')
            if frame != previous_frame:
                previous_frame = frame
                last_progress = time.monotonic()
            elif time.monotonic() - last_progress > 30:
                raise RuntimeError('No renderer progress for 30 seconds after the input sequence')
            time.sleep(0.5)
        control.capture(a.directory)
    completed = True
except BaseException as exc:
    error = str(exc)
    raise
finally:
    pad.write_text('')
    text = a.log.read_text(errors='replace')[baseline:]
    samples = [{'fps':float(m[1]), 'worst_ms':float(m[2]), 'compiles':int(m[3]), 'draws':int(m[4])} for m in pattern.finditer(text)]
    result = {'log':str(a.log.resolve()), 'seconds':a.seconds, 'samples':samples, 'completed':completed, 'error':error, 'move_seconds_each_direction':a.move_seconds, 'idle_seconds':a.idle_seconds}
    target = a.directory/('benchmark-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S')+'.json')
    target.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result), flush=True)
