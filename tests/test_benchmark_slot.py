"""CPU-only concurrency checks. Test artifacts are deliberately retained."""
import json
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
WRAPPER = ROOT / 'packaging/macos/benchmark-slot.py'
SPEC = importlib.util.spec_from_file_location('benchmark_slot', WRAPPER)
slot = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(slot)
# Ignore real GPU processes only in this isolated queue with dummy commands.
LOADER = ("import importlib.util,sys; "
          "s=importlib.util.spec_from_file_location('slot',sys.argv.pop(1)); "
          "m=importlib.util.module_from_spec(s);s.loader.exec_module(m); "
          "m.busy_processes=lambda: [];sys.exit(m.main())")


class QueueTest(unittest.TestCase):
    def setUp(self):
        parent = ROOT / 'out/macos/slot-queue-tests'
        parent.mkdir(parents=True, exist_ok=True)
        self.directory = Path(tempfile.mkdtemp(dir=parent))
        self.sync = self.directory / 'AGENT_SYNC.md'
        self.sync.write_text('GPU SLOT: free\n\nRetain this text.\n')
        self.queue = self.directory / 'queue'

    def start(self, label, command, timeout=20, loader=LOADER):
        return subprocess.Popen([sys.executable, '-c', loader, str(WRAPPER),
                                 '--sync', str(self.sync), '--queue', str(self.queue),
                                 '--owner', 'test', '--label', label, '--timeout', str(timeout),
                                 '--', *command], stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)

    def records(self):
        return [json.loads(p.read_text()) for p in sorted(self.queue.glob('*.json'))]

    def test_fifo_and_release_on_child_failure(self):
        events = self.directory / 'events'
        def command(label, code):
            return [sys.executable, '-c',
                    'from pathlib import Path;import time,sys;'
                    f'p=Path({str(events)!r});'
                    f'p.open("a").write({label!r}+" start\\n");'
                    'time.sleep(.2);'
                    f'p.open("a").write({label!r}+" end\\n");sys.exit({code})']
        first = self.start('one', command('one', 3))
        deadline = time.monotonic() + 5
        while not list(self.queue.glob('*.json')):
            self.assertLess(time.monotonic(), deadline)
            time.sleep(.05)
        second = self.start('two', command('two', 0))
        first_output = first.communicate(timeout=20)[0]
        second_output = second.communicate(timeout=20)[0]
        self.assertEqual(first.returncode, 3, first_output)
        self.assertEqual(second.returncode, 0, second_output)
        self.assertEqual(events.read_text().splitlines(), ['one start', 'one end', 'two start', 'two end'])
        self.assertEqual([r['state'] for r in self.records()], ['done', 'done'])
        self.assertTrue(self.sync.read_text().startswith('GPU SLOT: free'))
        self.assertTrue(self.sync.read_text().endswith('\nRetain this text.\n'))

    def test_legacy_owner_not_replaced_on_timeout(self):
        original = 'GPU SLOT: BUSY — legacy owner\n\nKeep append history.\n'
        self.sync.write_text(original)
        child = self.start('timeout', [sys.executable, '-c', 'raise RuntimeError("must not launch")'], .1)
        child.communicate(timeout=8)
        self.assertNotEqual(child.returncode, 0)
        self.assertEqual(self.sync.read_text(), original)
        record, = self.records()
        self.assertEqual(record['state'], 'cancelled')
        self.assertNotIn('child_pid', record)

    def test_dead_waiter_does_not_block(self):
        self.queue.mkdir()
        (self.queue / '000-dead.json').write_text(json.dumps(
            {'ticket': '000-dead', 'state': 'waiting', 'pid': 99999999, 'identity': 'gone'}))
        child = self.start('alive', [sys.executable, '-c', 'pass'])
        output = child.communicate(timeout=10)[0]
        self.assertEqual(child.returncode, 0, output)
        self.assertEqual(self.records()[0]['state'], 'abandoned')

    def test_only_proven_unlaunched_conflict_is_recovered(self):
        self.queue.mkdir()
        ticket = '01791840086104504000-eefadc7a'
        marker = f'GPU SLOT: BUSY — test [queue {ticket}]'
        self.sync.write_text(marker + '\nKeep history.\n')
        record = {'state': 'cancelled', 'pid': 99999999, 'identity': 'gone',
                  'error': 'Legacy slot conflict; command was not launched'}
        path = self.queue / (ticket + '.json')
        path.write_text(json.dumps(record))
        with patch.object(slot, 'process_identity', return_value=None):
            self.assertFalse(slot.recover_unlaunched_marker(self.sync, self.queue, marker, ['1 bb-render']))
            record['child_pid'] = 123
            path.write_text(json.dumps(record))
            self.assertFalse(slot.recover_unlaunched_marker(self.sync, self.queue, marker, []))
            record.pop('child_pid')
            record['error'] = 'unknown failure'
            path.write_text(json.dumps(record))
            self.assertFalse(slot.recover_unlaunched_marker(self.sync, self.queue, marker, []))
            record['error'] = 'Legacy slot conflict; command was not launched'
            path.write_text(json.dumps(record))
        with patch.object(slot, 'process_identity', return_value='gone'):
            self.assertFalse(slot.recover_unlaunched_marker(self.sync, self.queue, marker, []))
        original_record = path.read_bytes()
        with patch.object(slot, 'process_identity', return_value=None):
            self.assertTrue(slot.recover_unlaunched_marker(self.sync, self.queue, marker, []))
        self.assertTrue(self.sync.read_text().startswith('GPU SLOT: free'))
        self.assertTrue(self.sync.read_text().endswith('\nKeep history.\n'))
        self.assertEqual(path.read_bytes(), original_record)

    def test_compiler_at_finish_does_not_strand_marker(self):
        loader = LOADER.replace('m.busy_processes=lambda: []',
                                'calls=iter([[],[],["123 /usr/bin/clang"]]);'
                                'm.busy_processes=lambda: next(calls,[])')
        child = self.start('compiler at finish', [sys.executable, '-c', 'pass'], loader=loader)
        output = child.communicate(timeout=10)[0]
        self.assertEqual(child.returncode, 0, output)
        self.assertTrue(self.sync.read_text().startswith('GPU SLOT: free'))
        record, = self.records()
        self.assertEqual(record['state'], 'done')
        self.assertNotIn('release_blocked_by', record)
        self.assertIn('child_identity', record)

    def test_postclaim_compiler_retry_keeps_fifo_place(self):
        events = self.directory / 'events'
        def command(label):
            return [sys.executable, '-c', 'from pathlib import Path;'
                    f'Path({str(events)!r}).open("a").write({label!r}+"\\n")']
        loader = LOADER.replace('m.busy_processes=lambda: []',
                                'calls=iter([[],["123 /usr/bin/clang"],[],[],[]]);'
                                'm.busy_processes=lambda: next(calls,[])')
        first = self.start('first retry', command('first'), loader=loader)
        deadline = time.monotonic() + 5
        while not list(self.queue.glob('*.json')):
            self.assertLess(time.monotonic(), deadline)
            time.sleep(.02)
        second = self.start('second', command('second'))
        first_output = first.communicate(timeout=20)[0]
        second_output = second.communicate(timeout=20)[0]
        self.assertEqual(first.returncode, 0, first_output)
        self.assertEqual(second.returncode, 0, second_output)
        self.assertEqual(events.read_text().splitlines(), ['first', 'second'])
        records = self.records()
        self.assertEqual(len(records), 2)
        self.assertEqual([record['state'] for record in records], ['done', 'done'])
        self.assertEqual(len(records[0]['prelaunch_conflicts']), 1)
        self.assertIn('Retrying ' + records[0]['ticket'] + ' in place', first_output)

    def test_live_renderer_blocks_release_then_completed_claim_recovers(self):
        loader = LOADER.replace('m.busy_processes=lambda: []',
                                'calls=iter([[],[],["123 /run/bb-render"]]);'
                                'm.busy_processes=lambda: next(calls,[])')
        child = self.start('leftover renderer', [sys.executable, '-c', 'pass'], loader=loader)
        output = child.communicate(timeout=10)[0]
        self.assertEqual(child.returncode, 0, output)
        marker = self.sync.read_text().splitlines()[0]
        self.assertTrue(marker.startswith('GPU SLOT: BUSY'))
        record, = self.records()
        self.assertEqual(record['release_blocked_by'], ['123 /run/bb-render'])
        with slot.locked(self.queue / 'queue.lock'):
            self.assertFalse(slot.recover_completed_marker(self.sync, self.queue, marker,
                                                           ['123 /run/bb-render']))
            self.assertTrue(slot.recover_completed_marker(self.sync, self.queue, marker,
                                                          ['456 /usr/bin/clang']))
        self.assertTrue(self.sync.read_text().startswith('GPU SLOT: free'))
        self.assertIn('marker_recovered', self.records()[0])

    def test_completed_recovery_requires_claim_and_dead_child_evidence(self):
        self.queue.mkdir()
        ticket = '01791840086104504000-aefadc7a'
        marker = f'GPU SLOT: BUSY — test, done [queue {ticket}]'
        self.sync.write_text(marker + '\nKeep history.\n')
        record = {'ticket': ticket, 'owner': 'test', 'label': 'done', 'state': 'done',
                  'pid': 100, 'identity': 'owner-start', 'child_pid': 200,
                  'child_identity': 'child-start', 'ended': 'now', 'exit_code': 0}
        path = self.queue / (ticket + '.json')
        def save(**changes):
            path.write_text(json.dumps(dict(record, **changes)))
        save()
        with patch.object(slot, 'process_identity', side_effect=lambda pid: {100: 'owner-start'}.get(pid)):
            self.assertFalse(slot.recover_completed_marker(self.sync, self.queue, marker, []))
        with patch.object(slot, 'process_identity', side_effect=lambda pid: {200: 'child-start'}.get(pid)):
            self.assertFalse(slot.recover_completed_marker(self.sync, self.queue, marker, []))
        with patch.object(slot, 'process_identity', return_value=None):
            for changes in ({'label': 'different'}, {'state': 'abandoned'}, {'ended': None},
                            {'exit_code': None}, {'child_pid': None}, {'identity': None}):
                save(**changes)
                self.assertFalse(slot.recover_completed_marker(self.sync, self.queue, marker, []), changes)
            save()
            self.assertFalse(slot.recover_completed_marker(self.sync, self.queue, marker,
                                                           ['201 benchmark-run.py']))
        # Old records have no child start time: even a reused, live PID prevents recovery.
        save(child_identity=None)
        with patch.object(slot, 'process_identity', side_effect=lambda pid: {200: 'reused-child'}.get(pid)):
            self.assertFalse(slot.recover_completed_marker(self.sync, self.queue, marker, []))
        with patch.object(slot, 'process_identity', return_value=None):
            self.assertTrue(slot.recover_completed_marker(self.sync, self.queue, marker, []))
        self.sync.write_text(marker + '\nKeep history.\n')
        with patch.object(slot, 'process_identity', side_effect=lambda pid: {200: 'reused-child'}.get(pid)):
            save()
            self.assertTrue(slot.recover_completed_marker(self.sync, self.queue, marker, []))
        self.assertTrue(self.sync.read_text().endswith('\nKeep history.\n'))

    def test_live_benchmark_wrapper_prevents_release(self):
        loader = LOADER.replace('m.busy_processes=lambda: []',
                                'calls=iter([[],[],["123 benchmark-run.py"]]);'
                                'm.busy_processes=lambda: next(calls,[])')
        child = self.start('leftover harness', [sys.executable, '-c', 'pass'], loader=loader)
        output = child.communicate(timeout=10)[0]
        self.assertEqual(child.returncode, 0, output)
        self.assertTrue(self.sync.read_text().startswith('GPU SLOT: BUSY'))
        self.assertEqual(self.records()[0]['release_blocked_by'], ['123 benchmark-run.py'])

    def test_timed_out_prelaunch_claim_recovers_only_without_spawn_intent(self):
        self.queue.mkdir()
        ticket = '01791840086104504000-aefadc7a'
        marker = f'GPU SLOT: BUSY — test, retry [queue {ticket}]'
        self.sync.write_text(marker + '\nKeep history.\n')
        record = {'state': 'cancelled', 'pid': 99999999, 'identity': 'gone', 'claim': marker,
                  'launch_attempted': True, 'error': 'Queue timeout after pre-launch conflict'}
        path = self.queue / (ticket + '.json')
        path.write_text(json.dumps(record))
        with patch.object(slot, 'process_identity', return_value=None):
            self.assertFalse(slot.recover_unlaunched_marker(self.sync, self.queue, marker, []))
            record['launch_attempted'] = False
            path.write_text(json.dumps(record))
            self.assertTrue(slot.recover_unlaunched_marker(self.sync, self.queue, marker, []))

    def test_launch_and_release_process_classification(self):
        processes = ('101 S /usr/bin/clang\n102 S /run/bb-render\n103 Z /run/bb-probe\n'
                     '104 S /usr/bin/python3\n105 S /usr/bin/python3\n106 S /usr/bin/python3\n')
        arguments = ('101 clang -c source.cpp\n102 /run/bb-render\n103 /run/bb-probe\n'
                     '104 python3 /run/benchmark-run.py --binary /run/bb-probe\n'
                     '105 python3 /run/benchmark-slot.py -- python3 /run/benchmark-run.py\n'
                     '106 python3 -c "print(1)" /run/benchmark-run.py\n')
        with patch.object(slot.subprocess, 'check_output', side_effect=[processes, arguments]):
            blockers = slot.busy_processes()
        self.assertEqual(blockers, ['101 /usr/bin/clang', '102 /run/bb-render', '104 benchmark-run.py'])
        self.assertEqual(slot.release_blockers(blockers), ['102 /run/bb-render', '104 benchmark-run.py'])
        self.assertEqual(slot.benchmark_script('python3 -u "/path with spaces/camera-benchmark.py"'),
                         'camera-benchmark.py')


if __name__ == '__main__':
    unittest.main()
