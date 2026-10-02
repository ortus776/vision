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
    def test_scheduler_ranges_and_wic_png(self):
        for scenario, requested, skipped in [('late', 3600000, 3599999), ('overflow', 60000, 59998)]:
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


if __name__ == '__main__':
    unittest.main()
