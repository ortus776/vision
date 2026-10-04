"""Regression tests for FPS math, swap chains, exact clocks and collector attribution."""
import csv
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from analyze_experiment import Intervals, analyze, compare, load_frames, metrics
from run_experiment import write_json


class ExperimentAnalysis(unittest.TestCase):
    def test_windows_metadata_reader_lock_retried(self):
        original_replace = Path.replace
        attempts = []
        def replace(source, target):
            attempts.append(source)
            if len(attempts) == 1:
                raise PermissionError('temporary Windows reader lock')
            return original_replace(source, target)
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'run.json'
            with patch.object(Path, 'replace', replace), patch('run_experiment.time.sleep'):
                write_json(path, {'status': 'recording'})
            self.assertEqual(json.loads(path.read_text()), {'status': 'recording'})
            self.assertEqual(len(attempts), 2)
    def fixture(self, directory, variant='baseline', warmup=0):
        # Counters above double precision are deliberate. 10, 10, 40, 20 ms intervals.
        begin = 2**54 + 17
        run = dict(run_id='fixture', variant=variant, status='complete', target={'process_id': 42},
                   qpc_frequency=1000, measurement_start_qpc=begin, measurement_end_qpc=begin + 80,
                   warmup_seconds=warmup, roi_width=640, roi_height=640, periodic_ms=5000)
        (directory / 'run.json').write_text(json.dumps(run), encoding='utf-8')
        with (directory / 'game_frames.csv').open('w', newline='', encoding='utf-8-sig') as file:
            writer = csv.DictWriter(file, fieldnames=['ProcessID', 'SwapChainAddress', 'CPUStartQPC', 'CPUBusy', 'GPUBusy', 'PresentMode'])
            writer.writeheader()
            for offset in (0, 10, 20, 60, 80):
                writer.writerow(dict(ProcessID=42, SwapChainAddress='main', CPUStartQPC=begin + offset,
                                     CPUBusy='1.2', GPUBusy='NA', PresentMode='Composed: Flip'))
            writer.writerow(dict(ProcessID=42, SwapChainAddress='overlay', CPUStartQPC=begin))
            writer.writerow(dict(ProcessID=77, SwapChainAddress='main', CPUStartQPC=begin))
            writer.writerow(dict(ProcessID=42, SwapChainAddress='main', CPUStartQPC='NaN'))
        if variant != 'baseline':
            session = directory / 'collector/session_fixture'
            session.mkdir(parents=True)
            (session / 'session.json').write_text(json.dumps({'metadata': {'qpc_frequency': 1000}}), encoding='utf-8')
            trace = [dict(type='collector_started', qpc=begin), dict(type='collector_stopped', qpc=begin + 80),
                     dict(type='collection_status', state='collecting', qpc=begin),
                     dict(type='collection_status', state='user_paused', qpc=begin + 20),
                     dict(type='collection_status', state='collecting', qpc=begin + 60),
                     dict(type='span', stage='capture', start_qpc=begin + 10, end_qpc=begin + 20,
                          map_start_qpc=begin + 12, map_end_qpc=begin + 17, outcome='frame', requests=[{'event_id': 7}]),
                     dict(type='span', stage='png', start_qpc=begin + 10, end_qpc=begin + 20, frame_id=99)]
            for filename, records in [('collector_trace.jsonl', trace),
                                      ('events.jsonl', [{'type': 'session_finished', 'trace_dropped': 0}]),
                                      ('frames.jsonl', [{'frame_id': 99, 'path': 'images/screenshot.png'}])]:
                (session / filename).write_text('\n'.join(json.dumps(record) for record in records) + '\n', encoding='utf-8')
        return begin

    def test_fps_not_mean_of_instant_fps(self):
        result = metrics([{'frame_ms': t} for t in (10, 10, 40, 20)])
        self.assertEqual(result['average_fps'], 50)
        self.assertEqual(result['p99_ms'], 39.4)
        self.assertEqual(result['low_1_percent_fps'], 25)
        self.assertEqual(result['over_33_33_ms'], 1)

    def test_baseline_precision_and_swapchain(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            begin = self.fixture(directory)
            result = analyze(directory)
            self.assertEqual(result['measurement']['average_fps'], 50)
            self.assertEqual(result['diagnostics']['selected_swap_chain'], 'main')
            self.assertEqual(result['diagnostics']['rejected_rows'],
                             {'other_pid': 1, 'invalid_qpc_or_pid': 1, 'other_swap_chains': 1})
            with (directory / 'aligned_frames.csv').open(encoding='utf-8') as file:
                rows = list(csv.DictReader(file))
            self.assertEqual(int(rows[0]['start_qpc']), begin)
            self.assertEqual([float(row['frame_ms']) for row in rows], [10, 10, 40, 20])
            self.assertEqual(rows[0]['gpu_busy_ms'], '')
            compare(directory)
            self.assertTrue((directory / 'comparison.csv').is_file())

    def test_pause_and_exact_boundary_stage_attribution(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            self.fixture(directory, 'png-store')
            result = analyze(directory)
            self.assertEqual(result['measurement']['frames'], 3)
            self.assertEqual(result['measurement']['average_fps'], 75)
            self.assertEqual(result['states']['user_paused']['frames'], 1)
            self.assertEqual(result['stages']['png']['overlap']['frames'], 1)
            self.assertEqual(result['stages']['png']['no_overlap']['frames'], 2)
            with (directory / 'aligned_frames.csv').open(encoding='utf-8') as file:
                rows = list(csv.DictReader(file))
            self.assertEqual([float(row['png_overlap_ms']) for row in rows], [0, 10, 0, 0])
            self.assertEqual(rows[1]['png_paths'], 'collector/session_fixture/images/screenshot.png')
            self.assertEqual(rows[1]['capture_event_ids'], '7')
            self.assertEqual(float(rows[1]['map_overlap_ms']), 5)

    def test_warmup_excludes_straddling_frames(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            self.fixture(directory, warmup=0.015)
            result = analyze(directory)
            self.assertEqual(result['measurement']['frames'], 2)
            self.assertAlmostEqual(result['measurement']['average_fps'], 33.333)

    def test_state_transition_frame_is_excluded(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            begin = self.fixture(directory, 'held')
            path = directory / 'collector/session_fixture/collector_trace.jsonl'
            records = [json.loads(line) for line in path.read_text().splitlines()]
            records[3]['qpc'] = begin + 15
            path.write_text('\n'.join(json.dumps(record) for record in records), encoding='utf-8')
            result = analyze(directory)
            self.assertEqual(result['measurement']['frames'], 2)
            self.assertEqual(result['states']['transition']['frames'], 1)

    def test_union_does_not_double_count_nested_spans(self):
        index = Intervals([{'start_qpc': a, 'end_qpc': b} for a, b in [(10, 20), (12, 16), (17, 30), (40, 50)]])
        self.assertEqual(index.overlap(0, 60), 30)
        self.assertEqual(index.overlap(20, 40), 10)
        self.assertEqual(index.overlap(30, 40), 0)
        self.assertEqual(len(index.matching(16, 17)), 1)

    def test_empty_or_wrong_clock_csv_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'frames.csv'
            for contents in ['ProcessID,SwapChainAddress,CPUStartQPC\n', 'ProcessID,TimeInSeconds\n42,1.2\n']:
                path.write_text(contents, encoding='utf-8')
                with self.assertRaises(ValueError):
                    load_frames(path, 42, 1000)


if __name__ == '__main__':
    unittest.main()
