"""Strict WASI driver. Use make test-wasi-strict for whole-tree resource limits."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(command, **kwargs):
    print('+ ' + ' '.join(map(str, command)), flush=True)
    return subprocess.run(list(map(str, command)), check=True, timeout=120, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    args = parser.parse_args()
    missing = [tool for tool in ['zig', 'wasmtime'] if shutil.which(tool) is None]
    if missing:
        print('SKIP strict WASI: missing ' + ', '.join(missing), file=sys.stderr)
        return 77
    build = args.build.resolve()
    build.mkdir(parents=True, exist_ok=True)
    run(['zig', 'version'])
    run(['wasmtime', '--version'])
    settings = ['CC=zig cc -target wasm32-wasi',
                'STRICT=-std=c89 -pedantic-errors -Wall -Wextra -Werror',
                'CFLAGS=-O1 -g -UNDEBUG']
    run(['make', '-B', '-j1', '--no-print-directory', '-C', ROOT / 'examples/curun',
         'BUILD=' + str(build / 'curun'), *settings, 'curun'])
    tests = ['test_append', 'test_process', 'test_process_io']
    run(['make', '-B', '-j1', '--no-print-directory', '-C', ROOT,
         'BUILD=' + str(build / 'unit'), *settings, *[build / 'unit' / t for t in tests]])
    with tempfile.TemporaryDirectory(prefix='tmp-', dir=build) as directory:
        run(['wasmtime', 'run', '--dir', directory + '::/tmp', '--env', 'TMPDIR=/tmp',
             build / 'unit/test_append'])
    for test in tests[1:]:
        run(['wasmtime', 'run', build / 'unit' / test])
    binary = build / 'curun/curun'
    result = run(['wasmtime', 'run', binary, '--help'], capture_output=True)
    if not result.stdout.startswith(b'usage: curun ') or result.stderr:
        raise RuntimeError('WASI curun help did not match the expected interface')
    result = subprocess.run(['wasmtime', 'run', str(binary), '--', '/bin/echo'],
                            capture_output=True, timeout=20)
    if result.returncode != 125 or result.stdout or b'unavailable on this platform' not in result.stderr:
        raise RuntimeError('WASI curun did not report unsupported execution')
    print('PASS strict WASI: full library links, append contracts, ENOSYS process APIs, curun interface')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print('strict WASI: ' + str(error), file=sys.stderr)
        sys.exit(1)
