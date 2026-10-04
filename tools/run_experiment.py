"""Record PresentMon frames and an optional collector session on the same QPC clock."""
import argparse
import ctypes
from ctypes import wintypes
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import threading
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
VARIANTS = ('baseline', 'legacy', 'balanced', 'held', 'png-none', 'png-store', 'capture-only', 'input-only')


def qpc():
    value = ctypes.c_longlong()
    if not ctypes.windll.kernel32.QueryPerformanceCounter(ctypes.byref(value)):
        raise ctypes.WinError()
    return value.value


def frequency():
    value = ctypes.c_longlong()
    if not ctypes.windll.kernel32.QueryPerformanceFrequency(ctypes.byref(value)):
        raise ctypes.WinError()
    return value.value


def target_window(title, expected_pid=None):
    user = ctypes.WinDLL('user32', use_last_error=True)
    user.IsWindowVisible.argtypes = [wintypes.HWND]
    user.IsIconic.argtypes = [wintypes.HWND]
    user.GetWindow.argtypes = [wintypes.HWND, wintypes.UINT]
    user.GetWindow.restype = wintypes.HWND
    user.GetWindowTextLengthW.argtypes = [wintypes.HWND]
    user.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
    found = []

    @callback_type
    def visit(hwnd, _):
        if user.IsWindowVisible(hwnd) and not user.IsIconic(hwnd) and not user.GetWindow(hwnd, 4):
            text = ctypes.create_unicode_buffer(user.GetWindowTextLengthW(hwnd) + 1)
            user.GetWindowTextW(hwnd, text, len(text))
            if title.casefold() in text.value.casefold():
                pid = wintypes.DWORD()
                user.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
                found.append({'window_handle': int(hwnd), 'window_title': text.value, 'process_id': pid.value})
        return True

    if not user.EnumWindows(visit, 0):
        raise ctypes.WinError(ctypes.get_last_error())
    if len(found) != 1:
        raise RuntimeError(f'Expected one visible window containing {title!r}; found {len(found)}: {found}')
    if expected_pid is not None and expected_pid != found[0]['process_id']:
        raise RuntimeError('GameProcessId differs from the process owning the collector target window')
    return found[0]


def write_json(path, value):
    temp = path.with_suffix('.tmp')
    temp.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    for attempt in range(30):
        try:
            temp.replace(path)
            return
        except PermissionError:
            # Windows readers/antivirus may briefly hold the destination without delete sharing.
            if attempt == 29:
                raise
            time.sleep(0.01)


