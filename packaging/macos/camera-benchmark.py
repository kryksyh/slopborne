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
p.add_argument('--startup-stall-seconds', type=float, default=45)
p.add_argument('--idle-seconds', type=float, default=0, help='Monitor idle gameplay after the input sequence')
p.add_argument('--move-seconds', type=float, default=0, help='Walk forward then backward for this many seconds each')
p.add_argument('--captures', choices=('all', 'none'), default='all')
p.add_argument('--ab-masks')
p.add_argument('--ab-seconds', type=float, default=30)
p.add_argument('--ab-settle', type=float, default=10)
p.add_argument('--toggle-file', type=pathlib.Path)
a = p.parse_args()
if a.ab_masks and (not a.toggle_file or a.ab_seconds <= 0 or a.ab_settle < 5):
    p.error('A/B requires a toggle file, positive phase duration and >=5s settling')
pattern = re.compile(r'Frame stats: ([\d.]+) FPS, worst frame ([\d.]+) ms.*?; (\d+) shader/pipeline compiles.*?, (\d+) draws/frame')
module = importlib.util.spec_from_file_location('game_control', pathlib.Path(__file__).with_name('game-control.py'))
control = importlib.util.module_from_spec(module)
module.loader.exec_module(control)
pad = a.directory/'pad.txt'
neutral = 'lx=128 ly=128 rx=128 ry=128'
started = time.monotonic()
warmup_started = started
warmup_seconds = None
capture_seconds = 0.0
phases = []

def capture():
    global capture_seconds
    if a.captures == 'all':
        begin = time.monotonic()
        control.capture(a.directory)
        capture_seconds += time.monotonic() - begin

last_press = -100
presses = 0
warm = 0
previous_count = 0
samples = []
baseline = 0
completed = False
error = None
startup_log_size = -1
startup_log_progress = time.monotonic()
faults = ('STOP:', 'Host fault', 'VK_ERROR_DEVICE_LOST', 'Lost VkDevice', 'Device lost during', 'Caused GPU Hang', 'VK_ERROR_OUT_OF_DEVICE_MEMORY', 'GPU semaphore wait failed')
try:
    while time.monotonic() - started < a.timeout:
        text = a.log.read_text(errors='replace')
        if len(text) != startup_log_size:
            startup_log_size = len(text)
            startup_log_progress = time.monotonic()
        elif a.startup_stall_seconds > 0 and time.monotonic() - startup_log_progress > a.startup_stall_seconds:
            raise RuntimeError('No startup log progress for '+str(a.startup_stall_seconds)+' seconds')
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
    warmup_seconds = time.monotonic() - warmup_started
    capture()
    baseline = len(a.log.read_text(errors='replace'))
    if a.ab_masks:
        pad.write_text(neutral)
        for mask in map(lambda value: int(value, 0), a.ab_masks.split(',')):
            phase_start = len(a.log.read_text(errors='replace'))
            a.toggle_file.write_text(str(mask)+'\n')
            # The watcher only logs changes; adjacent identical masks need no ack.
            if not phases or phases[-1]['mask'] != mask:
                previous = a.log.read_text(errors='replace')[:phase_start]
                changes = re.findall(r'Runtime: disabled optimizations mask=(\d+)', previous)
                if not changes or int(changes[-1]) != mask:
                    deadline = time.monotonic() + 5
                    while f'Runtime: disabled optimizations mask={mask}\n' not in a.log.read_text(errors='replace')[phase_start:]:
                        if time.monotonic() > deadline:
                            raise RuntimeError('No acknowledgement for toggle mask '+str(mask))
                        time.sleep(.1)
            phase_started = time.monotonic()
            sample_begin = None
            last_progress = phase_started
            previous_frame = -1
            while time.monotonic() - phase_started < a.ab_settle + a.ab_seconds:
                text = a.log.read_text(errors='replace')
                if any(fault in text[phase_start:] for fault in faults):
                    raise RuntimeError('Game faulted during A/B phase')
                frame = text.rfind('Frame stats:')
                if frame != previous_frame:
                    previous_frame = frame
                    last_progress = time.monotonic()
                elif time.monotonic() - last_progress > 15:
                    raise RuntimeError('No frame progress during A/B phase')
                if sample_begin is None and time.monotonic() - phase_started >= a.ab_settle:
                    sample_begin = len(text)
                time.sleep(.25)
            phase_text = a.log.read_text(errors='replace')[sample_begin:]
            phase_samples = [{'fps':float(m[1]), 'worst_ms':float(m[2]),
                              'compiles':int(m[3]), 'draws':int(m[4])}
                             for m in pattern.finditer(phase_text)]
            phase = {'mask':mask, 'samples':phase_samples, 'settle_seconds':a.ab_settle,
                     'measured_seconds':a.ab_seconds}
            phases.append(phase)
            print('A/B phase:', json.dumps(phase), flush=True)
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
    capture()
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
        capture()
    completed = True
except BaseException as exc:
    error = str(exc)
    raise
finally:
    pad.write_text('')
    text = a.log.read_text(errors='replace')[baseline:]
    samples = [{'fps':float(m[1]), 'worst_ms':float(m[2]), 'compiles':int(m[3]), 'draws':int(m[4])} for m in pattern.finditer(text)]
    result = {'log':str(a.log.resolve()), 'seconds':a.seconds, 'samples':samples, 'completed':completed, 'error':error, 'move_seconds_each_direction':a.move_seconds, 'idle_seconds':a.idle_seconds,
              'warmup_seconds':warmup_seconds, 'capture_seconds':capture_seconds, 'captures':a.captures,
              'phases':phases}
    target = a.directory/('benchmark-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S')+'.json')
    target.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result), flush=True)
