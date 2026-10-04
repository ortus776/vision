"""Analyze game CPU frame intervals and align collector spans using integer QPC timestamps."""
import argparse
from bisect import bisect_left, bisect_right
from collections import Counter, defaultdict
import csv
import json
import math
from pathlib import Path
import sys

STAGES = ('prepare', 'capture', 'acquire', 'map', 'copy', 'png', 'publish', 'writer_queue')


def json_file(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def json_lines(path):
    if not path.exists():
        return []
    return [json.loads(line) for line in path.read_text(encoding='utf-8-sig').splitlines() if line.strip()]


def number(value):
    try:
        value = float(value)
        return value if math.isfinite(value) else None
    except (ValueError, TypeError):
        return None


def percentile(values, quantile):
    values = sorted(values)
    at = (len(values) - 1) * quantile
    lower = math.floor(at)
    return values[lower] + (values[math.ceil(at)] - values[lower]) * (at - lower)


def metrics(frames):
    times = [frame['frame_ms'] for frame in frames]
    if not times:
        return dict(frames=0, average_fps=None, mean_ms=None, p95_ms=None, p99_ms=None, low_1_percent_fps=None,
                    over_16_67_ms=0, over_33_33_ms=0, interval_seconds=0)
    slowest = sorted(times, reverse=True)[:max(1, math.ceil(len(times) * 0.01))]
    return dict(frames=len(times), average_fps=round(1000 * len(times) / sum(times), 3),
                mean_ms=round(sum(times) / len(times), 6), p95_ms=round(percentile(times, 0.95), 6),
                p99_ms=round(percentile(times, 0.99), 6), low_1_percent_fps=round(1000 * len(slowest) / sum(slowest), 3),
                over_16_67_ms=sum(t > 16.67 for t in times), over_33_33_ms=sum(t > 33.33 for t in times),
                interval_seconds=round(sum(times) / 1000, 6))


class Intervals:
    """Union-duration queries in O(log n); half-open intervals prevent boundary false overlaps."""
    def __init__(self, spans):
        self.raw = sorted(spans, key=lambda span: span['start_qpc'])
        self.raw_starts = [span['start_qpc'] for span in self.raw]
        self.max_ends = []
        merged = []
        for span in self.raw:
            begin, end = span['start_qpc'], span['end_qpc']
            self.max_ends.append(max(end, self.max_ends[-1] if self.max_ends else end))
            if merged and begin <= merged[-1][1]:
                merged[-1][1] = max(end, merged[-1][1])
            else:
                merged.append([begin, end])
        self.starts = [item[0] for item in merged]
        self.ends = [item[1] for item in merged]
        self.prefix = [0]
        for begin, end in merged:
            self.prefix.append(self.prefix[-1] + end - begin)

    def before(self, tick):
        index = bisect_right(self.ends, tick)
        total = self.prefix[index]
        if index < len(self.starts):
            total += max(0, tick - self.starts[index])
        return total

    def overlap(self, begin, end):
        return self.before(end) - self.before(begin)

    def matching(self, begin, end):
        left = bisect_right(self.max_ends, begin)
        right = bisect_left(self.raw_starts, end)
        return [span for span in self.raw[left:right] if span['end_qpc'] > begin]


def load_frames(path, pid, frequency):
    """Use consecutive CPUStartQPC values on one swap chain, independent of CSV metric revision."""
    groups = defaultdict(list)
    rejected = Counter()
    with path.open(newline='', encoding='utf-8-sig') as file:
        reader = csv.DictReader(file)
        headers = reader.fieldnames or []
        if not {'CPUStartQPC', 'ProcessID', 'SwapChainAddress'} <= set(headers):
            raise ValueError('PresentMon CSV needs CPUStartQPC, ProcessID, SwapChainAddress (run with --qpc_time)')
        for index, row in enumerate(reader, 2):
            try:
                process = int(row['ProcessID'])
                tick = int(row['CPUStartQPC'])  # Never convert QPC to float: counters may exceed 2**53.
            except (ValueError, TypeError):
                rejected['invalid_qpc_or_pid'] += 1
                continue
            if process != pid:
                rejected['other_pid'] += 1
                continue
            if tick <= 0:
                rejected['invalid_qpc_or_pid'] += 1
                continue
            groups[row['SwapChainAddress']].append((tick, index, row))
    if not groups:
        raise ValueError('No game frame timestamps found; check target PID and PresentMon logs/ETW permissions')
    chain = max(groups, key=lambda key: len(groups[key]))
    rows = sorted(groups[chain])
    frames = []
    for (begin, line, row), (end, _, _) in zip(rows, rows[1:]):
        if end <= begin:
            rejected['duplicate_qpc'] += 1
            continue
        frames.append(dict(start_qpc=begin, end_qpc=end, frame_ms=(end - begin) * 1000 / frequency,
                           raw_csv_line=line, swap_chain=chain, present_mode=row.get('PresentMode', ''),
                           cpu_busy_ms=number(row.get('CPUBusy', row.get('MsCPUBusy'))),
                           cpu_wait_ms=number(row.get('CPUWait', row.get('MsCPUWait'))),
                           gpu_busy_ms=number(row.get('GPUBusy', row.get('MsGPUBusy'))),
                           display_ms=number(row.get('DisplayedTime'))))
    if not frames:
        raise ValueError('Need at least two distinct timestamps on the main swap chain')
    rejected['other_swap_chains'] = sum(len(rows) for key, rows in groups.items() if key != chain)
    return frames, dict(headers=headers, selected_swap_chain=chain,
                        swap_chain_rows={key: len(rows) for key, rows in groups.items()}, rejected_rows=dict(rejected))


def write_csv(path, rows, columns):
    with path.open('w', newline='', encoding='utf-8') as file:
        writer = csv.DictWriter(file, fieldnames=columns, extrasaction='ignore')
        writer.writeheader()
        writer.writerows(rows)


def analyze(directory):
    directory = Path(directory)
    run = json_file(directory / 'run.json')
    frequency = int(run['qpc_frequency'])
    if frequency <= 0:
        raise ValueError('QPC frequency must be positive')
    frames, diagnostics = load_frames(directory / 'game_frames.csv', int(run['target']['process_id']), frequency)
    sessions = sorted((directory / 'collector').glob('session_*'))
    if run['variant'] != 'baseline' and len(sessions) != 1:
        raise ValueError(f'Expected one collector session; found {len(sessions)}')
    trace, events, manifest = [], [], []
    session = sessions[0] if sessions else None
    if session:
        metadata = json_file(session / 'session.json')['metadata']
        if int(metadata['qpc_frequency']) != frequency:
            raise ValueError('Collector and PresentMon QPC frequencies differ')
        trace = json_lines(session / 'collector_trace.jsonl')
        events = json_lines(session / 'events.jsonl')
        manifest = json_lines(session / 'frames.jsonl')
        if not trace:
            raise ValueError('Collector trace missing/empty; run with --trace-frames')
    spans = defaultdict(list)
    for event in trace:
        if event['type'] != 'span':
            continue
        if event['end_qpc'] > event['start_qpc'] > 0:
            spans[event['stage']].append(event)
        if event['stage'] == 'capture':
            for stage in ('acquire', 'map', 'copy'):
                begin, end = event.get(stage + '_start_qpc', 0), event.get(stage + '_end_qpc', 0)
                if end > begin > 0:
                    spans[stage].append(dict(event, stage=stage, start_qpc=begin, end_qpc=end))
    indexes = {stage: Intervals(spans[stage]) for stage in STAGES}
    states = sorted((event for event in trace if event['type'] == 'collection_status'), key=lambda event: event['qpc'])
    if not session:
        states = sorted(json_lines(directory / 'run_trace.jsonl'), key=lambda event: event['qpc'])
    state_ticks = [event['qpc'] for event in states]
    origin = int(run['measurement_start_qpc'])
    collector_starts = [event['qpc'] for event in trace if event['type'] == 'collector_started']
    collector_stops = [event['qpc'] for event in trace if event['type'] == 'collector_stopped']
    start = max(origin, min(collector_starts)) if collector_starts else origin
    start += round(float(run.get('warmup_seconds', 0)) * frequency)
    stop = int(run['measurement_end_qpc'])
    if collector_stops:
        stop = min(stop, max(collector_stops))
    image_paths = {entry['frame_id']: str((session / entry['path']).relative_to(directory)).replace('\\', '/') for entry in manifest}
    request_images = {req['event_id']: image_paths[entry['frame_id']] for entry in manifest for req in entry.get('requests', [])}
    for frame in frames:
        begin, end = frame['start_qpc'], frame['end_qpc']
        state_index = bisect_right(state_ticks, begin) - 1
        state = states[state_index]['state'] if state_index >= 0 else ('baseline' if not session else 'setup')
        # Exclude a whole game frame if it straddles a state boundary.
        crosses_state = bisect_left(state_ticks, end) > bisect_right(state_ticks, begin)
        frame['state'] = 'transition' if crosses_state else state
        frame['included'] = int(begin >= start and end <= stop and
                                (state in ('baseline', 'collecting') and not crosses_state))
        frame['elapsed_seconds'] = round((begin - origin) / frequency, 6)
        for stage, index in indexes.items():
            frame[stage + '_overlap_ms'] = index.overlap(begin, end) * 1000 / frequency
        pngs = indexes['png'].matching(begin, end)
        captures = indexes['capture'].matching(begin, end)
        frame['png_frame_ids'] = ';'.join(str(span['frame_id']) for span in pngs)
        frame['png_paths'] = ';'.join(image_paths.get(span['frame_id'], '') for span in pngs)
        capture_ids = sorted({req['event_id'] for span in captures for req in span.get('requests', [])})
        frame['capture_event_ids'] = ';'.join(str(value) for value in capture_ids)
        frame['captured_image_paths'] = ';'.join(sorted({request_images[value] for value in capture_ids if value in request_images}))
    measured = [frame for frame in frames if frame['included']]
    final = next((event for event in reversed(events) if event['type'] == 'session_finished'), {})
    diagnostics.update(trace_records=len(trace), trace_dropped=final.get('trace_dropped', 0),
                       collector_finished=bool(final) if session else None,
                       captured_timeouts=sum(span.get('outcome') == 'timeout' for span in spans['capture']),
                       saved_images=len(manifest), measurement_start_qpc=start, measurement_end_qpc=stop)
    warnings = []
    if not measured:
        warnings.append('No eligible frames: check foreground focus and warmup duration')
    if diagnostics['trace_dropped']:
        warnings.append('Collector trace overflow: stage attribution is incomplete')
    if session and not final:
        warnings.append('Collector did not complete; this is a partial run')
    if run.get('status') != 'complete':
        warnings.append('Experiment did not complete; compare this run with caution')
    summary = dict(schema_version=1, run_id=run.get('run_id', directory.name), variant=run['variant'],
                   run_status=run.get('status'), measurement=metrics(measured), all_recorded=metrics(frames),
                   states={state: metrics([frame for frame in frames if frame['state'] == state]) for state in sorted({frame['state'] for frame in frames})},
                   stages={stage: dict(overlap=metrics([frame for frame in measured if frame[stage + '_overlap_ms'] > 0]),
                                       no_overlap=metrics([frame for frame in measured if frame[stage + '_overlap_ms'] == 0]),
                                       calls=len(spans[stage]), total_span_ms=sum(span['end_qpc'] - span['start_qpc'] for span in spans[stage]) * 1000 / frequency)
                           for stage in STAGES}, diagnostics=diagnostics, warnings=warnings,
                   frame_definition='Interval between consecutive CPUStartQPC values on the dominant swap chain; last row has no interval.',
                   low_1_percent_definition='1000 / mean duration of slowest ceil(1% * frame_count) CPU frame intervals',
                   attribution='Overlap is correlation, not proof of causation. PNG span includes encoding and file IO; writer_queue is latency, not CPU work.')
    (directory / 'summary.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    columns = ['raw_csv_line', 'start_qpc', 'end_qpc', 'elapsed_seconds', 'frame_ms', 'state', 'included',
               'cpu_busy_ms', 'cpu_wait_ms', 'gpu_busy_ms', 'display_ms', 'present_mode', 'swap_chain']
    columns += [stage + '_overlap_ms' for stage in STAGES] + ['capture_event_ids', 'captured_image_paths', 'png_frame_ids', 'png_paths']
    write_csv(directory / 'aligned_frames.csv', frames, columns)
    buckets = defaultdict(list)
    for frame in measured:
        buckets[math.floor(frame['elapsed_seconds'])].append(frame)
    timeline = []
    for second, batch in sorted(buckets.items()):
        row = dict(second=second, **metrics(batch))
        row.update({stage + '_overlap_ms': sum(frame[stage + '_overlap_ms'] for frame in batch) for stage in STAGES})
        timeline.append(row)
    write_csv(directory / 'timeline.csv', timeline, ['second'] + list(metrics([])) + [stage + '_overlap_ms' for stage in STAGES])
    write_csv(directory / 'summary.csv', [dict(variant=run['variant'], **metrics(measured))], ['variant'] + list(metrics([])))
    return summary


def compare(root):
    rows = []
    for path in sorted(Path(root).rglob('summary.json')):
        summary = json_file(path)
        if 'measurement' not in summary:
            continue
        run = json_file(path.parent / 'run.json')
        row = dict(run=str(path.parent), variant=summary['variant'], status=run.get('status'),
                   roi=f'{run.get("roi_width")}x{run.get("roi_height")}', periodic_ms=run.get('periodic_ms'),
                   collector_sha256=run.get('collector_sha256', ''), **summary['measurement'],
                   trace_dropped=summary['diagnostics']['trace_dropped'], warnings='; '.join(summary['warnings']))
        rows.append(row)
    if not rows:
        raise ValueError('No experiment summaries found')
    path = Path(root) / 'comparison.csv'
    write_csv(path, rows, list(rows[0]))
    for row in rows:
        print(f'{row["variant"]:12} FPS={row["average_fps"]} P99={row["p99_ms"]} 1%low={row["low_1_percent_fps"]} frames={row["frames"]} status={row["status"]}')
    print(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', nargs='?', type=Path)
    parser.add_argument('--compare', type=Path)
    args = parser.parse_args()
    try:
        if args.compare:
            compare(args.compare)
        elif args.run:
            result = analyze(args.run)
            print(json.dumps(result['measurement'], indent=2))
            if not result['measurement']['frames']:
                return 1
        else:
            parser.error('provide a run directory or --compare ROOT')
    except (OSError, ValueError, KeyError) as exc:
        print(f'Analysis failed: {exc}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
