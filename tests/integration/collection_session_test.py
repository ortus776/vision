"""Validate real C++ scheduler/writer/WIC PNG output without desktop access."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

root = Path(__file__).resolve().parents[2]
executable = Path(sys.argv.pop(1)).resolve()
spec = importlib.util.spec_from_file_location('validator', root / 'tools/validate_session.py')
validator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(validator)


class RealCollectionSession(unittest.TestCase):
    def test_long_windows_experiment_path(self):
        with tempfile.TemporaryDirectory(prefix='long-experiment-', dir=root / 'build') as temp:
            output = Path(temp) / ('experiment_' + 'x' * 85) / ('run_' + 'y' * 85)
            for scenario in ('late', 'store'):
                subprocess.run([str(executable), str(output / scenario), scenario], cwd=root, check=True,
                               capture_output=True, timeout=10)
                session = next((output / scenario).glob('session_*'))
                image = next((session / 'images').glob('*.png'))
                self.assertGreater(len(str(image.resolve())), 260)
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertTrue(validator.validate(session))

    def test_stored_png_block_boundaries_and_errors(self):
        from png_checks import read_png
        with tempfile.TemporaryDirectory(prefix='stored-png-', dir=root / 'build') as temp:
            directory = Path(temp)
            subprocess.run([str(executable), str(directory), 'store-boundaries'], cwd=root,
                           check=True, capture_output=True, timeout=10)
            self.assertFalse((directory / 'invalid.png').exists())
            for height in (1, 13106, 13107, 13108, 26214):
                width, decoded_height, channels, raw = read_png(directory / f'{height}.png')
                self.assertEqual((width, decoded_height, channels), (1, height, 4))
                expected = bytearray()
                for y in range(height):
                    b, g, r, a = ((y * 4 + c) % 256 for c in range(4))
                    expected.extend((0, r, g, b, a))
                self.assertEqual(raw, expected)

    def test_scheduler_ranges_and_wic_png(self):
        for scenario, requested, skipped in [('late', 3600000, 3599999), ('overflow', 60000, 59998), ('store', 3600000, 3599999)]:
            with self.subTest(scenario=scenario), tempfile.TemporaryDirectory(prefix='real-collection-', dir=root / 'build') as temp:
                directory = Path(temp).resolve()
                self.assertTrue(directory.is_relative_to((root / 'build').resolve()))
                subprocess.run([str(executable), str(directory), scenario], cwd=root, check=True, capture_output=True, timeout=10)
                sessions = list(directory.glob('session_*'))
                self.assertEqual(len(sessions), 1)
                session = sessions[0]
                metadata = json.loads((session / 'session.json').read_text(encoding='utf-8'))
                self.assertEqual(metadata['schema_version'], 2)
                events = [json.loads(line) for line in (session / 'events.jsonl').read_text(encoding='utf-8').splitlines()]
                ranges = [event for event in events if event['type'] == 'timer_skipped_range']
                self.assertEqual(len(ranges), 1)
                self.assertEqual(ranges[0]['count'], skipped)
                self.assertEqual(events[-1]['requested'], requested)
                self.assertLess(len(events), 10)
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertTrue(validator.validate(session))
                if scenario == 'store':
                    trace = [json.loads(line) for line in (session / 'collector_trace.jsonl').read_text(encoding='utf-8').splitlines()]
                    self.assertEqual([span['stage'] for span in trace], ['writer_queue', 'png', 'publish'])
                    for span in trace:
                        self.assertGreater(span['start_qpc'], 0)
                        self.assertGreaterEqual(span['end_qpc'], span['start_qpc'])
                        self.assertEqual(span['frame_id'], 1)
                        self.assertTrue(span['requests'])
                    self.assertEqual(trace[0]['end_qpc'], trace[1]['start_qpc'])
                    self.assertEqual(trace[1]['end_qpc'], trace[2]['start_qpc'])
                    from png_checks import read_png
                    png = next((session / 'images').glob('*.png'))
                    width, height, channels, scanlines = read_png(png)
                    self.assertEqual((width, height, channels), (2, 2, 4))
                    self.assertEqual(scanlines, bytes([0,255,0,0,255,0,255,0,128,
                                                      0,0,0,255,64,11,22,33,0]))
                else:
                    self.assertFalse((session / 'collector_trace.jsonl').exists())


if __name__ == '__main__':
    unittest.main()
