"""Private local qualification baseline; consistency hashes are not publisher authentication."""
import argparse
import ctypes
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import subprocess
import tempfile
import xml.etree.ElementTree as ET

MAX_MANIFEST = 4 * 1024 * 1024
MAX_FILES = 20000
MAX_FILE = 2 * 1024 * 1024 * 1024
REQUIRED_RUNTIME = ('maskpilot.exe', 'maskpilot_cli.exe', 'maskpilot_native_host.exe',
                    'media_guard/ffmpeg.exe', 'media_guard/ffprobe.exe')
SOURCE_ROOTS = {'src', 'apps', 'resources', 'database', 'cmake'}
SOURCE_FILES = {'CMakeLists.txt', 'CMakePresets.json', 'vcpkg.json', 'vcpkg-configuration.json',
                'scripts/audit_release_paths.py', 'scripts/probe_portable_compiler.ps1',
                'scripts/release_manifest.py', 'scripts/build.ps1', 'scripts/package.ps1'}
RUNTIME_TREES = ('plugins', 'translations', 'platforms', 'styles', 'imageformats', 'iconengines',
                 'networkinformation', 'tls', 'generic')


def _fail(message):
    raise ValueError('Qualification manifest: ' + message)


def _safe(path, *, existing=True):
    path = Path(path)
    if (not path.is_absolute() or '..' in path.parts or str(path).startswith(('\\\\', '//')) or
            any(ord(character) < 32 or ord(character) == 127 for character in str(path))):
        _fail('expected an absolute local path')
    for part in (*reversed(path.parents), path):
        try:
            status = part.lstat()
        except FileNotFoundError:
            if existing:
                raise
            continue
        if part.is_symlink() or getattr(status, 'st_file_attributes', 0) & 0x400:
            _fail('symbolic links and reparse paths are not qualified')
    if existing and not path.exists():
        _fail('missing local path')
    return path


def _base(path):
    path = _safe(Path(path).absolute())
    if not path.is_dir():
        _fail('root is not a directory')
    return path.resolve(strict=True)


def _relative(value):
    if (not isinstance(value, str) or not value or len(value) > 1024 or '\\' in value or ':' in value or
            any(ord(character) < 32 or ord(character) == 127 for character in value)):
        _fail('invalid relative inventory path')
    path = PurePosixPath(value)
    if path.is_absolute() or any(part in ('', '.', '..') for part in value.split('/')):
        _fail('invalid relative inventory path')
    return value


def _record(path):
    path = _safe(path)
    if not path.is_file():
        _fail('inventory entry is not a regular file')
    before = path.stat()
    if before.st_size > MAX_FILE:
        _fail('inventory file exceeds qualification bound')
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        while block := stream.read(1024 * 1024):
            digest.update(block)
    after = path.stat()
    _safe(path)
    if (before.st_size, before.st_mtime_ns, before.st_ino) != (after.st_size, after.st_mtime_ns, after.st_ino):
        _fail('file changed while hashing')
    return {'sha256': digest.hexdigest(), 'size': before.st_size}


