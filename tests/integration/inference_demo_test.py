"""Run the real executable and verify that mock boxes/points match rendered PNG pixels."""
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib

root = Path(__file__).resolve().parents[2]
executable = Path(sys.argv[1]).resolve()
build = root / 'build'
build.mkdir(exist_ok=True)


def read_png(path):
    data = path.read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    offset = 8
    compressed = bytearray()
    ended = False
    while offset < len(data):
        length = struct.unpack_from('>I', data, offset)[0]
        kind = data[offset + 4:offset + 8]
        body = data[offset + 8:offset + 8 + length]
        crc = struct.unpack_from('>I', data, offset + 8 + length)[0]
        assert zlib.crc32(kind + body) & 0xffffffff == crc
        if kind == b'IHDR':
            width, height, depth, color, compression, filtering, interlace = struct.unpack('>IIBBBBB', body)
            assert depth == 8 and color in (2, 6) and not compression and not filtering and not interlace
        elif kind == b'IDAT':
            compressed.extend(body)
        elif kind == b'IEND':
            ended = True
        offset += length + 12
    assert ended
    channels = 4 if color == 6 else 3
    raw = zlib.decompress(compressed)
    stride = width * channels
    assert len(raw) == (stride + 1) * height
    rows = []
    previous = bytearray(stride)
    for y in range(height):
        mode = raw[y * (stride + 1)]
        row = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        assert mode <= 4
        for x in range(stride):
            a = row[x - channels] if x >= channels else 0
            b = previous[x]
            c = previous[x - channels] if x >= channels else 0
            if mode == 1:
                predictor = a
            elif mode == 2:
                predictor = b
            elif mode == 3:
                predictor = (a + b) // 2
            elif mode == 4:
                p = a + b - c
                distances = abs(p-a), abs(p-b), abs(p-c)
                predictor = a if distances[0] <= distances[1] and distances[0] <= distances[2] else (
                    b if distances[1] <= distances[2] else c)
            else:
                predictor = 0
            row[x] = (row[x] + predictor) & 255
        rows.append(row)
        previous = row
    return width, height, channels, rows


def run(directory, style):
    subprocess.run([str(executable), '--inference-demo', '--roi-width', '160', '--roi-height', '96',
        '--input-width', '80', '--input-height', '80', '--demo-frames', '3', '--mock-seed', '42',
        '--overlay-style', style, '--output', str(directory)], cwd=root, check=True, capture_output=True,
        timeout=15)
    sessions = list(directory.glob('inference_demo_*'))
    assert len(sessions) == 1
    session = sessions[0]
    metadata = json.loads((session / 'demo.json').read_text(encoding='utf-8'))
    assert metadata['input_shape'] == [1, 3, 80, 80] and metadata['backend'] == 'mock'
    records = [json.loads(line) for line in (session / 'detections.jsonl').read_text(encoding='utf-8').splitlines()]
    assert len(records) == 3 and len(list((session / 'images').glob('*.png'))) == 3
    boxes = []
    for ident, record in enumerate(records, 1):
        assert record['frame_id'] == ident and record['completed_ms'] >= record['captured_ms']
        assert len(record['detections']) == 1
        box = record['detections'][0]['box']
        assert 0 <= box['left'] < box['right'] <= 160 and 0 <= box['top'] < box['bottom'] <= 96
        boxes.append(box)
        width, height, channels, rows = read_png(session / record['path'])
        assert (width, height) == (160, 96)
        left, top = math.floor(box['left']), math.floor(box['top'])
        right, bottom = math.ceil(box['right']), math.ceil(box['bottom'])
        cx, cy = left + (right-left)//2, top + (bottom-top)//2
        expected = set()
        for y in range(height):
            for x in range(width):
                inside = left <= x < right and top <= y < bottom
                painted = (inside and (x < left+2 or x >= right-2 or y < top+2 or y >= bottom-2)) if style == 'box' else (
                    (x-cx)**2 + (y-cy)**2 <= 9)
                if painted:
                    expected.add((x, y))
        actual = {(x, y) for y, row in enumerate(rows) for x in range(width)
                  if row[x*channels:x*channels+3] == bytes((0,255,0))}
        assert actual == expected and actual, f'{style} PNG pixels must agree with manifest coordinates'
    assert boxes[0] != boxes[1] != boxes[2], 'the mock must move the box on successive frames'


with tempfile.TemporaryDirectory(prefix='inference-demo-test-', dir=build) as temp:
    directory = Path(temp).resolve()
    assert directory.is_relative_to(build.resolve())
    run(directory / 'box', 'box')
    run(directory / 'point', 'point')
print('Real mock inference PNGs match box/point coordinates')
