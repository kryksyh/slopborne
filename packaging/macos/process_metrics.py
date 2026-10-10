"""Read-only macOS process accounting for game benchmarks (no GPU synchronization)."""
import ctypes
import datetime
import errno
import json
import statistics
import sys
import threading
import time


class RUsageInfoV4(ctypes.Structure):
    # sys/resource.h, RUSAGE_INFO_V4. Keep the ABI order, including unused fields.
    _fields_ = [('uuid', ctypes.c_uint8 * 16)] + [
        (name, ctypes.c_uint64) for name in (
            'user_time system_time pkg_idle_wkups interrupt_wkups pageins wired_size '
            'resident_size phys_footprint proc_start_abstime proc_exit_abstime '
            'child_user_time child_system_time child_pkg_idle_wkups child_interrupt_wkups '
            'child_pageins child_elapsed_abstime diskio_bytesread diskio_byteswritten '
            'cpu_time_qos_default cpu_time_qos_maintenance cpu_time_qos_background '
            'cpu_time_qos_utility cpu_time_qos_legacy cpu_time_qos_user_initiated '
            'cpu_time_qos_user_interactive billed_system_time serviced_system_time '
            'logical_writes lifetime_max_phys_footprint instructions cycles billed_energy '
            'serviced_energy interval_max_phys_footprint runnable_time'
        ).split()]


class ProcessAccounting:
    def __init__(self, pid):
        self.pid = pid
        self.started = None
        self.lib = ctypes.CDLL('/usr/lib/libproc.dylib', use_errno=True)
        self.lib.proc_pid_rusage.argtypes = [ctypes.c_int, ctypes.c_int,
                                            ctypes.POINTER(RUsageInfoV4)]
        self.lib.proc_pid_rusage.restype = ctypes.c_int

    def sample(self):
        info = RUsageInfoV4()
        if self.lib.proc_pid_rusage(self.pid, 4, ctypes.byref(info)):
            error = ctypes.get_errno()
            raise OSError(error, 'proc_pid_rusage failed')
        if self.started is None:
            self.started = info.proc_start_abstime
        if info.proc_start_abstime != self.started:
            raise ProcessLookupError(errno.ESRCH, 'PID was reused')
        if info.proc_exit_abstime:
            raise ProcessLookupError(errno.ESRCH, 'Process exited')
        return {name: getattr(info, name) for name in (
            'resident_size', 'phys_footprint', 'wired_size', 'lifetime_max_phys_footprint',
            'diskio_bytesread', 'diskio_byteswritten', 'pageins', 'proc_exit_abstime')}


def summarize(samples):
    """Describe growth without labelling asset/cache warm-up as a memory leak."""
    recorded = len(samples)
    # macOS can return a terminal accounting record with zero physical footprint
    # before the PID disappears. It is not an observed release during gameplay.
    # Also filter old captures that did not yet record proc_exit_abstime.
    samples = [s for s in samples if not s.get('proc_exit_abstime', 0)
               and s['phys_footprint'] > 0]
    result = {'samples': len(samples), 'recorded_samples': recorded,
              'discarded_exit_or_unavailable_samples': recorded - len(samples),
              'memory_unit': 'bytes',
              'scope': 'Whole-process accounting; not a separate Metal allocation counter.',
              'interpretation': 'Growth can be asset/cache warm-up. Compare repeated routes and idle periods.'}
    if not samples:
        return result
    result['duration_seconds'] = samples[-1]['elapsed_seconds'] - samples[0]['elapsed_seconds']
    # Separate late-run trend from initial executable/asset loading.
    late = samples[len(samples) // 2:]
    for field in ('resident_size', 'phys_footprint', 'wired_size'):
        values = [s[field] for s in samples]
        window = min(5, len(late))
        metric = {'first': values[0], 'last': values[-1], 'peak': max(values),
                  'late_first_median': statistics.median(s[field] for s in late[:window]),
                  'late_last_median': statistics.median(s[field] for s in late[-window:])}
        if len(late) >= 10:
            xs = [s['elapsed_seconds'] for s in late]
            ys = [s[field] for s in late]
            mx, my = statistics.mean(xs), statistics.mean(ys)
            denom = sum((x - mx) ** 2 for x in xs)
            if denom:
                metric['late_bytes_per_minute'] = 60 * sum(
                    (x - mx) * (y - my) for x, y in zip(xs, ys)) / denom
        result[field] = metric
    return result


class ResourceMonitor:
    def __init__(self, pid, run, interval=1.0):
        self.pid, self.run, self.interval = pid, run, interval
        self.stop = threading.Event()
        self.thread = None
        self.samples = []

    def __enter__(self):
        if sys.platform == 'darwin':
            self.thread = threading.Thread(target=self._collect, daemon=True)
            self.thread.start()
        return self

    def _collect(self):
        started = time.monotonic()
        try:
            accounting = ProcessAccounting(self.pid)
            with (self.run / 'process-metrics.jsonl').open('x') as output:
                while not self.stop.is_set():
                    try:
                        sample = accounting.sample()
                    except ProcessLookupError:
                        break
                    sample.update(elapsed_seconds=time.monotonic() - started,
                                  time=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                                  pid=self.pid)
                    self.samples.append(sample)
                    output.write(json.dumps(sample) + '\n')
                    output.flush()
                    self.stop.wait(self.interval)
        except OSError as error:
            # Diagnostics must not terminate the game or controller.
            print('Process metrics unavailable:', error, file=sys.stderr, flush=True)

    def __exit__(self, *_):
        self.stop.set()
        if self.thread:
            self.thread.join(timeout=5)
            if not self.thread.is_alive():
                with (self.run / 'process-metrics-summary.json').open('x') as output:
                    json.dump(summarize(self.samples), output, indent=2)
                    output.write('\n')
