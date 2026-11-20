"""Create an exclusive local Linux QA bundle; this is not a signed release or installer."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ('maskpilot', 'maskpilot_cli', 'maskpilot_native_host', 'media_guard/ffmpeg', 'media_guard/ffprobe')


def execute(arguments, **kwargs):
    result = subprocess.run(list(map(str, arguments)), capture_output=True, text=True,
                            env=dict(os.environ, LC_ALL='C'), **kwargs)
    if result.returncode:
        raise RuntimeError(result.stdout[-6000:] + result.stderr[-6000:])
    return result.stdout


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def native_source_digest(root=ROOT):
    paths = [root / name for name in ('CMakeLists.txt', 'CMakePresets.json', 'vcpkg.json',
                                      'vcpkg-configuration.json')]
    for name in ('src', 'apps', 'resources', 'database', 'cmake'):
        paths.extend(path for path in (root / name).rglob('*') if path.is_file())
    value = hashlib.sha256()
    for path in sorted(paths, key=lambda item: item.relative_to(root).as_posix()):
        if path.is_symlink():
            raise ValueError('Native source fingerprint refuses symlinks')
        value.update((path.relative_to(root).as_posix() + '\0' + digest(path) + '\n').encode())
    return value.hexdigest()


def inventory(root):
    result = {}
    for path in sorted(root.rglob('*')):
        name = path.relative_to(root).as_posix()
        if path.is_symlink():
            target = os.readlink(path)
            if Path(target).is_absolute() or not path.resolve(strict=True).is_relative_to(root):
                raise ValueError('Package link escapes its private root: ' + name)
            result[name] = dict(type='symlink', target=target)
        elif path.is_file():
            result[name] = dict(type='file', size=path.stat().st_size, sha256=digest(path),
                                mode=path.stat().st_mode & 0o777)
        elif not path.is_dir():
            raise ValueError('Unsupported package entry: ' + name)
    return result


def package(build, output, declared_commit=None):
    if sys.platform != 'linux':
        raise ValueError('This helper packages native Linux builds only')
    build = build.resolve(strict=True)
    output = output.absolute()
    for candidate in (output, Path(str(output) + '.tar.gz'), Path(str(output) + '.tar.gz.sha256')):
        if candidate.exists() or candidate.is_symlink():
            raise FileExistsError('Refusing to replace an existing package stage, archive or checksum')
    output = output.resolve()
    archive = Path(str(output) + '.tar.gz')
    cache = dict(re.findall(r'^([^/#\n][^:\n]*):[^=\n]+=([^\r\n]*)$',
                            (build / 'CMakeCache.txt').read_text(), re.MULTILINE))
    if (cache.get('CMAKE_PROJECT_NAME') != 'MaskPilot' or
            Path(cache.get('CMAKE_HOME_DIRECTORY', '')).resolve() != ROOT or
            cache.get('CMAKE_BUILD_TYPE') != 'Release' or cache.get('MASKPILOT_ENABLE_ASAN') != 'OFF'):
        raise ValueError('Select this checkout\'s non-instrumented Release build')
    for parent in (build, *[ROOT / part for part in ('src', 'apps', 'resources', 'database', 'cmake')]):
        if output == parent or output.is_relative_to(parent) or parent.is_relative_to(output):
            raise ValueError('Package stage overlaps source or build inputs')
    if declared_commit and not re.fullmatch('[a-f0-9]{40}', declared_commit):
        raise ValueError('Source commit must be a complete lowercase Git object ID')
    targets = ['maskpilot', 'maskpilot_cli', 'maskpilot_native_host',
               'maskpilot_media_ffmpeg', 'maskpilot_media_ffprobe']
    execute(['cmake', '--build', build, '--target', *targets, '--parallel', '1'], timeout=1800)
    fingerprint = native_source_digest()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.mkdir(mode=0o755)  # Exclusive ownership; failed stages remain for inspection.
    execute(['cmake', '--install', build, '--prefix', output, '--component', 'LinuxRuntime'], timeout=90)
    required = (*RUNTIME, 'LICENSE', 'licenses/libtorrent/LICENSE', 'licenses/libtorrent/COPYING',
                'browser_extension/chromium/manifest.json', 'browser_extension/firefox/manifest.json',
                'linux-build-info.json')
    for name in required:
        if not (output / name).is_file():
            raise ValueError('Required package file missing (check notices and extension build): ' + name)
    info = json.loads((output / 'linux-build-info.json').read_text())
    if info['system'] != 'Linux' or info['architecture'] != platform.machine():
        raise ValueError('Package architecture differs from the qualification host')
    for name in RUNTIME:
        binary = output / name
        with binary.open('rb') as stream:
            if stream.read(4) != b'\x7fELF' or not os.access(binary, os.X_OK):
                raise ValueError('Expected a runnable native ELF: ' + name)
        dependencies = execute(['ldd', binary], timeout=15)
        if 'not found' in dependencies:
            raise ValueError('Missing system runtime for ' + name + ': ' + dependencies)
        if 'libtorrent-rasterbar' in dependencies:
            line = next(line for line in dependencies.splitlines() if 'libtorrent-rasterbar' in line)
            if str(output / 'lib') + '/' not in line:
                raise ValueError('BT runtime fell back outside the installed bundle')
    if fingerprint != native_source_digest():
        raise ValueError('Native inputs changed during packaging')
    commit, verified_commit = declared_commit, False
    if (ROOT / '.git').exists():
        actual = execute(['git', '-C', ROOT, 'rev-parse', 'HEAD']).strip()
        dirty = execute(['git', '-C', ROOT, 'status', '--porcelain']).strip()
        commit = declared_commit or actual
        verified_commit = not dirty and commit == actual
    manifest = dict(schema=1, purpose='local-linux-qa-not-a-signed-release', sourceCommit=commit,
                    sourceCommitVerified=verified_commit, nativeSourceSha256=fingerprint,
                    build=info, qualificationLibc=platform.libc_ver(), files=inventory(output))
    with (output / 'manifest.json').open('x', encoding='utf-8') as stream:
        json.dump(manifest, stream, ensure_ascii=False, indent=2)
        stream.write('\n')
    with tarfile.open(archive, 'x:gz') as bundle:
        bundle.add(output, arcname=output.name, recursive=True)
    checksum = digest(archive)
    with Path(str(archive) + '.sha256').open('x', encoding='ascii') as stream:
        stream.write(checksum + '  ' + archive.name + '\n')
    return dict(directory=str(output), archive=str(archive), sha256=checksum,
                nativeSourceSha256=fingerprint, sourceCommit=commit, sourceCommitVerified=verified_commit)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-directory', type=Path, required=True)
    parser.add_argument('--output-directory', type=Path, required=True)
    parser.add_argument('--source-commit', help='Declared commit for a Git-less acceptance snapshot')
    options = parser.parse_args()
    try:
        print(json.dumps(package(options.build_directory, options.output_directory, options.source_commit)))
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        parser.exit(1, str(error) + '\n')
