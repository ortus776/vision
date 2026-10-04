"""Read-only CSV summary of collector telemetry; these are not game FPS measurements."""
import argparse
import csv
import json
from pathlib import Path
import sys


def summarize(directory):
    session = json.loads((directory / 'session.json').read_text(encoding='utf-8'))
    events = [json.loads(line) for line in (directory / 'events.jsonl').read_text(encoding='utf-8').splitlines()]
    samples = [event for event in events if event.get('type') == 'collection_performance']
    finished = [event for event in events if event.get('type') == 'session_finished']
    if not samples:
        return None
    p = samples[-1]
    end = finished[-1] if finished else {}
    calls, frames, saved = p['capture_calls'], p['captured_frames'], p['saved']
    mean = lambda total, count: round(total / count, 3) if count else ''
    return dict(
        session=directory.name, variant=session['metadata'].get('collect_variant', 'unknown'),
        finished=bool(finished), seconds=round(p['at_ms'] / 1000, 3),
        saved=saved, captured=frames, requests=end.get('requested', ''),
        skips=end.get('capture_skips', ''), writer_rejected=end.get('writer_rejected', ''),
        writer_failed=end.get('writer_failed', ''),
        timeouts=p['timeouts'], next_mean_ms=mean(p['capture_ms'], calls),
        next_max_ms=p['max_capture_ms'], acquire_mean_ms=mean(p['acquire_ms'], calls),
        map_mean_ms=mean(p['map_ms'], frames), map_max_ms=p['max_map_ms'],
        copy_mean_ms=mean(p['copy_ms'], frames), png_mean_ms=mean(p['png_ms'], saved),
        png_max_ms=p['max_png_ms'], write_mean_ms=mean(p['write_ms'], saved),
        writer_high_water=p['writer_high_water'], writer_pending=p['writer_pending'],
        journal_total_ms=p['journal_ms'], prepare_calls=p['prepare_calls'],
        prepare_total_ms=p['prepare_ms'], input_total_ms=p['pump_ms'],
        process_cpu_ms=p['process_cpu_ms'],
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path, help='Session directory or a parent containing sessions')
    args = parser.parse_args()
    if not args.root.is_dir():
        parser.error(f'directory does not exist: {args.root}')
    directories = [args.root] if (args.root / 'session.json').exists() else sorted(
        path.parent for path in args.root.rglob('session.json'))
    rows = []
    for directory in directories:
        try:
            row = summarize(directory)
            if row:
                rows.append(row)
        except (OSError, ValueError, KeyError) as error:
            print(f'{directory}: {error}', file=sys.stderr)
            return 1
    if not rows:
        print('No collection_performance telemetry found.', file=sys.stderr)
        return 1
    writer = csv.DictWriter(sys.stdout, fieldnames=list(rows[0]))
    writer.writeheader()
    writer.writerows(rows)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
