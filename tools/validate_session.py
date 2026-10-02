"""Read-only integrity/audit check of collector sessions, schema versions 1 and 2."""
import argparse
from bisect import bisect_right
import collections
import json
from pathlib import Path, PurePosixPath
import sys
import zlib

# Supports callers loading this file with importlib rather than as a package.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from png_checks import MAX_PIXELS, read_png


def png(path):
    return read_png(path)[:2]


def integer(value, name, minimum=0):
    if type(value) is not int or value < minimum:
        raise ValueError(f'{name} must be an integer >= {minimum}')
    return value


def object_record(value, name):
    if not isinstance(value, dict):
        raise ValueError(f'{name} must be an object')
    return value


def request_record(value):
    value = object_record(value, 'request')
    result = {key: integer(value[key], key, 1 if key == 'event_id' else 0)
              for key in ('event_id', 'burst_id', 'burst_index', 'planned_ms')}
    result['reason'] = value['reason']
    if result['reason'] not in ('click', 'timer'):
        raise ValueError('request reason must be click or timer')
    if result['reason'] == 'timer' and (result['burst_id'] or result['burst_index']):
        raise ValueError('timer requests must have zero burst_id and burst_index')
    if result['reason'] == 'click' and not result['burst_id']:
        raise ValueError('click request must have a positive burst_id')
    return result


def request_list(record, required=False):
    values = record.get('requests', [])
    if not isinstance(values, list) or (required and not values):
        raise ValueError('requests must be a nonempty array' if required else 'requests must be an array')
    result = [request_record(value) for value in values]
    if len({r['event_id'] for r in result}) != len(result):
        raise ValueError('duplicate event_id in requests array')
    return result


def lines(path):
    records = []
    with path.open(encoding='utf-8') as stream:
        for number, line in enumerate(stream, 1):
            if not line.endswith('\n'):
                raise ValueError(f'{path.name}:{number}: unterminated JSONL record')
            try:
                records.append(object_record(json.loads(line), 'JSONL record'))
            except ValueError as error:
                raise ValueError(f'{path.name}:{number}: {error}') from error
    return records


