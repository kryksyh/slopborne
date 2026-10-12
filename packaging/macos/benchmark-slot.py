#!/usr/bin/env python3
"""Run one ready build/test job in the shared FIFO; preserve all queue records.

All worktrees must use the same --sync path. Legacy AGENT_SYNC.md owners are
respected. Queue only ready jobs, and keep a job to one short experiment series.
No process other than the wrapper's own child is signalled.
"""
import argparse
import contextlib
import datetime
import fcntl
import json
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
import time
import uuid


def stamp():
    return datetime.datetime.now().astimezone().isoformat(timespec='seconds')


def process_identity(pid):
    result = subprocess.run(['ps', '-p', str(pid), '-o', 'lstart='],
                            capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 else None


def busy_processes():
    result = subprocess.check_output(['ps', '-axo', 'pid=,stat=,comm='], text=True)
    names = RUN_NAMES | COMPILER_NAMES
    busy = {}
    live = set()
    for line in result.splitlines():
        fields = line.split(None, 2)
        if len(fields) != 3 or fields[1].startswith('Z'):
            continue
        pid, _, command = fields
        live.add(pid)
        if Path(command).name in names:
            busy[pid] = f'{pid} {command}'
    # ps comm identifies the Python interpreter, so inspect argv for benchmark scripts.
    arguments = subprocess.check_output(['ps', '-axo', 'pid=,args='], text=True)
    for line in arguments.splitlines():
        fields = line.split(None, 1)
        if len(fields) != 2 or fields[0] not in live or fields[0] in busy:
            continue
        script = benchmark_script(fields[1])
        if script:
            busy[fields[0]] = f'{fields[0]} {script}'
    return list(busy.values())


COMPILER_NAMES = {'ninja', 'clang', 'clang++'}
RUN_NAMES = {'bb-probe', 'bb-render', 'image-slice-clear-test'}
BENCHMARK_NAMES = {'benchmark-run.py', 'camera-benchmark.py'}


def benchmark_script(arguments):
    try:
        tokens = shlex.split(arguments)
    except ValueError:
        return None
    if not tokens:
        return None
    if Path(tokens[0]).name in BENCHMARK_NAMES:
        return Path(tokens[0]).name
    # Inspect only the interpreter's script, never a queued command later in argv.
    for token in tokens[1:]:
        if token in ('-c', '-m'):
            return None
        if token.startswith('-'):
            continue
        return Path(token).name if Path(token).name in BENCHMARK_NAMES else None
    return None


def release_blockers(blockers):
    """Compilers block launch, but cannot strand an already finished GPU claim."""
    return [line for line in blockers
            if Path(line.split(None, 1)[-1]).name not in COMPILER_NAMES]


@contextlib.contextmanager
def locked(path):
    with path.open('a+') as stream:
        fcntl.flock(stream, fcntl.LOCK_EX)
        yield


def write_record(path, record):
    # Readers hold the same lock. Keep completed/cancelled records for auditing.
    path.write_text(json.dumps(record, indent=2) + '\n')


def replace_marker(sync, expected, replacement):
    with sync.open('r+') as stream:
        text = stream.read()
        first, separator, rest = text.partition('\n')
        if first != expected:
            return False
        stream.seek(0)
        stream.write(replacement + '\n' + rest)
        stream.truncate()
    return True


def recover_unlaunched_marker(sync, queue, marker, blockers):
    """Called under queue.lock; recover only a proven pre-launch conflict."""
    if blockers:
        return False
    match = re.search(r'\[queue ([0-9]{20}-[0-9a-f]{8})\]$', marker)
    if not match:
        return False
    path = queue / (match[1] + '.json')
    if not path.is_file():
        return False
    record = json.loads(path.read_text())
    proven_unlaunched = (record.get('launch_attempted') is False or
                         ('launch_attempted' not in record and
                          record.get('error') == 'Legacy slot conflict; command was not launched'))
    if (record.get('state') != 'cancelled' or 'child_pid' in record or not proven_unlaunched
            or record.get('claim', marker) != marker
            or process_identity(record['pid']) == record['identity']):
        return False
    return replace_marker(sync, marker,
                          f'GPU SLOT: free — recovered unlaunched queue job {match[1]} {stamp()}')


def recover_completed_marker(sync, queue, marker, blockers):
    """Under queue.lock, release only a terminal job with verifiably dead owners."""
    if release_blockers(blockers):
        return False
    match = re.search(r'\[queue ([0-9]{20}-[0-9a-f]{8})\]$', marker)
    if not match:
        return False
    path = queue / (match[1] + '.json')
    if not path.is_file():
        return False
    record = json.loads(path.read_text())
    claim = record.get('claim')
    if not claim and all(key in record for key in ('owner', 'label', 'ticket')):
        claim = f'GPU SLOT: BUSY — {record["owner"]}, {record["label"]} [queue {record["ticket"]}]'
    if (record.get('ticket') != match[1] or claim != marker or record.get('state') != 'done'
            or not record.get('ended') or not isinstance(record.get('exit_code'), int)
            or not record.get('child_pid') or not record.get('identity') or not record.get('pid')):
        return False
    if process_identity(record['pid']) == record['identity']:
        return False
    child_identity = process_identity(record['child_pid'])
    if child_identity is not None and (not record.get('child_identity')
                                       or child_identity == record['child_identity']):
        return False
    if not replace_marker(sync, marker,
                          f'GPU SLOT: free — recovered completed queue job {match[1]} {stamp()}'):
        return False
    record['marker_recovered'] = stamp()
    write_record(path, record)
    return True


def run(args):
    sync = args.sync.resolve()
    queue = args.queue.resolve() if args.queue else sync.parent / 'out/macos/slot-queue'
    queue.mkdir(parents=True, exist_ok=True)
    lock = queue / 'queue.lock'
    if args.status:
        with locked(lock):
            records = [json.loads(p.read_text()) for p in sorted(queue.glob('*.json'))]
        print(json.dumps({'marker': sync.read_text().splitlines()[0],
                          'jobs': records, 'busy_processes': busy_processes()}, indent=2))
        return 0
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command:
        raise ValueError('Supply a command after --')
    token = f'{time.time_ns():020d}-{uuid.uuid4().hex[:8]}'
    path = queue / (token + '.json')
    queued = time.monotonic()
    record = {'ticket': token, 'owner': args.owner, 'label': args.label,
              'pid': os.getpid(), 'identity': process_identity(os.getpid()),
              'queued': stamp(), 'state': 'waiting', 'command': command,
              'cwd': str(Path.cwd()), 'launch_attempted': False}
    claim = f'GPU SLOT: BUSY — {args.owner}, {args.label} [queue {token}]'
    record['claim'] = claim
    acquired = False
    child = None
    exit_code = 1
    last_notice = -100.0
    with locked(lock):
        write_record(path, record)
    try:
        while True:
            ready = False
            with locked(lock):
                pending = []
                for other in sorted(queue.glob('*.json')):
                    data = json.loads(other.read_text())
                    if data['state'] not in ('waiting', 'running'):
                        continue
                    if process_identity(data['pid']) != data['identity']:
                        data.update(state='abandoned', ended=stamp())
                        write_record(other, data)
                        # Never clear a dead owner's marker: its game may survive.
                        continue
                    pending.append(data['ticket'])
                marker = sync.read_text().splitlines()[0]
                blockers = busy_processes()
                # A competing live run can temporarily retain our pre-launch marker.
                # Once it ends, yield our exact marker even if compilers remain active.
                if marker == claim and record['state'] == 'waiting' and not release_blockers(blockers):
                    replace_marker(sync, claim, f'GPU SLOT: free — {args.owner} retrying pre-launch claim {stamp()}')
                    marker = sync.read_text().splitlines()[0]
                    acquired = False
                if (recover_unlaunched_marker(sync, queue, marker, release_blockers(blockers))
                        or recover_completed_marker(sync, queue, marker, blockers)):
                    marker = sync.read_text().splitlines()[0]
                if pending and pending[0] == token and marker.startswith('GPU SLOT: free') and not blockers:
                    acquired = replace_marker(sync, marker, claim)
                    if acquired:
                        ready = True
                        record.update(state='running', started=stamp(),
                                      wait_seconds=round(time.monotonic() - queued, 3))
                        write_record(path, record)
            if not ready:
                elapsed = time.monotonic() - queued
                if elapsed >= args.timeout:
                    raise TimeoutError('Queue timeout; no other job was interrupted')
                if elapsed - last_notice >= 30:
                    print(f'Queued {token}: {marker}; blockers={blockers}', flush=True)
                    last_notice = elapsed
                time.sleep(2)
                continue
            # Give legacy (non-locking) launchers time to publish a competing claim.
            time.sleep(2)
            with locked(lock):
                marker = sync.read_text().splitlines()[0]
                blockers = busy_processes()
                if marker == claim and not blockers:
                    break
                record.setdefault('prelaunch_conflicts', []).append(
                    {'time': stamp(), 'marker': marker, 'blockers': blockers})
                record['state'] = 'waiting'
                if marker == claim and not release_blockers(blockers):
                    replace_marker(sync, claim, f'GPU SLOT: free — {args.owner} retrying pre-launch claim {stamp()}')
                    acquired = False
                elif marker != claim:
                    acquired = False
                write_record(path, record)
            if time.monotonic() - queued >= args.timeout:
                raise TimeoutError('Queue timeout after pre-launch conflict; command was not launched')
            print(f'Retrying {token} in place: {marker}; blockers={blockers}', flush=True)
        print(f'Starting {token}, waited {record["wait_seconds"]}s: {command}', flush=True)
        started = time.monotonic()
        # Persist intent before spawning: absence of child_pid alone cannot prove
        # an interrupted wrapper never launched a child.
        record['launch_attempted'] = True
        with locked(lock):
            write_record(path, record)
        child = subprocess.Popen(command)
        record['child_pid'] = child.pid
        record['child_identity'] = process_identity(child.pid)
        with locked(lock):
            write_record(path, record)
        exit_code = child.wait()
        record.update(state='done', exit_code=exit_code,
                      run_seconds=round(time.monotonic() - started, 3))
        return exit_code if exit_code >= 0 else 128 - exit_code
    except BaseException as exc:
        record.update(state='cancelled', error=str(exc))
        if child is not None and child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=10)
            except subprocess.TimeoutExpired:
                record['child_still_running'] = child.pid
        raise
    finally:
        record['ended'] = stamp()
        with locked(lock):
            blockers = release_blockers(busy_processes())
            if acquired and not blockers and (child is None or child.poll() is not None):
                replace_marker(sync, claim, f'GPU SLOT: free — {args.owner} queue job ended {stamp()}')
            elif acquired:
                record['release_blocked_by'] = blockers
            write_record(path, record)
        print(f'Job record: {path}', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sync', type=Path, required=True)
    parser.add_argument('--queue', type=Path, help='Default: beside sync, out/macos/slot-queue')
    parser.add_argument('--owner', default='primary')
    parser.add_argument('--label', default='ready experiment')
    parser.add_argument('--timeout', type=float, default=1800)
    parser.add_argument('--status', action='store_true')
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    return run(args)


if __name__ == '__main__':
    sys.exit(main())