def run(args):
    if sys.platform != 'win32':
        raise RuntimeError('Live experiments require Windows')
    from install_presentmon import VERSION, SHA256
    presentmon = ROOT / f'build/tools/presentmon/PresentMon-{VERSION}-x64.exe'
    if not presentmon.exists() or hashlib.sha256(presentmon.read_bytes()).hexdigest() != SHA256:
        subprocess.run([sys.executable, str(ROOT / 'tools/install_presentmon.py')], check=True)
    if args.variant != 'baseline' and not args.exe.is_file():
        raise RuntimeError('Build Release first: cmake --build --preset windows-msvc-release')
    target = target_window(args.window_title, args.game_process_id)
    run_id = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ_') + uuid.uuid4().hex[:12]
    output = args.output.resolve() / args.variant / ('run_' + run_id)
    output.mkdir(parents=True, exist_ok=False)
    session_name = 'CollectorExperiment_' + uuid.uuid4().hex
    pm_args = [str(presentmon), '--process_id', str(target['process_id']), '--output_file', str(output / 'game_frames.csv'),
               '--qpc_time', '--v2_metrics', '--no_console_stats', '--no_track_input',
               '--session_name', session_name, '--timed', str(args.seconds + 60), '--terminate_after_timed']
    app_args = [str(args.exe.resolve()), '--collect', '--start-active', '--trace-frames',
                '--window-title', args.window_title, '--collect-variant', args.variant,
                '--collect-seconds', str(args.seconds), '--roi-width', str(args.roi_width),
                '--roi-height', str(args.roi_height), '--periodic-ms', str(args.periodic_ms),
                '--output', str(output / 'collector')]
    metadata = dict(schema_version=1, run_id=run_id, variant=args.variant, target=target,
                    started_utc=datetime.now(timezone.utc).isoformat(), qpc_frequency=frequency(),
                    warmup_seconds=args.warmup_seconds, seconds=args.seconds, roi_width=args.roi_width,
                    roi_height=args.roi_height, periodic_ms=args.periodic_ms, status='starting',
                    presentmon_version=VERSION, presentmon_sha256=SHA256, presentmon_command=pm_args,
                    collector_command=app_args if args.variant != 'baseline' else None)
    if args.variant != 'baseline':
        metadata['collector_sha256'] = hashlib.sha256(args.exe.read_bytes()).hexdigest()
    write_json(output / 'run.json', metadata)
    print(f'Results: {output}', flush=True)
    print(f'Target PID: {target["process_id"]}; variant: {args.variant}. Switch to the target window.', flush=True)
    monitor = collector = None
    error = None
    foreground_window = ctypes.windll.user32.GetForegroundWindow
    foreground_window.restype = wintypes.HWND
    previous_focus = None
    with (output / 'presentmon.stdout.log').open('wb') as pm_out, (output / 'presentmon.stderr.log').open('wb') as pm_err, \
            (output / 'run_trace.jsonl').open('w', encoding='utf-8') as run_trace:
        def sample_focus():
            nonlocal previous_focus
            focused = foreground_window() == target['window_handle']
            if previous_focus is None or focused != previous_focus:
                run_trace.write(json.dumps(dict(type='collection_status', qpc=qpc(),
                                               state='baseline' if focused else 'focus_paused')) + '\n')
                run_trace.flush()
                previous_focus = focused
        try:
            monitor = subprocess.Popen(pm_args, stdout=pm_out, stderr=pm_err, creationflags=subprocess.CREATE_NO_WINDOW)
            time.sleep(1)
            if monitor.poll() is not None:
                raise RuntimeError(f'PresentMon exited at startup ({monitor.returncode}); see presentmon.stderr.log/stdout.log')
            metadata['measurement_start_qpc'] = qpc()
            metadata['focus_poll_ms'] = 100
            sample_focus()
            metadata['status'] = 'recording'
            write_json(output / 'run.json', metadata)
            if args.variant == 'baseline':
                deadline = time.monotonic() + args.seconds
                while time.monotonic() < deadline:
                    sample_focus()
                    if monitor.poll() is not None:
                        raise RuntimeError('PresentMon stopped during baseline; see its logs')
                    time.sleep(min(0.1, max(0, deadline - time.monotonic())))
            else:
                with (output / 'collector.log').open('w', encoding='utf-8') as app_log:
                    collector = subprocess.Popen(app_args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                                 text=True, encoding='utf-8', errors='backslashreplace', bufsize=1)

                    def log_collector():
                        for line in collector.stdout:
                            app_log.write(line)
                            app_log.flush()
                            print(line, end='', flush=True)

                    reader = threading.Thread(target=log_collector, daemon=True)
                    deadline = time.monotonic() + args.seconds + 30
                    try:
                        metadata['collector_process_id'] = collector.pid
                        write_json(output / 'run.json', metadata)
                        reader.start()
                        while collector.poll() is None:
                            sample_focus()
                            if monitor.poll() is not None:
                                raise RuntimeError('PresentMon stopped while collector was running; see its logs')
                            if time.monotonic() >= deadline:
                                raise RuntimeError('Collector exceeded its duration plus 30 seconds for draining')
                            time.sleep(0.1)
                    finally:
                        if collector.poll() is None:
                            collector.terminate()
                            collector.wait(timeout=10)
                        if reader.ident is not None:
                            reader.join(timeout=10)
                    metadata['collector_exit_code'] = collector.returncode
                    if collector.returncode:
                        raise RuntimeError(f'Collector exited with code {collector.returncode}; see collector.log')
            metadata['status'] = 'complete'
        except (Exception, KeyboardInterrupt) as exc:
            error = str(exc) or 'Interrupted'
            metadata.update(status='failed', error=error)
        finally:
            metadata['measurement_end_qpc'] = qpc()
            if monitor is not None:
                # Also attempt cleanup after a crash: an ETW session can outlive its process.
                # Never stop sessions belonging to another experiment or another user tool.
                try:
                    stop = subprocess.run([str(presentmon), '--session_name', session_name, '--terminate_existing_session'],
                                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10,
                                          creationflags=subprocess.CREATE_NO_WINDOW)
                    (output / 'presentmon.stop.log').write_bytes(stop.stdout)
                    metadata['presentmon_stop_exit_code'] = stop.returncode
                    monitor.wait(timeout=15)
                except (OSError, subprocess.TimeoutExpired) as exc:
                    if monitor.poll() is None:
                        error = error or f'PresentMon did not shut down cleanly: {exc}'
                        monitor.terminate()
                        monitor.wait(timeout=10)
                metadata['presentmon_exit_code'] = monitor.returncode
                if monitor.returncode:
                    error = error or f'PresentMon exited with code {monitor.returncode}; see its logs'
            if error:
                metadata.update(status='failed', error=error)
            metadata['finished_utc'] = datetime.now(timezone.utc).isoformat()
            write_json(output / 'run.json', metadata)
    try:
        from analyze_experiment import analyze
        summary = analyze(output)
        active = summary['measurement']
        print(f'Analyzed {active["frames"]} frames: {active["average_fps"]} FPS; P99 {active["p99_ms"]} ms', flush=True)
        if not active['frames']:
            error = error or 'No frames in the measurement window (check focus, duration, target PID and PresentMon logs)'
    except Exception as exc:
        error = error or f'Frame analysis failed: {exc}'
    if error:
        metadata.update(status='failed', error=error)
        write_json(output / 'run.json', metadata)
        raise RuntimeError(f'{error}\nPartial results preserved: {output}')
    print('Compare runs: python tools/analyze_experiment.py --compare data/performance', flush=True)


def main():
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, 'reconfigure'):
            stream.reconfigure(errors='backslashreplace')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--variant', choices=VARIANTS, default='png-store')
    parser.add_argument('--window-title', default='PUBG')
    parser.add_argument('--game-process-id', type=int)
    parser.add_argument('--seconds', type=int, default=90)
    parser.add_argument('--warmup-seconds', type=float, default=5)
    parser.add_argument('--roi-width', type=int, default=640)
    parser.add_argument('--roi-height', type=int, default=640)
    parser.add_argument('--periodic-ms', type=int, default=5000)
    parser.add_argument('--exe', type=Path, default=ROOT / 'build/windows-msvc/Release/pubg_vision_app.exe')
    parser.add_argument('--output', type=Path, default=ROOT / 'data/performance')
    args = parser.parse_args()
    if not 1 <= args.seconds <= 3600 or not 0 <= args.warmup_seconds < args.seconds:
        parser.error('seconds must be 1..3600; warmup-seconds must be >=0 and less than seconds')
    if not all(1 <= size <= 8192 for size in (args.roi_width, args.roi_height)) or not 1 <= args.periodic_ms <= 3600000:
        parser.error('invalid ROI or periodic interval')
    try:
        run(args)
    except Exception as exc:
        print(str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
