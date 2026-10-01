"""Exercise raw-session validation with valid, corrupt and interrupted fixtures."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import tempfile
import zlib

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('validate_session', root / 'tools/validate_session.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)


def valid(directory):
    with contextlib.redirect_stdout(io.StringIO()):
        try:
            return module.validate(directory)
        except (ValueError, KeyError, OSError):
            return False


build = root / 'build'
build.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix='session-validator-', dir=build) as temp:
    directory = Path(temp).resolve()
    assert directory.is_relative_to(build.resolve())
    (directory / 'images').mkdir()
    image = directory / 'images/frame_00000001.png'
    image.write_bytes(b'\x89PNG\r\n\x1a\n' +
        chunk(b'IHDR', struct.pack('>IIBBBBB', 2, 2, 8, 6, 0, 0, 0)) +
        chunk(b'IDAT', zlib.compress((b'\x00' + b'\xff' * 8) * 2)) + chunk(b'IEND', b''))
    request = {'event_id': 1, 'burst_id': 0, 'burst_index': 0, 'planned_ms': 5000, 'reason': 'timer'}
    session = {'session_id': 'test', 'metadata': {'max_lateness_ms': 100}}
    frame = {'session_id': 'test', 'frame_id': 1, 'path': 'images/frame_00000001.png',
             'captured_ms': 5001, 'roi': {'width': 2, 'height': 2}, 'requests': [request]}
    event = {'type': 'requested', 'requests': [request]}
    finish = {'type': 'session_finished', 'saved': 1}
    (directory / 'session.json').write_text(json.dumps(session), encoding='utf-8')
    frames = directory / 'frames.jsonl'
    events = directory / 'events.jsonl'
    frames.write_text(json.dumps(frame) + '\n', encoding='utf-8')
    events.write_text(json.dumps(event) + '\n' + json.dumps(finish) + '\n', encoding='utf-8')
    assert valid(directory), 'valid fixture should pass'
    original = image.read_bytes()
    image.write_bytes(original[:-5])
    assert not valid(directory), 'truncated PNG should fail'
    image.write_bytes(original)
    leftover = directory / 'images/frame_2.png.tmp'
    leftover.write_bytes(b'partial')
    assert not valid(directory), 'leftover temp should fail'
    leftover.unlink()
    frames.write_text(json.dumps(frame), encoding='utf-8')
    assert not valid(directory), 'unterminated JSONL should fail'
    frames.write_text(json.dumps(frame) + '\n', encoding='utf-8')
    events.write_text(json.dumps(event) + '\n' + json.dumps({'type': 'skipped', 'requests': [request]}) +
                      '\n' + json.dumps(finish) + '\n', encoding='utf-8')
    assert not valid(directory), 'duplicate terminal outcome should fail'
    events.write_text(json.dumps(event) + '\n', encoding='utf-8')
    assert not valid(directory), 'unfinished session should fail'
    events.write_text(json.dumps(event) + '\n' + json.dumps(finish) + '\n', encoding='utf-8')
    frame['path'] = '../outside.png'
    frames.write_text(json.dumps(frame) + '\n', encoding='utf-8')
    assert not valid(directory), 'escaping file path should fail'
print('Session validator fixtures passed')