def _git(repo, *arguments):
    result = subprocess.run(['git', '-C', str(repo), *arguments], capture_output=True, timeout=20,
                            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    if result.returncode or len(result.stdout) > MAX_MANIFEST:
        _fail('cannot inspect bounded local Git provenance')
    return result.stdout


def _sources(repo):
    result = {}
    for raw in _git(repo, 'ls-files', '-z', '--others', '--exclude-standard').split(b'\0'):
        if raw:
            name = _relative(raw.decode('utf-8', errors='strict'))
            if name in SOURCE_FILES or PurePosixPath(name).parts[0] in SOURCE_ROOTS:
                _fail('untracked native input must be staged before qualification')
    for raw in _git(repo, 'ls-files', '-z', '--cached').split(b'\0'):
        if not raw:
            continue
        name = _relative(raw.decode('utf-8', errors='strict'))
        if name in SOURCE_FILES or PurePosixPath(name).parts[0] in SOURCE_ROOTS:
            result[name] = _record(repo / name)['sha256']
    if not SOURCE_FILES.issubset(result) or len(result) > MAX_FILES:
        _fail('missing or excessive tracked native inputs')
    return dict(sorted(result.items()))


def _runtime(build):
    paths = [path for path in build.iterdir() if path.suffix.lower() in ('.exe', '.dll')]
    configuration = build / 'qt.conf'
    _safe(configuration, existing=False)
    if configuration.exists():
        paths.append(configuration)
    guard = build / 'media_guard'
    _safe(guard, existing=False)
    if guard.exists():
        _safe(guard)
        paths.extend(path for path in guard.iterdir() if path.suffix.lower() in ('.exe', '.dll'))
    for name in RUNTIME_TREES:
        directory = build / name
        _safe(directory, existing=False)
        if not directory.exists():
            continue
        _safe(directory)
        for current, directories, files in os.walk(directory, followlinks=False):
            for child in directories:
                _safe(Path(current) / child)
            paths.extend(Path(current) / child for child in files)
    if len(paths) > MAX_FILES:
        _fail('excessive runtime inventory')
    result = {}
    for path in paths:
        name = _relative(path.relative_to(build).as_posix())
        if 'asan' in path.name.lower():
            _fail('instrumented runtime is not a Release qualification')
        result[name] = _record(path)
    if not set(REQUIRED_RUNTIME).issubset(result):
        _fail('three applications and two media guards are required')
    return dict(sorted(result.items()))


def _bounded_text(path, limit=MAX_MANIFEST):
    path = _safe(path)
    if not path.is_file() or path.stat().st_size > limit:
        _fail('oversized or invalid metadata file')
    return path.read_text(encoding='utf-8-sig')


def _json(path):
    return json.loads(_bounded_text(path))


def _version(repo):
    match = re.search(r'project\s*\(\s*MaskPilot\s+VERSION\s+(\d+\.\d+\.\d+)\b',
                      _bounded_text(repo / 'CMakeLists.txt'))
    if not match or not re.fullmatch(r'(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)', match[1]):
        _fail('current project must declare a strict semantic version')
    version = match[1]
    for name in ('browser_extension/package.json', 'browser_extension/manifests/manifest.json',
                 'browser_extension/manifests/firefox.json'):
        if _json(repo / name).get('version') != version:
            _fail('project and extension versions disagree')
    lock = _json(repo / 'browser_extension/package-lock.json')
    if lock.get('version') != version or lock.get('packages', {}).get('', {}).get('version') != version:
        _fail('extension lock version disagrees')
    if _json(repo / 'vcpkg.json').get('version-string') != version:
        _fail('dependency manifest version disagrees')
    assembly = ET.fromstring(_bounded_text(repo / 'resources/windows/app.manifest'))
    identity = assembly.find('{urn:schemas-microsoft-com:asm.v1}assemblyIdentity')
    if identity is None or identity.get('version') != version + '.0':
        _fail('Windows assembly version disagrees')
    return version


def _cache(repo, build, version):
    result = {}
    for line in _bounded_text(build / 'CMakeCache.txt').splitlines():
        if line.startswith(('#', '//')):
            continue
        match = re.fullmatch(r'([^:=]+):[^=]*=(.*)', line)
        if match:
            if match[1] in result:
                _fail('duplicate CMake cache key')
            result[match[1]] = match[2]
    if result.get('CMAKE_BUILD_TYPE') != 'Release' or result.get('MASKPILOT_ENABLE_ASAN') != 'OFF':
        _fail('Release with AddressSanitizer OFF is required')
    if result.get('CMAKE_PROJECT_VERSION') != version:
        _fail('configured project version is stale')
    home = result.get('CMAKE_HOME_DIRECTORY', '')
    if not home or _base(Path(home)) != repo:
        _fail('build does not belong to this source repository')
    for name, value in result.items():
        if ('FLAGS' in name or name == 'CMAKE_CXX_COMPILER') and re.search(r'fsanitize[=:]address|asan', value, re.I):
            _fail('instrumented compiler flags are not qualified')
    keys = ('CMAKE_BUILD_TYPE', 'MASKPILOT_ENABLE_ASAN', 'CMAKE_PROJECT_VERSION', 'CMAKE_GENERATOR',
            'CMAKE_CXX_COMPILER', 'CMAKE_CXX_COMPILER_VERSION', 'CMAKE_MAKE_PROGRAM', 'CMAKE_TOOLCHAIN_FILE',
            'CMAKE_CXX_FLAGS', 'CMAKE_CXX_FLAGS_RELEASE', 'CMAKE_EXE_LINKER_FLAGS',
            'CMAKE_EXE_LINKER_FLAGS_RELEASE', 'VCPKG_TARGET_TRIPLET', 'VCPKG_INSTALLED_DIR', 'Qt6_DIR')
    return {name: result[name] for name in keys if name in result}


def _binary_version(path):
    if os.name != 'nt':
        _fail('actual executable VERSIONINFO qualification requires Windows')
    from ctypes import wintypes
    library = ctypes.WinDLL('version.dll', use_last_error=True, winmode=0x800)
    library.GetFileVersionInfoSizeW.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(wintypes.DWORD)]
    library.GetFileVersionInfoSizeW.restype = wintypes.DWORD
    library.GetFileVersionInfoW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID]
    library.GetFileVersionInfoW.restype = wintypes.BOOL
    library.VerQueryValueW.argtypes = [wintypes.LPCVOID, wintypes.LPCWSTR, ctypes.POINTER(wintypes.LPVOID), ctypes.POINTER(wintypes.UINT)]
    library.VerQueryValueW.restype = wintypes.BOOL
    size = library.GetFileVersionInfoSizeW(str(path), None)
    if not 0 < size <= 1024 * 1024:
        _fail('missing or excessive executable VERSIONINFO')
    buffer = ctypes.create_string_buffer(size)
    if not library.GetFileVersionInfoW(str(path), 0, size, buffer):
        _fail('cannot read executable VERSIONINFO')
    pointer, length = wintypes.LPVOID(), wintypes.UINT()
    if not library.VerQueryValueW(buffer, '\\', ctypes.byref(pointer), ctypes.byref(length)) or length.value < 52:
        _fail('missing fixed executable version')
    fields = ctypes.cast(pointer, ctypes.POINTER(wintypes.DWORD * 13)).contents
    if fields[0] != 0xFEEF04BD:
        _fail('invalid fixed executable version')
    return '.'.join(map(str, (fields[2] >> 16, fields[2] & 65535, fields[3] >> 16, fields[3] & 65535)))


