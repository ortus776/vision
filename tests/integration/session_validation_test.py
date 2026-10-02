"""Regression fixtures for canonical requests, schema, counters and bounded PNGs."""
import contextlib
import copy
import importlib.util
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
import zlib

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('validate_session', root / 'tools/validate_session.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def chunk(kind, body):
    return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xffffffff)


def image_bytes(channels=4, raw=None):
    if raw is None:
        raw = (b'\x00' + b'\xff' * (2 * channels)) * 2
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 2, 2, 8, 6 if channels == 4 else 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


class SessionValidation(unittest.TestCase):
    def setUp(self):
        build = root / 'build'
        build.mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix='session-validator-', dir=build)
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name).resolve()
        self.assertTrue(self.directory.is_relative_to(build.resolve()))
        (self.directory / 'images').mkdir()
        self.image = self.directory / 'images/frame_00000001.png'
        self.original = image_bytes()
        self.image.write_bytes(self.original)
        self.request = dict(event_id=1, burst_id=0, burst_index=0, planned_ms=5000, reason='timer')
        self.session = dict(schema_version=2, session_id='test', metadata=dict(max_lateness_ms=100, periodic_interval_ms=1))
        self.frame = dict(session_id='test', frame_id=1, path='images/frame_00000001.png',
                          captured_ms=5001, roi=dict(width=2, height=2), requests=[copy.deepcopy(self.request)])
        self.event = dict(type='requested', requests=[copy.deepcopy(self.request)])
        self.finish = dict(type='session_finished', saved=1, requested=1, expired=0, pending_overflow=0, merged=0, writer_rejected=0)

    def valid(self, frames=None, events=None):
        frames = [self.frame] if frames is None else frames
        events = [self.event, self.finish] if events is None else events
        (self.directory / 'session.json').write_text(json.dumps(self.session), encoding='utf-8')
        (self.directory / 'frames.jsonl').write_text(''.join(json.dumps(f) + '\n' for f in frames), encoding='utf-8')
        (self.directory / 'events.jsonl').write_text(''.join(json.dumps(e) + '\n' for e in events), encoding='utf-8')
        self.output = io.StringIO()
        with contextlib.redirect_stdout(self.output):
            try:
                return module.validate(self.directory)
            except (OSError, ValueError, KeyError, TypeError, zlib.error):
                return False

    def test_valid_and_legacy_schema(self):
        self.assertTrue(self.valid())
        self.session['schema_version'] = 1
        self.assertTrue(self.valid())
        self.image.write_bytes(image_bytes(channels=3))
        self.assertTrue(self.valid(), 'RGB and RGBA WIC outputs are supported')

    def test_canonical_request_fields(self):
        for changes in (dict(planned_ms=9000), dict(burst_id=5, reason='click'), dict(burst_index=1), dict(reason='other')):
            with self.subTest(changes=changes):
                original = copy.deepcopy(self.frame)
                self.frame['requests'][0].update(changes)
                self.frame['captured_ms'] = self.frame['requests'][0]['planned_ms'] + 1
                self.assertFalse(self.valid())
                self.frame = original
        accepted = dict(type='writer_accepted', frame_id=1, requests=[dict(self.request, planned_ms=0)])
        self.assertFalse(self.valid(events=[self.event, accepted, self.finish]))

    def test_empty_or_unknown_frame_requests(self):
        self.frame['requests'] = []
        self.finish['requested'] = 0
        self.assertFalse(self.valid(events=[self.finish]))
        self.frame['requests'] = [dict(self.request, event_id=2)]
        self.assertFalse(self.valid())

    def test_summary_schema_and_types(self):
        self.finish['writer_failed'] = 0
        for key, value in (('saved', 9), ('requested', 999), ('requested', True), ('expired', 1), ('merged', 1), ('writer_rejected', 1), ('writer_failed', 1)):
            with self.subTest(key=key, value=value):
                original = self.finish[key]
                self.finish[key] = value
                self.assertFalse(self.valid())
                self.finish[key] = original
        for version in (True, 0, 3, '2'):
            self.session['schema_version'] = version
            self.assertFalse(self.valid())
        self.session['schema_version'] = 2
        for value in (0, True, '1'):
            self.frame['frame_id'] = value
            self.assertFalse(self.valid())

    def test_identity_deadline_and_duplicates(self):
        self.frame['session_id'] = 'foreign'
        self.assertFalse(self.valid())
        self.frame['session_id'] = 'test'
        self.frame['captured_ms'] = 5101
        self.assertFalse(self.valid())
        self.frame['captured_ms'] = 5000
        self.assertTrue(self.valid(), 'exact deadline is allowed')
        self.frame['captured_ms'] = 5100
        self.assertTrue(self.valid(), 'max lateness boundary is inclusive')
        self.assertFalse(self.valid(frames=[self.frame, self.frame]))
        self.assertFalse(self.valid(events=[self.event, self.event, self.finish]))
        self.assertFalse(self.valid(events=[self.event, dict(type='skipped', requests=[self.request]), self.finish]))
        self.assertFalse(self.valid(events=[self.event]))
        self.assertFalse(self.valid(events=[self.event, self.finish, dict(type='state')]))

    def test_empty_and_skipped_session(self):
        self.image.unlink()
        self.finish.update(saved=0, requested=0)
        self.assertTrue(self.valid(frames=[], events=[self.finish]))
        self.assertIn('no capture requests were generated', self.output.getvalue())
        self.finish['requested'] = 1
        self.assertTrue(self.valid(frames=[], events=[self.event, dict(type='skipped', requests=[self.request]), self.finish]))
        self.assertIn('no images were saved', self.output.getvalue())

    def test_merged_frame(self):
        second = dict(self.request, event_id=2)
        self.frame['requests'].append(second)
        self.finish.update(requested=2, merged=1)
        events = [self.event, dict(type='requested', requests=[second]),
                  dict(type='merged', requests=[self.request, second]), self.finish]
        self.assertTrue(self.valid(events=events), 'one PNG can satisfy two original requests')

    def test_path_tmp_orphan_and_jsonl(self):
        for path in ('../outside.png', 'images/../images/frame_00000001.png', 'images\\frame_00000001.png', 'C:/frame.png'):
            self.frame['path'] = path
            self.assertFalse(self.valid())
        self.frame['path'] = 'images/frame_00000001.png'
        orphan = self.directory / 'images/extra.png'
        orphan.write_bytes(self.original)
        self.assertFalse(self.valid())
        orphan.unlink()
        temporary = self.directory / 'images/partial.png.tmp'
        temporary.write_bytes(b'partial')
        self.assertFalse(self.valid())
        temporary.unlink()
        self.assertTrue(self.valid())
        path = self.directory / 'frames.jsonl'
        path.write_text(json.dumps(self.frame), encoding='utf-8')
        with self.assertRaises(ValueError):
            module.validate(self.directory)

    def test_png_structure_and_bounded_decompression(self):
        header = self.original[:33]
        compressed = zlib.compress((b'\x00' + b'\xff' * 8) * 2)
        variants = [self.original[:-5], self.original + b'trailing',
                    header + self.original[8:33] + self.original[33:],
                    self.original[:-12] + chunk(b'IEND', b'x'),
                    header + chunk(b'IEND', b''),
                    header + chunk(b'ABCD', b'') + self.original[33:],
                    header + chunk(b'IDAT', compressed[:4]) + chunk(b'tEXt', b'') + chunk(b'IDAT', compressed[4:]) + chunk(b'IEND', b''),
                    header + chunk(b'IDAT', compressed + zlib.compress(b'other')) + chunk(b'IEND', b''),
                    image_bytes(raw=b'\x00' * 65536), image_bytes(raw=b'\x05' + b'\xff' * 8 + b'\x00' + b'\xff' * 8)]
        corrupted = bytearray(self.original)
        corrupted[45] ^= 1
        variants.append(corrupted)
        for index, content in enumerate(variants):
            with self.subTest(index=index):
                self.image.write_bytes(content)
                self.assertFalse(self.valid())
        self.image.write_bytes(header + chunk(b'IDAT', compressed[:4]) + chunk(b'IDAT', compressed[4:]) + chunk(b'IEND', b''))
        self.assertTrue(self.valid(), 'consecutive IDAT chunks are valid')
        self.image.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 2**31, 2, 8, 6, 0, 0, 0)))
        with self.assertRaises(ValueError):
            module.read_png(self.image)

    def test_compact_timer_ranges(self):
        self.image.unlink()
        count = 3599899
        interval = dict(type='timer_skipped_range', first_event_id=1, count=count,
                        first_planned_ms=1, period_ms=1, at_ms=3600000, reason='late')
        self.finish.update(saved=0, requested=count, expired=count)
        self.assertTrue(self.valid(frames=[], events=[interval, self.finish]), 'millions of IDs stay compact')
        self.assertFalse(self.valid(frames=[], events=[interval, interval, self.finish]))
        bad = dict(interval, at_ms=count + 100)
        self.assertFalse(self.valid(frames=[], events=[bad, self.finish]), 'late range cannot contain fresh deadlines')
        self.session['schema_version'] = 1
        self.assertFalse(self.valid(frames=[], events=[interval, self.finish]))


if __name__ == '__main__':
    unittest.main()
