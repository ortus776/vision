"""Desktop smoke: real PresentMon + PNGs + F8 pause/resume + early F9 stop.

Run on an unlocked Windows desktop: python tests/manual/experiment_smoke.py
The D3D11 fixture creates and focuses its own window; raw artifacts are kept in build.
"""
import ctypes
from ctypes import wintypes
import csv
import json
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from analyze_experiment import analyze


def hotkey(process_id, key):
    user = ctypes.WinDLL('user32', use_last_error=True)
    user.FindWindowExW.argtypes = [wintypes.HWND, wintypes.HWND, wintypes.LPCWSTR, wintypes.LPCWSTR]
    user.FindWindowExW.restype = wintypes.HWND
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    window = None
    while True:
        window = user.FindWindowExW(ctypes.c_void_p(-3).value, window, 'PubgVisionRawInput', None)
        if not window:
            raise RuntimeError('Own collector input window was not found')
        owner = wintypes.DWORD()
        user.GetWindowThreadProcessId(window, ctypes.byref(owner))
        if owner.value == process_id:
            if not user.PostMessageW(window, 0x0312, key, 0):  # WM_HOTKEY
                raise ctypes.WinError(ctypes.get_last_error())
            return


def main():
    output = ROOT / 'build' / ('experiment-smoke-' + str(time.time_ns()))
    binary = ROOT / 'build/windows-msvc/Release/pubg_vision_presentmon_target.exe'
    target = subprocess.Popen([str(binary), '40', 'focus'], creationflags=subprocess.CREATE_NO_WINDOW)
    runner = None
    try:
        time.sleep(1)
        base_command = [sys.executable, str(ROOT / 'tools/run_experiment.py'), '--window-title',
                        'Collector PresentMon Test Target', '--game-process-id', str(target.pid),
                        '--warmup-seconds', '0', '--periodic-ms', '150', '--output', str(output)]
        subprocess.run(base_command + ['--variant', 'baseline', '--seconds', '5'], check=True, timeout=30)
        runner = subprocess.Popen(base_command + ['--variant', 'png-store', '--seconds', '20'])
        deadline = time.monotonic() + 10
        run = None
        while time.monotonic() < deadline:
            candidates = list((output / 'png-store').glob('run_*/run.json'))
            if candidates:
                try:
                    value = json.loads(candidates[0].read_text(encoding='utf-8'))
                    if value.get('status') == 'failed':
                        raise RuntimeError(value.get('error', 'Experiment failed'))
                    if 'collector_process_id' in value:
                        run = candidates[0].parent
                        break
                except (OSError, ValueError):
                    pass
            time.sleep(0.1)
        if run is None:
            raise RuntimeError('Collector did not start')
        time.sleep(2)
        if runner.poll() is not None:
            raise RuntimeError('Collector stopped before pause/resume test; inspect its logs')
        hotkey(value['collector_process_id'], 8)
        time.sleep(1)
        hotkey(value['collector_process_id'], 8)
        time.sleep(2)
        hotkey(value['collector_process_id'], 9)
        if runner.wait(timeout=25):
            raise RuntimeError('Experiment launcher failed')
        summary = analyze(run)
        assert summary['measurement']['frames'] > 0
        assert summary['states']['user_paused']['frames'] > 0
        assert summary['diagnostics']['saved_images'] > 0
        assert summary['diagnostics']['trace_dropped'] == 0
        with (run / 'aligned_frames.csv').open(encoding='utf-8') as file:
            rows = list(csv.DictReader(file))
        assert any(row['png_paths'] for row in rows)
        assert any(row['captured_image_paths'] for row in rows)
        session = next((run / 'collector').glob('session_*'))
        subprocess.run([sys.executable, str(ROOT / 'tools/validate_session.py'), str(session)], check=True)
        metadata = json.loads((run / 'run.json').read_text(encoding='utf-8'))
        assert (metadata['measurement_end_qpc'] - metadata['measurement_start_qpc']) / metadata['qpc_frequency'] < 15
        print(f'Experiment smoke passed; artifacts: {output}')
    finally:
        if runner is not None and runner.poll() is None:
            # Give the launcher its normal cleanup path, avoiding an orphan PresentMon session.
            try:
                if run is not None:
                    hotkey(value['collector_process_id'], 9)
                runner.wait(timeout=30)
            except (Exception, subprocess.TimeoutExpired):
                runner.terminate()
                runner.wait(timeout=5)
        if target.poll() is None:
            target.terminate()
        target.wait(timeout=5)


if __name__ == '__main__':
    main()
