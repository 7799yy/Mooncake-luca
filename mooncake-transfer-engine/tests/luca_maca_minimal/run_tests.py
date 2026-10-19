"""Build production transport sources against CPU-only dependency doubles.

No vendor SDK, GPU, network, model, or installed packages are required.
This checks lifecycle and completion behavior, not native ABI or performance.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', help='Host C++17 compiler (g++/clang++/c++)')
    args = parser.parse_args()
    compiler = args.cxx or shutil.which('g++') or shutil.which('c++')
    if not compiler:
        raise SystemExit('A host C++17 compiler is required')
    here = Path(__file__).resolve().parent
    engine = here.parents[1]
    sources = [here / 'test_mother.cpp', here / 'mock_luca.cpp',
               engine / 'src/transport/luca_transport/luca_transport.cpp',
               engine / 'src/multi_transport.cpp', engine / 'src/transport/transport.cpp',
               engine / 'src/common/base/status.cpp']
    temporary_root = Path(tempfile.gettempdir()).resolve()
    with tempfile.TemporaryDirectory(prefix='mooncake-luca-mock-', dir=temporary_root) as temporary:
        work = Path(temporary).resolve()
        assert work.parent == temporary_root and work.name.startswith('mooncake-luca-mock-')
        program = work / ('luca_mock.exe' if os.name == 'nt' else 'luca_mock')
        command = [compiler, '-std=c++17', '-pthread', '-O1', '-g', '-DUSE_LUCA',
                   '-I' + str(here),
                   '-I' + str(here / 'include'), '-I' + str(engine / 'include'),
                   *map(str, sources), '-o', str(program)]
        if os.name == 'nt':
            # This suite is single-threaded. The available MinGW win32 runtime
            # crashes on a minimal thread_local vector destructor too. Keep a
            # process-lifetime slice cache in Windows tests only; Linux retains
            # the production TLS implementation and standard mutex unchanged.
            command.insert(1, '-Dthread_local=')
        built = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, encoding='utf-8', errors='replace', timeout=120)
        if built.returncode:
            lines = [line for line in built.stderr.splitlines()
                     if 'error:' in line or 'undefined reference' in line]
            print('\n'.join(lines[:20]) or built.stderr[-4000:])
            raise SystemExit(built.returncode)
        environment = dict(os.environ)
        compiler_path = Path(shutil.which(compiler) or compiler).resolve().parent
        environment['PATH'] = str(compiler_path) + os.pathsep + environment.get('PATH', '')
        for mode in ('default', 'batchflag', 'auto'):
            for sync in ('host', 'wait'):
                subprocess.run([str(program), mode, sync], env=environment, check=True, timeout=30)
        assert work.parent == temporary_root and work.name.startswith('mooncake-luca-mock-')


if __name__ == '__main__':
    main()