def _environment(repo, build, version):
    versions = {name: _binary_version(build / name) for name in REQUIRED_RUNTIME}
    if any(value != version + '.0' for value in versions.values()):
        _fail('actual application/guard version disagrees with configured sources')
    return {'python': platform.python_version(), 'platform': platform.platform(), 'machine': platform.machine(),
            'processor': platform.processor(), 'cache': _cache(repo, build, version), 'binaryVersions': versions,
            'portablePolicySha256': _record(repo / 'scripts/audit_release_paths.py')['sha256']}


def _audit(repo, build):
    """Use the same actual artifact gate as packaging, including signed supplier copies."""
    cache = _cache(repo, build, _version(repo))
    if cache.get('VCPKG_TARGET_TRIPLET') != 'x64-windows-portable':
        _fail('portable Release dependency triplet is required')
    qt_directory = cache.get('Qt6_DIR', '')
    compiler_path = cache.get('CMAKE_CXX_COMPILER', '')
    if not qt_directory or not compiler_path:
        _fail('configured Qt and compiler supplier roots are required')
    qt_directory, compiler = _safe(Path(qt_directory)), _safe(Path(compiler_path))
    if len(qt_directory.parents) < 3 or len(compiler.parents) < 7:
        _fail('configured supplier paths have an unsupported layout')
    qt = qt_directory.parents[2]
    # Compiler is <VS>/VC/Tools/MSVC/<version>/bin/Hostx64/x64/cl.exe.
    redist = compiler.parents[6] / 'Redist/MSVC'
    candidates = sorted((p for p in redist.iterdir() if re.fullmatch(r'\d+\.\d+\.\d+', p.name)),
                        key=lambda p: tuple(map(int, p.name.split('.'))), reverse=True)
    if not candidates:
        _fail('matching local MSVC redistributable source is unavailable')
    crt = candidates[0] / 'x64/Microsoft.VC143.CRT'
    spec = importlib.util.spec_from_file_location('maskpilot_release_paths', repo / 'scripts/audit_release_paths.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.audit_runtime(repo, build, qt, crt)


def capture(repo: Path, build: Path, external: dict[str, Path] = {}) -> dict:
    repo, build = _base(repo), _base(build)
    version = _version(repo)
    environment = _environment(repo, build, version)
    _audit(repo, build)
    tools = {}
    if not isinstance(external, dict) or len(external) > 128:
        _fail('invalid external tool inventory')
    for label, path in sorted(external.items()):
        if not isinstance(label, str) or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]{0,63}', label):
            _fail('invalid external label')
        path = _safe(Path(path))
        tools[label] = {'path': str(path.resolve(strict=True)), **_record(path)}
    commit = _git(repo, 'rev-parse', 'HEAD').decode().strip()
    if not re.fullmatch(r'[0-9a-f]{40,64}', commit):
        _fail('invalid local source commit')
    result = {'schema': 1, 'product': 'MaskPilot', 'version': version, 'sourceCommit': commit,
              'sourceDirty': bool(_git(repo, 'status', '--porcelain=v1', '-z', '--untracked-files=normal')),
              'sources': _sources(repo), 'runtime': _runtime(build), 'external': tools, 'environment': environment}
    if len(json.dumps(result, ensure_ascii=False).encode('utf-8')) > MAX_MANIFEST:
        _fail('baseline exceeds manifest bound')
    return result


