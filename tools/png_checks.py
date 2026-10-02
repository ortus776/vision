"""Strict, bounded checker for non-interlaced 8-bit RGB/RGBA collector PNGs."""
from pathlib import Path
import struct
import zlib

MAX_PIXELS = 16 * 1024 * 1024
MAX_FILE_BYTES = 128 * 1024 * 1024


def read_png(path, expected_size=None):
    path = Path(path)
    if path.stat().st_size > MAX_FILE_BYTES:
        raise ValueError('PNG exceeds file size limit')
    with path.open('rb') as stream:
        data = stream.read(MAX_FILE_BYTES + 1)
    if len(data) > MAX_FILE_BYTES or data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('invalid or excessive PNG')
    at, width, height, channels = 8, None, None, None
    compressed = bytearray()
    saw_idat = closed_idat = saw_palette = ended = False
    while at < len(data):
        if at + 12 > len(data):
            raise ValueError('truncated PNG chunk')
        size = struct.unpack_from('>I', data, at)[0]
        kind = data[at + 4:at + 8]
        if not all(65 <= c <= 90 or 97 <= c <= 122 for c in kind) or kind[2] & 32:
            raise ValueError('invalid PNG chunk name')
        end = at + size + 12
        if end > len(data):
            raise ValueError('truncated PNG data')
        body = data[at + 8:at + 8 + size]
        crc = struct.unpack_from('>I', data, at + 8 + size)[0]
        if zlib.crc32(body, zlib.crc32(kind)) & 0xffffffff != crc:
            raise ValueError('PNG CRC mismatch')
        if width is None and kind != b'IHDR':
            raise ValueError('IHDR must be the first PNG chunk')
        if kind == b'IHDR':
            if width is not None or size != 13:
                raise ValueError('duplicate or malformed IHDR')
            width, height, depth, color, compression, filtering, interlace = struct.unpack('>IIBBBBB', body)
            if not width or not height or width * height > MAX_PIXELS:
                raise ValueError('PNG dimensions exceed pixel limit')
            if expected_size is not None and (width, height) != expected_size:
                raise ValueError('wrong PNG dimensions')
            if depth != 8 or color not in (2, 6) or compression or filtering or interlace:
                raise ValueError('unsupported PNG encoding for collector output')
            channels = 4 if color == 6 else 3
        elif kind == b'PLTE':
            if saw_palette or saw_idat or size == 0 or size > 768 or size % 3:
                raise ValueError('invalid PNG palette')
            saw_palette = True
        elif kind == b'IDAT':
            if closed_idat:
                raise ValueError('PNG IDAT chunks must be consecutive')
            saw_idat = True
            compressed.extend(body)
        elif kind == b'IEND':
            if size or not saw_idat or end != len(data):
                raise ValueError('invalid IEND or trailing PNG data')
            ended = True
        elif not kind[0] & 32:
            raise ValueError('unsupported critical PNG chunk')
        if saw_idat and kind != b'IDAT':
            closed_idat = True
        at = end
    if not ended:
        raise ValueError('incomplete PNG')
    stride = width * channels + 1
    expected_bytes = stride * height
    decoder = zlib.decompressobj()
    raw = decoder.decompress(compressed, expected_bytes + 1)
    if (len(raw) != expected_bytes or not decoder.eof or decoder.unconsumed_tail or
            decoder.unused_data or any(raw[row * stride] > 4 for row in range(height))):
        raise ValueError('invalid PNG scanlines or compressed stream')
    return width, height, channels, raw
