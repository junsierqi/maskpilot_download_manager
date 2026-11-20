"""Actual relocated Linux package, no-overwrite and native runtime acceptance."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def execute(arguments, env, expected=0, timeout=120):
    result = subprocess.run(list(map(str, arguments)), env=env, capture_output=True, text=True, timeout=timeout)
    assert result.returncode == expected, (result.returncode, result.stdout[-6000:], result.stderr[-6000:])
    return result.stdout


def exercise(build):
    env = dict(os.environ, QT_QPA_PLATFORM='offscreen', PATH='/usr/bin:/bin')
    for key in ('LD_LIBRARY_PATH', 'QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH', 'QT_ROOT_DIR', 'QML2_IMPORT_PATH'):
        env.pop(key, None)
    with tempfile.TemporaryDirectory(prefix='maskpilot-linux-package-') as temporary:
        root = Path(temporary)
        stage = root / 'relocated'
        command = [sys.executable, ROOT / 'scripts/package_linux.py', '--build-directory', build,
                   '--output-directory', stage]
        result = json.loads(execute(command, env, timeout=180))
        archive = Path(result['archive'])
        original = archive.read_bytes()
        assert hashlib.sha256(original).hexdigest() == result['sha256']
        manifest = json.loads((stage / 'manifest.json').read_text())
        for name, record in manifest['files'].items():
            path = stage / name
            if record['type'] == 'symlink':
                assert path.is_symlink() and os.readlink(path) == record['target']
                assert path.resolve(strict=True).is_relative_to(stage)
            else:
                assert path.stat().st_size == record['size']
                assert hashlib.sha256(path.read_bytes()).hexdigest() == record['sha256']
        with tarfile.open(archive) as bundle:
            names = bundle.getnames()
            assert names and all(Path(name).parts[0] == stage.name and '..' not in Path(name).parts for name in names)
            assert bundle.extractfile(stage.name + '/manifest.json').read() == (stage / 'manifest.json').read_bytes()
        rejected = subprocess.run(list(map(str, command)), env=env, capture_output=True, text=True, timeout=15)
        assert rejected.returncode != 0 and 'Refusing to replace' in rejected.stderr
        assert archive.read_bytes() == original
        blocked = root / 'checksum-only'
        checksum = Path(str(blocked) + '.tar.gz.sha256')
        checksum.write_text('owned-preexisting-checksum')
        refused = subprocess.run([sys.executable, str(ROOT / 'scripts/package_linux.py'),
            '--build-directory', str(build), '--output-directory', str(blocked)], env=env,
            capture_output=True, text=True, timeout=15)
        assert refused.returncode != 0 and not blocked.exists() and checksum.read_text() == 'owned-preexisting-checksum'
        print('PASS actual package bytes, complete notices, internal links and exclusive stage/archive/checksum', flush=True)
        dependencies = execute(['ldd', stage / 'maskpilot_cli'], env)
        assert str(stage / 'lib') in dependencies and str(build) not in dependencies
        execute([stage / 'maskpilot', '--smoke-test', '--data-dir', root / 'gui-data'], env, timeout=15)
        execute([sys.executable, ROOT / 'tests/integration/native_host_test.py',
                 stage / 'maskpilot_native_host', stage / 'maskpilot'], env, timeout=45)
        print('PASS installed Qt event loop and real native bridge transfer without developer runtime paths', flush=True)
        env.update(MASKPILOT_TEST_CLI=str(stage / 'maskpilot_cli'), MASKPILOT_TEST_CLEAN_CLI_ENV='1')
        execute([build / 'tests/maskpilot_tests',
                 '--gtest_filter=TorrentEngine.ActualCliCapturesAndDownloadsLocalTorrentWithoutPublicDiscovery'],
                env, timeout=60)
        print('PASS installed CLI captures local torrent, transfers verified pieces and persists completion', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-directory', type=Path, required=True)
    exercise(parser.parse_args().build_directory.resolve(strict=True))
