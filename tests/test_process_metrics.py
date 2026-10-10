"""Memory trends must not interpret terminal process accounting as leak recovery."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    'process_metrics', Path(__file__).resolve().parents[1] / 'packaging/macos/process_metrics.py')
metrics = importlib.util.module_from_spec(spec)
spec.loader.exec_module(metrics)


class MemorySummaryTest(unittest.TestCase):
    def samples(self):
        return [dict(elapsed_seconds=i, resident_size=2000 + i * 10,
                     phys_footprint=4000 + i * 20, wired_size=0) for i in range(30)]

    def test_exit_record_does_not_reverse_growth(self):
        samples = self.samples()
        expected = metrics.summarize(samples)
        for exit_record in [dict(samples[-1], elapsed_seconds=30, phys_footprint=0),
                            dict(samples[-1], elapsed_seconds=30, proc_exit_abstime=123)]:
            result = metrics.summarize(samples + [exit_record])
            self.assertEqual(result['samples'], 30)
            self.assertEqual(result['discarded_exit_or_unavailable_samples'], 1)
            self.assertEqual(result['duration_seconds'], 29)
            self.assertEqual(result['phys_footprint'], expected['phys_footprint'])
            self.assertEqual(result['phys_footprint']['late_bytes_per_minute'], 1200)
            self.assertEqual(result['wired_size']['last'], 0)

    def test_all_unavailable_has_no_trend(self):
        result = metrics.summarize([dict(self.samples()[0], phys_footprint=0)])
        self.assertEqual(result['samples'], 0)
        self.assertNotIn('phys_footprint', result)


if __name__ == '__main__':
    unittest.main()
