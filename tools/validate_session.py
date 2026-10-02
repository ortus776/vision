"""Read-only integrity/audit check of one raw collector session (no annotation checks)."""
import argparse
import collections
import json
from pathlib import Path
import struct
import zlib


def png(path):
    data = path.read_bytes()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('invalid PNG signature')
    at = 8
    compressed = bytearray()
    width = height = None
    ended = False
    while at < len(data):
        if at + 12 > len(data):
            raise ValueError('truncated PNG chunk')
        size = struct.unpack_from('>I', data, at)[0]
        kind = data[at + 4:at + 8]
        body = data[at + 8:at + 8 + size]
        if at + size + 12 > len(data):
            raise ValueError('truncated PNG data')
        crc = struct.unpack_from('>I', data, at + 8 + size)[0]
        if zlib.crc32(kind + body) & 0xffffffff != crc:
            raise ValueError('PNG CRC mismatch')
        if kind == b'IHDR':
            width, height, depth, color, compression, filtering, interlace = struct.unpack('>IIBBBBB', body)
            if depth != 8 or color not in (2, 6) or compression or filtering or interlace:
                raise ValueError('unsupported PNG encoding for collector output')
        elif kind == b'IDAT':
            compressed.extend(body)
        elif kind == b'IEND':
            ended = True
        at += size + 12
    if not ended or not width or not height:
        raise ValueError('incomplete PNG')
    raw = zlib.decompress(compressed)
    channels = 4 if color == 6 else 3
    stride = width * channels + 1
    if len(raw) != stride * height or any(raw[row * stride] > 4 for row in range(height)):
        raise ValueError('invalid PNG scanlines')
    return width, height


def lines(path):
    records = []
    with path.open(encoding='utf-8') as stream:
        for number, line in enumerate(stream, 1):
            if not line.endswith('\n'):
                raise ValueError(f'{path.name}:{number}: unterminated JSONL record')
            try:
                records.append(json.loads(line))
            except json.JSONDecodeError as error:
                raise ValueError(f'{path.name}:{number}: {error}') from error
    return records


def validate(directory):
    session = json.loads((directory / 'session.json').read_text(encoding='utf-8'))
    frames = lines(directory / 'frames.jsonl')
    events = lines(directory / 'events.jsonl')
    errors = []
    paths = set()
    ids = set()
    outcomes = collections.Counter()
    requested = set()
    for event in events:
        for request in event.get('requests', []):
            ident = request['event_id']
            if event['type'] == 'requested':
                if ident in requested:
                    errors.append(f'duplicate requested event {ident}')
                requested.add(ident)
            if event['type'] in ('skipped', 'cancelled', 'write_failed'):
                outcomes[ident] += 1
    for frame in frames:
        relative = frame['path']
        path = (directory / relative).resolve()
        if not path.is_relative_to(directory.resolve()):
            errors.append(f'path escapes session: {relative}')
            continue
        if relative in paths or frame['frame_id'] in ids:
            errors.append(f'duplicate frame or path: {relative}')
        paths.add(relative)
        ids.add(frame['frame_id'])
        if frame['session_id'] != session['session_id']:
            errors.append(f'wrong session_id: {relative}')
        try:
            dimensions = png(path)
            if dimensions != (frame['roi']['width'], frame['roi']['height']):
                errors.append(f'wrong PNG dimensions: {relative}')
        except (OSError, ValueError, zlib.error) as error:
            errors.append(f'{relative}: {error}')
        for request in frame['requests']:
            outcomes[request['event_id']] += 1
            lateness = frame['captured_ms'] - request['planned_ms']
            limit = session['metadata'].get('max_lateness_ms', 100)
            if not 0 <= lateness <= limit:
                errors.append(f'frame outside request deadline: {relative}')
    for path in directory.rglob('*.tmp'):
        errors.append(f'leftover temporary file: {path.relative_to(directory)}')
    for path in (directory / 'images').glob('*.png'):
        if path.relative_to(directory).as_posix() not in paths:
            errors.append(f'orphaned PNG: {path.name}')
    for ident in requested | set(outcomes):
        if ident not in requested or outcomes[ident] != 1:
            errors.append(f'event {ident}: requested={ident in requested}, terminal outcomes={outcomes[ident]}')
    finished = [e for e in events if e['type'] == 'session_finished']
    if len(finished) != 1 or finished[0].get('saved') != len(frames):
        errors.append('missing/inconsistent session_finished summary')
    for error in errors:
        print(f'ERROR: {error}')
    if not frames:
        if not requested:
            print('WARNING: empty session; no capture requests were generated. '
                  'Check collection_status/control events, F8 and selected-window focus.')
        else:
            print('WARNING: no images were saved. Inspect skipped/cancelled/write_failed events.')
    print(f'{directory}: frames={len(frames)}, requests={len(requested)}, errors={len(errors)}')
    return not errors


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('session', type=Path)
    args = parser.parse_args()
    try:
        raise SystemExit(0 if validate(args.session) else 1)
    except (OSError, ValueError, KeyError, struct.error) as error:
        print(f'ERROR: {error}')
        raise SystemExit(1)