def validate(directory):
    directory = Path(directory).resolve()
    session = object_record(json.loads((directory / 'session.json').read_text(encoding='utf-8')), 'session')
    version = integer(session['schema_version'], 'schema_version', 1)
    if version not in (1, 2):
        raise ValueError('unsupported session schema_version')
    if not isinstance(session['session_id'], str) or not session['session_id']:
        raise ValueError('session_id must be a nonempty string')
    metadata = object_record(session['metadata'], 'metadata')
    limit = integer(metadata.get('max_lateness_ms', 100), 'max_lateness_ms')
    frames = lines(directory / 'frames.jsonl')
    events = lines(directory / 'events.jsonl')
    errors, paths, ids = [], set(), set()
    requested, outcomes, references, ranges = {}, collections.Counter(), [], []
    expired = overflow = merged = writer_rejected = writer_failed = cancelled_or_capture_skipped = 0
    for event in events:
        kind = event['type']
        if not isinstance(kind, str):
            raise ValueError('event type must be a string')
        if 'at_ms' in event:
            integer(event['at_ms'], 'event.at_ms')
        requests = request_list(event, kind in ('requested', 'skipped', 'cancelled', 'write_failed', 'writer_accepted', 'merged'))
        references.extend(requests)
        for request in requests:
            ident = request['event_id']
            if kind == 'requested':
                if ident in requested:
                    errors.append(f'duplicate requested event {ident}')
                requested[ident] = request
            if kind in ('skipped', 'cancelled', 'write_failed'):
                outcomes[ident] += 1
        if kind == 'skipped':
            if event.get('reason') == 'late':
                expired += len(requests)
            if event.get('reason') == 'pending_queue_full':
                overflow += len(requests)
            if event.get('reason') == 'writer_queue_full':
                writer_rejected += 1
            elif event.get('reason') not in ('late', 'pending_queue_full'):
                cancelled_or_capture_skipped += len(requests)
        if kind == 'write_failed' or (kind == 'cancelled' and event.get('reason') == 'writer_error'):
            writer_failed += 1
        elif kind == 'cancelled':
            cancelled_or_capture_skipped += len(requests)
        if kind == 'merged':
            if len(requests) < 2:
                errors.append('merged event must reference multiple requests')
            merged += len(requests) - 1
        if kind == 'timer_skipped_range':
            if version != 2 or requests:
                raise ValueError('timer ranges require schema 2 without requests array')
            first = integer(event['first_event_id'], 'first_event_id', 1)
            count = integer(event['count'], 'count', 1)
            planned = integer(event['first_planned_ms'], 'first_planned_ms')
            period = integer(event['period_ms'], 'period_ms', 1)
            at = integer(event['at_ms'], 'at_ms')
            last_planned = planned + (count - 1) * period
            if event.get('reason') not in ('late', 'pending_queue_full') or last_planned > at:
                raise ValueError('invalid timer range deadline or reason')
            if 'periodic_interval_ms' in metadata and period != metadata['periodic_interval_ms']:
                raise ValueError('timer range period differs from session metadata')
            if event['reason'] == 'late':
                if at - last_planned <= limit:
                    raise ValueError('late timer range contains a nonexpired request')
                expired += count
            else:
                overflow += count
            ranges.append((first, first + count - 1))
    ranges.sort()
    for previous, current in zip(ranges, ranges[1:]):
        if previous[1] >= current[0]:
            errors.append('overlapping timer event ranges')
    starts = [first for first, _ in ranges]
    for request in references:
        ident = request['event_id']
        index = bisect_right(starts, ident) - 1
        if index >= 0 and ident <= ranges[index][1]:
            errors.append(f'event {ident} also belongs to a skipped timer range')
        if ident not in requested or request != requested[ident]:
            errors.append(f'event {ident}: missing or changed canonical request')
    for frame in frames:
        ident = integer(frame['frame_id'], 'frame_id', 1)
        captured = integer(frame['captured_ms'], 'captured_ms')
        roi = object_record(frame['roi'], 'roi')
        size = tuple(integer(roi[key], f'roi.{key}', 1) for key in ('width', 'height'))
        if size[0] * size[1] > MAX_PIXELS:
            raise ValueError('frame ROI exceeds pixel limit')
        for key in ('left', 'top'):
            if key in roi and type(roi[key]) is not int:
                raise ValueError(f'roi.{key} must be an integer')
        for key in ('generation', 'captured_utc_ms', 'source_qpc'):
            if key in frame and frame[key] is not None:
                integer(frame[key], key, 1 if key == 'generation' else 0)
        if 'source_size' in frame:
            source = object_record(frame['source_size'], 'source_size')
            for axis, roi_extent in zip(('width', 'height'), size):
                if integer(source[axis], f'source_size.{axis}', 1) < roi_extent:
                    errors.append('ROI does not fit source_size')
        relative = frame['path']
        if not isinstance(relative, str):
            raise ValueError('frame path must be a string')
        local = PurePosixPath(relative)
        if ('\\' in relative or ':' in relative or local.is_absolute() or '..' in local.parts or
                len(local.parts) != 2 or local.parts[0] != 'images' or local.suffix != '.png' or
                local.as_posix() != relative):
            errors.append(f'invalid session image path: {relative}')
            continue
        path = (directory / relative).resolve()
        if not path.is_relative_to(directory):
            errors.append(f'path escapes session: {relative}')
            continue
        if relative in paths or ident in ids:
            errors.append(f'duplicate frame or path: {relative}')
        paths.add(relative)
        ids.add(ident)
        if frame['session_id'] != session['session_id']:
            errors.append(f'wrong session_id: {relative}')
        try:
            read_png(path, expected_size=size)
        except (OSError, ValueError, zlib.error) as error:
            errors.append(f'{relative}: {error}')
        for request in request_list(frame, required=True):
            request_id = request['event_id']
            outcomes[request_id] += 1
            if request_id not in requested or request != requested[request_id]:
                errors.append(f'event {request_id}: missing or changed frame request')
            if not 0 <= captured - request['planned_ms'] <= limit:
                errors.append(f'frame outside request deadline: {relative}')
    for path in directory.rglob('*.tmp'):
        errors.append(f'leftover temporary file: {path.relative_to(directory)}')
    for path in (directory / 'images').glob('*.png'):
        if path.relative_to(directory).as_posix() not in paths:
            errors.append(f'orphaned PNG: {path.name}')
    for ident in set(requested) | set(outcomes):
        if ident not in requested or outcomes[ident] != 1:
            errors.append(f'event {ident}: requested={ident in requested}, terminal outcomes={outcomes[ident]}')
    request_count = len(requested) + sum(last - first + 1 for first, last in ranges)
    finished = [e for e in events if e['type'] == 'session_finished']
    if len(finished) != 1 or events[-1]['type'] != 'session_finished':
        errors.append('missing/duplicate/nonterminal session_finished summary')
    else:
        summary = finished[0]
        expected = dict(saved=len(frames), requested=request_count, expired=expired,
                        pending_overflow=overflow, merged=merged, writer_rejected=writer_rejected, writer_failed=writer_failed)
        for key, value in expected.items():
            if key in ('saved', 'requested') or key in summary:
                if integer(summary[key], f'session_finished.{key}') != value:
                    errors.append(f'inconsistent session_finished.{key}: expected {value}')
        for key in ('writer_high_water', 'cancelled', 'capture_skips', 'input_dropped'):
            if key in summary:
                integer(summary[key], f'session_finished.{key}')
        if 'cancelled' in summary and 'capture_skips' in summary:
            if summary['cancelled'] + summary['capture_skips'] != cancelled_or_capture_skipped:
                errors.append('inconsistent session_finished cancelled/capture_skips totals')
        if 'writer_high_water' in summary and 'writer_capacity' in metadata:
            if summary['writer_high_water'] > integer(metadata['writer_capacity'], 'writer_capacity', 1):
                errors.append('writer_high_water exceeds writer_capacity')
    for error in errors:
        print(f'ERROR: {error}')
    if not frames:
        if not request_count:
            print('WARNING: empty session; no capture requests were generated. '
                  'Check collection_status/control events, F8 and selected-window focus.')
        else:
            print('WARNING: no images were saved. Inspect skipped/cancelled/write_failed events.')
    print(f'{directory}: frames={len(frames)}, requests={request_count}, errors={len(errors)}')
    return not errors


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('session', type=Path)
    args = parser.parse_args()
    try:
        raise SystemExit(0 if validate(args.session) else 1)
    except (OSError, ValueError, KeyError, TypeError, zlib.error) as error:
        print(f'ERROR: {error}')
        raise SystemExit(1)