def _validate_records(records, *, external=False):
    if not isinstance(records, dict) or len(records) > (128 if external else MAX_FILES):
        _fail('invalid record inventory')
    for name, record in records.items():
        if not external:
            _relative(name)
        elif not isinstance(name, str) or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]{0,63}', name):
            _fail('invalid external label')
        if not isinstance(record, dict) or set(record) != ({'path', 'sha256', 'size'} if external else {'sha256', 'size'}):
            _fail('invalid file record shape')
        if not isinstance(record['sha256'], str) or not re.fullmatch(r'[0-9a-f]{64}', record['sha256']):
            _fail('invalid hash')
        if type(record['size']) is not int or not 0 <= record['size'] <= MAX_FILE:
            _fail('invalid file size')
        if external:
            if not isinstance(record['path'], str) or len(record['path']) > 32768:
                _fail('invalid external path')
            _safe(Path(record['path']))


def verify(manifest: dict, repo: Path, build: Path) -> None:
    if not isinstance(manifest, dict) or set(manifest) != {'schema', 'product', 'version', 'sourceCommit',
            'sourceDirty', 'sources', 'runtime', 'external', 'environment'}:
        _fail('invalid top-level shape')
    if len(json.dumps(manifest, ensure_ascii=False).encode('utf-8')) > MAX_MANIFEST:
        _fail('baseline exceeds manifest bound')
    if type(manifest['schema']) is not int or manifest['schema'] != 1 or manifest['product'] != 'MaskPilot' or type(manifest['sourceDirty']) is not bool:
        _fail('invalid baseline identity')
    if not isinstance(manifest['sourceCommit'], str) or not re.fullmatch(r'[0-9a-f]{40,64}', manifest['sourceCommit']):
        _fail('invalid source provenance')
    sources = manifest['sources']
    if not isinstance(sources, dict) or len(sources) > MAX_FILES:
        _fail('invalid source inventory')
    for name, digest in sources.items():
        _relative(name)
        if not isinstance(digest, str) or not re.fullmatch(r'[0-9a-f]{64}', digest):
            _fail('invalid source hash')
    _validate_records(manifest['runtime'])
    _validate_records(manifest['external'], external=True)
    repo, build = _base(repo), _base(build)
    _git(repo, 'cat-file', '-e', manifest['sourceCommit'] + '^{commit}')
    version = _version(repo)
    _audit(repo, build)
    if manifest['version'] != version or manifest['environment'] != _environment(repo, build, version):
        _fail('version or qualification environment drifted')
    if sources != _sources(repo) or manifest['runtime'] != _runtime(build):
        _fail('native source or compiled runtime inventory drifted')
    for record in manifest['external'].values():
        if {key: record[key] for key in ('sha256', 'size')} != _record(Path(record['path'])):
            _fail('external tool or plugin drifted')


def _write_new(path, value):
    path = _safe(Path(path).absolute(), existing=False)
    if not path.parent.is_dir():
        _fail('output parent must already exist')
    data = json.dumps(value, indent=2, ensure_ascii=False).encode('utf-8') + b'\n'
    if len(data) > MAX_MANIFEST:
        _fail('serialized manifest exceeds bound')
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(prefix='.qualification-', suffix='.tmp', dir=path.parent, delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        # Same-volume hard-link publication is atomic and fails when output already exists.
        os.link(temporary, path, follow_symlinks=False)
    finally:
        if temporary is not None:
            temporary.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_subparsers(dest='action', required=True)
    for name in ('create', 'verify'):
        command = actions.add_parser(name)
        command.add_argument('--root', type=Path, required=True)
        command.add_argument('--build-directory', type=Path, required=True)
        command.add_argument('--output' if name == 'create' else '--manifest', type=Path, required=True)
        if name == 'create':
            command.add_argument('--external', action='append', default=[])
    args = parser.parse_args()
    if args.action == 'create':
        external = {}
        for item in args.external:
            label, separator, path = item.partition('=')
            if not separator or label in external:
                parser.error('external entries must be unique LABEL=ABSOLUTE_PATH')
            external[label] = Path(path)
        _write_new(args.output, capture(args.root, args.build_directory, external))
    else:
        verify(_json(args.manifest.absolute()), args.root, args.build_directory)
    print('PASS local qualification baseline ' + args.action)


if __name__ == '__main__':
    main()
