"""Download a pinned official portable PresentMon console binary and verify its SHA-256."""
import argparse
import hashlib
from pathlib import Path
import urllib.request

VERSION = '2.6.0'
SHA256 = 'b2a706bc6ad475749e3b7e3409263aa1e6906d45bdcf993f6dbc0f660188f1af'
URL = f'https://github.com/GameTechDev/PresentMon/releases/download/v{VERSION}/PresentMon-{VERSION}-x64.exe'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, default=Path(__file__).resolve().parents[1] / 'build/tools/presentmon')
    args = parser.parse_args()
    path = args.directory / f'PresentMon-{VERSION}-x64.exe'
    if path.exists() and hashlib.sha256(path.read_bytes()).hexdigest() == SHA256:
        print(path)
        return
    request = urllib.request.Request(URL, headers={'User-Agent': 'collector-performance'})
    with urllib.request.urlopen(request, timeout=60) as response:
        data = response.read(64 * 1024 * 1024 + 1)
    if len(data) > 64 * 1024 * 1024 or hashlib.sha256(data).hexdigest() != SHA256:
        raise RuntimeError('PresentMon download failed SHA-256 verification')
    args.directory.mkdir(parents=True, exist_ok=True)
    temp = path.with_suffix('.download')
    temp.write_bytes(data)
    temp.replace(path)
    print(path)


if __name__ == '__main__':
    main()
