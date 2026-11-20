"""Offline portable-artifact policy; hashes do not establish publisher authentication.

Supplier PE exceptions require exact SDK/redist bytes AND cached Windows trust verification.
This module never executes an audited image, modifies a supplier file, or downloads anything.
"""
import argparse
import configparser
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import struct
import subprocess
import tempfile
import zipfile

MAX_PE = 64 * 1024 * 1024
MAX_FILE = 2 * 1024**3
MAX_TOTAL = 8 * 1024**3
MAX_FILES = 20000
LOCAL_PE = frozenset(('maskpilot.exe', 'maskpilot_cli.exe', 'maskpilot_native_host.exe',
                      'media_guard/ffmpeg.exe', 'media_guard/ffprobe.exe', 'libcurl.dll',
                      'libz.dll', 'nghttp2.dll', 'sqlite3.dll', 'torrent-rasterbar.dll',
                      'libcrypto-3-x64.dll', 'libssl-3-x64.dll', 'libxml2.dll', 'libssh2.dll'))
QT_DLLS = ('Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'Qt6Network.dll', 'Qt6Svg.dll')
QT_PLUGINS = ('platforms/qwindows.dll', 'platforms/qoffscreen.dll',
              'styles/qmodernwindowsstyle.dll', 'iconengines/qsvgicon.dll',
              'imageformats/qsvg.dll', 'imageformats/qjpeg.dll', 'imageformats/qgif.dll',
              'tls/qschannelbackend.dll')
CRT_DLLS = frozenset(('concrt140.dll', 'msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll',
                     'msvcp140_atomic_wait.dll', 'msvcp140_codecvt_ids.dll', 'vccorlib140.dll',
                     'vcruntime140.dll', 'vcruntime140_1.dll', 'vcruntime140_threads.dll'))
RUNTIME_TREES = frozenset(('media_guard', 'plugins', 'translations', 'platforms', 'styles',
                           'imageformats', 'iconengines', 'tls', 'networkinformation', 'generic'))
POLICY = 'maskpilot-portable-artifacts-1'


def _fail(message):
    raise ValueError('Release path audit: ' + message)


def _relative(name):
    if not isinstance(name, str) or not name or len(name) > 1024:
        _fail('invalid relative filename')
    name = name.replace('\\', '/')
    if name.startswith('/') or ':' in name or any(ord(c) < 32 or ord(c) == 127 for c in name):
        _fail('absolute/device/control archive path')
    for component in name.split('/'):
        if (not component or component in ('.', '..') or component.endswith((' ', '.'))
                or re.search(r'[<>"|?*]', component)
                or re.match(r'^(con|prn|aux|nul|conin\$|conout\$|com[1-9¹²³]|lpt[1-9¹²³])(\.|$)', component, re.I)):
            _fail('unsafe Windows filename')
    return name


def _plain(path):
    for component in (path, *path.parents):
        try:
            attrs = component.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(attrs.st_mode) or getattr(attrs, 'st_file_attributes', 0) & 0x400:
            _fail('links/reparse paths are not audited inputs')


def _digest(path):
    _plain(path)
    sha = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            sha.update(block)
    return sha.hexdigest()


def _private_prefixes(repo, qt_root, crt_root):
    try:
        home = Path.home()
    except RuntimeError as exc:
        raise ValueError('Release path audit: cannot determine user-home prefix policy') from exc
    prefixes = {str(repo.absolute()), str(home), tempfile.gettempdir(),
                str(qt_root.absolute()), str(crt_root.absolute())}
    for variable in ('TEMP', 'TMP', 'USERPROFILE', 'VCToolsInstallDir', 'VSINSTALLDIR',
                     'WindowsSdkDir', 'QT_ROOT_DIR', 'VCPKG_ROOT', 'VCPKG_INSTALLED_DIR'):
        if os.environ.get(variable):
            prefixes.add(os.environ[variable])
    # The fixed redist input binds the actual VS installation even outside bootstrap.ps1.
    for parent in crt_root.absolute().parents:
        if parent.name.casefold() == 'vc':
            relative = crt_root.absolute().relative_to(parent).parts
            if len(relative) >= 5 and tuple(p.casefold() for p in relative[:2]) == ('redist', 'msvc'):
                prefixes.add(str(parent.parent))
    return tuple(p.rstrip('/\\') for p in prefixes if p and len(p.rstrip('/\\')) >= 5)


def _patterns(prefixes):
    result = set()
    for prefix in prefixes:
        base = {prefix.replace('\\', '/'), prefix.replace('/', '\\')}
        for value in tuple(base):
            base.add(json.dumps(value, ensure_ascii=False)[1:-1])
            base.add(json.dumps(value, ensure_ascii=True)[1:-1])
            base.add(value.replace('\\', '\\\\').replace('\\', '\\\\'))
        for value in base:
            for encoding in ('utf-8', 'utf-16-le', 'utf-16-be'):
                result.add(value.encode(encoding).lower())
    return tuple(sorted(result, key=len, reverse=True))


def _scan(stream, patterns, name):
    overlap = max(map(len, patterns), default=1) - 1
    tail = b''
    for block in iter(lambda: stream.read(1024 * 1024), b''):
        sample = (tail + block).lower()
        if any(pattern in sample for pattern in patterns):
            _fail('private host prefix in ' + name)
        tail = sample[-overlap:] if overlap else b''


def pe_codeview(data):
    """Parse PE32+ debug-directory RVAs; unrelated RSDS byte sequences are not evidence."""
    if len(data) > MAX_PE or len(data) < 64 or data[:2] != b'MZ':
        _fail('invalid/bounded PE image')

    def span(offset, size):
        if offset < 0 or size < 0 or offset + size > len(data):
            _fail('PE file bounds')
        return data[offset:offset + size]

    def u16(offset):
        return struct.unpack('<H', span(offset, 2))[0]

    def u32(offset):
        return struct.unpack('<I', span(offset, 4))[0]

    pe = u32(60)
    if span(pe, 4) != b'PE\0\0' or u16(pe + 4) != 0x8664:
        _fail('expected x64 PE signature')
    count, optional_size = u16(pe + 6), u16(pe + 20)
    optional = pe + 24
    span(optional, optional_size)
    if not 1 <= count <= 96 or optional_size < 168 or u16(optional) != 0x20b:
        _fail('PE optional header/section count')
    directory_count = u32(optional + 108)
    if directory_count > 16 or optional_size < 112 + directory_count * 8:
        _fail('PE data directory bounds')
    headers_size = u32(optional + 60)
    if not optional + optional_size + count * 40 <= headers_size <= len(data):
        _fail('PE declared header bounds')
    sections = []
    raw_ranges = []
    for index in range(count):
        section = optional + optional_size + 40 * index
        span(section, 40)
        virtual_size, address, raw_size, pointer = (u32(section + n) for n in (8, 12, 16, 20))
        if raw_size:
            if pointer < headers_size:
                _fail('PE section overlaps its headers')
            span(pointer, raw_size)
            if any(pointer < end and start < pointer + raw_size for start, end in raw_ranges):
                _fail('overlapping PE raw sections')
            raw_ranges.append((pointer, pointer + raw_size))
        sections.append((address, virtual_size, pointer, raw_size))

    def rva(address, size):
        matches = []
        for base, virtual_size, pointer, raw_size in sections:
            if base <= address and address + size <= base + max(virtual_size, raw_size):
                if address - base + size > raw_size:
                    _fail('debug RVA points to unbacked virtual bytes')
                matches.append(pointer + address - base)
        if address + size <= u32(optional + 60):
            matches.append(address)
        if len(matches) != 1:
            _fail('ambiguous/unmapped PE debug RVA')
        span(matches[0], size)
        return matches[0]

    if u32(optional + 108) <= 6:
        return []
    debug_rva, debug_size = u32(optional + 160), u32(optional + 164)
    if not debug_rva and not debug_size:
        return []
    if not debug_size or debug_size % 28 or debug_size > 28 * 4096:
        _fail('PE debug directory size')
    directory = rva(debug_rva, debug_size)
    records = []
    for offset in range(directory, directory + debug_size, 28):
        if u32(offset + 12) != 2:
            continue
        size, address, pointer = (u32(offset + n) for n in (16, 20, 24))
        if not 17 <= size <= 65536 or not pointer:
            _fail('CodeView size/pointer')
        payload = span(pointer, size)
        if address and rva(address, size) != pointer:
            _fail('CodeView RVA/raw pointer mismatch')
        if payload[:4] == b'RSDS':
            if size < 25:
                _fail('truncated RSDS payload')
            prefix, age = 24, struct.unpack('<I', span(pointer + 20, 4))[0]
        elif payload[:4] == b'NB10':
            prefix, age = 16, struct.unpack('<I', span(pointer + 12, 4))[0]
        else:
            _fail('unsupported CodeView signature')
        terminated = payload[prefix:]
        if b'\0' not in terminated:
            _fail('unterminated CodeView PDB name')
        filename, remainder = terminated.split(b'\0', 1)
        if not filename or len(filename) > 4096 or any(remainder):
            _fail('invalid CodeView PDB payload')
        try:
            name = filename.decode('utf-8')
        except UnicodeDecodeError as exc:
            raise ValueError('Release path audit: non-UTF8 CodeView name') from exc
        if any(ord(c) < 32 or ord(c) == 127 for c in name):
            _fail('control in CodeView name')
        records.append({'format': payload[:4].decode('ascii'), 'pdb': name, 'age': age})
    return records


SIGNATURE_SCRIPT = r'''
$ErrorActionPreference='Stop'
[Console]::InputEncoding=[Text.UTF8Encoding]::new($false)
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class MPTrust {
 [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] struct FileInfo {
  public uint size; [MarshalAs(UnmanagedType.LPWStr)] public string path; public IntPtr file; public IntPtr subject;
 }
 [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] struct TrustData {
  public uint size; public IntPtr policy; public IntPtr sip; public uint ui; public uint revocation;
  public uint choice; public IntPtr info; public uint action; public IntPtr state;
  public IntPtr url; public uint flags; public uint context;
 }
 [DllImport("wintrust.dll", ExactSpelling=true, CharSet=CharSet.Unicode)]
 static extern int WinVerifyTrust(IntPtr window, ref Guid action, ref TrustData data);
 public static int Verify(string path) {
  FileInfo file=new FileInfo(); file.size=(uint)Marshal.SizeOf(typeof(FileInfo)); file.path=path;
  IntPtr pointer=Marshal.AllocHGlobal(Marshal.SizeOf(typeof(FileInfo)));
  Marshal.StructureToPtr(file,pointer,false);
  TrustData data=new TrustData(); data.size=(uint)Marshal.SizeOf(typeof(TrustData));
  data.ui=2; data.choice=1; data.info=pointer; data.action=1; data.flags=0x1000|0x2000;
  Guid action=new Guid("00AAC56B-CD44-11d0-8CC2-00C04FC295EE");
  try { return WinVerifyTrust(new IntPtr(-1),ref action,ref data); }
  finally { data.action=2; WinVerifyTrust(new IntPtr(-1),ref action,ref data); Marshal.DestroyStructure(pointer,typeof(FileInfo)); Marshal.FreeHGlobal(pointer); }
 }
}
'@
$request=[Console]::In.ReadToEnd() | ConvertFrom-Json
$items=@(foreach($path in $request.paths) {
 $status=[MPTrust]::Verify($path)
 if($status -ne 0) { throw 'Cached Authenticode verification failed.' }
 $certificate=[Security.Cryptography.X509Certificates.X509Certificate2]::new([Security.Cryptography.X509Certificates.X509Certificate]::CreateFromSignedFile($path))
 try { @{subject=$certificate.Subject; version=[Diagnostics.FileVersionInfo]::GetVersionInfo($path).FileVersion; valid=$true} }
 finally { $certificate.Dispose() }
})
@{items=$items} | ConvertTo-Json -Compress -Depth 4
'''


def _signatures(paths):
    if os.name != 'nt':
        _fail('actual supplier signature verification requires Windows')
    powershell = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32/WindowsPowerShell/v1.0/powershell.exe'
    try:
        result = subprocess.run([str(powershell), '-NoProfile', '-NonInteractive', '-Command', SIGNATURE_SCRIPT],
                                input=json.dumps({'paths': [str(p) for p in paths]}).encode('utf-8'),
                                capture_output=True, timeout=90, check=False)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise ValueError('Release path audit: supplier trust check unavailable') from exc
    if result.returncode or len(result.stdout) > 1024 * 1024:
        _fail('cached supplier trust verification failed')
    try:
        items = json.loads(result.stdout.decode('utf-8-sig'))['items']
    except (ValueError, KeyError, UnicodeDecodeError) as exc:
        raise ValueError('Release path audit: invalid signature result') from exc
    if not isinstance(items, list) or len(items) != len(paths):
        _fail('incomplete supplier trust result')
    return items


def _supplier_files(qt_root, crt_root):
    suppliers = {name: (qt_root / 'bin' / name, 'Qt', 'qt:bin/' + name) for name in QT_DLLS}
    for name in QT_PLUGINS:
        suppliers['plugins/' + name] = (qt_root / 'plugins' / name, 'Qt', 'qt:plugins/' + name)
    suppliers['translations/qtbase_zh_CN.qm'] = (qt_root / 'translations/qtbase_zh_CN.qm', 'Qt-data', 'qt:translations/qtbase_zh_CN.qm')
    _plain(crt_root)
    actual = {p.name for p in crt_root.glob('*.dll')}
    if not {'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll'} <= actual or not actual <= CRT_DLLS:
        _fail('unexpected/incomplete fixed redistributable source set')
    for name in actual:
        suppliers[name] = (crt_root / name, 'Microsoft', 'crt:' + name)
    for source, _, _ in suppliers.values():
        _plain(source)
        if not source.is_file():
            _fail('required fixed supplier input is absent')
    return suppliers


def _runtime_member(name):
    path = PurePosixPath(name)
    return ((len(path.parts) == 1 and (path.suffix.lower() in ('.exe', '.dll') or name == 'qt.conf'))
            or path.parts[0] in RUNTIME_TREES)


def _walk(directory):
    files = {}
    seen = set()
    total = 0
    for parent, directories, names in os.walk(directory, followlinks=False):
        for name in directories + names:
            path = Path(parent) / name
            _plain(path)
            relative = _relative(path.relative_to(directory).as_posix())
            if relative.casefold() in seen:
                _fail('case-fold file/directory collision')
            seen.add(relative.casefold())
            if len(seen) > MAX_FILES:
                _fail('too many artifact entries')
        for name in names:
            path = Path(parent) / name
            size = path.stat().st_size
            if not path.is_file() or size > MAX_FILE:
                _fail('nonregular/excessive artifact file')
            total += size
            if total > MAX_TOTAL:
                _fail('artifact expanded size limit')
            files[path.relative_to(directory).as_posix()] = path
    return files


def _qt_conf(path):
    configuration = configparser.ConfigParser(interpolation=None)
    try:
        configuration.read_string(path.read_text(encoding='utf-8-sig'))
    except (ValueError, OSError) as exc:
        raise ValueError('Release path audit: invalid Qt configuration') from exc
    if configuration.sections() != ['Paths'] or dict(configuration['Paths']) != {'prefix': '.', 'plugins': 'plugins', 'translations': 'translations'}:
        _fail('Qt configuration is not the fixed relative deployment')


def _catalog(repo):
    path = Path(repo) / 'third_party/sources.json'
    _plain(path)
    if path.stat().st_size > 1024 * 1024:
        _fail('pinned source catalog exceeds bound')
    catalog = json.loads(path.read_text(encoding='utf-8-sig'))
    if not isinstance(catalog, dict) or not isinstance(catalog.get('qtVersion'), str) or not re.fullmatch(r'\d+\.\d+\.\d+', catalog['qtVersion']):
        _fail('invalid pinned Qt version')
    entries = catalog.get('archives')
    if not isinstance(entries, list) or len(entries) > 16:
        _fail('invalid source archive catalog')
    seen = set()
    for entry in entries:
        if (not isinstance(entry, dict) or not isinstance(entry.get('file'), str)
                or '/' in entry['file'] or '\\' in entry['file'] or not entry['file'].endswith('.tar.xz')
                or not isinstance(entry.get('sha256'), str) or not re.fullmatch(r'[a-f0-9]{64}', entry['sha256'])
                or type(entry.get('size')) is not int or not 0 < entry['size'] <= MAX_FILE
                or entry['file'].casefold() in seen):
            _fail('invalid pinned source archive record')
        _relative(entry['file'])
        seen.add(entry['file'].casefold())
    return catalog


def _audit_runtime(repo, runtime, qt_root, crt_root):
    repo, runtime, qt_root, crt_root = (Path(p).absolute() for p in (repo, runtime, qt_root, crt_root))
    _plain(runtime)
    if not runtime.is_dir():
        _fail('runtime directory is absent')
    # Build-only cache/log/test evidence is outside this explicitly selected runtime inventory.
    files = {name: path for name, path in _walk(runtime).items() if _runtime_member(name)}
    suppliers = _supplier_files(qt_root, crt_root)
    expected = LOCAL_PE | set(suppliers) | {'qt.conf'}
    if set(files) != expected:
        _fail('runtime inventory differs from fixed own/supplier module set')
    _qt_conf(files['qt.conf'])
    patterns = _patterns(_private_prefixes(repo, qt_root, crt_root))
    records, exceptions, supplier_pe = {}, [], []
    for name, path in sorted(files.items()):
        with path.open('rb') as stream:
            _scan(stream, patterns, name)
        record = {'sha256': _digest(path), 'size': path.stat().st_size}
        if name in suppliers:
            source, publisher, alias = suppliers[name]
            if record['sha256'] != _digest(source):
                _fail('supplier bytes differ from the fixed source: ' + name)
            record['kind'] = 'supplier'
            record['source'] = alias
        else:
            publisher = None
            record['kind'] = 'local' if name in LOCAL_PE else 'configuration'
        if path.suffix.lower() in ('.dll', '.exe'):
            if record['size'] > MAX_PE:
                _fail('PE image exceeds structural parser bound: ' + name)
            codeview = pe_codeview(path.read_bytes())
            record['codeview'] = codeview
            if name in LOCAL_PE:
                if not codeview or any('/' in cv['pdb'] or '\\' in cv['pdb'] or ':' in cv['pdb'] or not cv['pdb'].lower().endswith('.pdb') for cv in codeview):
                    _fail('local CodeView must contain basename-only PDB: ' + name)
            else:
                supplier_pe.append((name, path, publisher))
        records[name] = record
    signatures = _signatures([path for _, path, _ in supplier_pe])
    catalog = _catalog(repo)
    qt_version = catalog['qtVersion']
    if not isinstance(qt_version, str) or not re.fullmatch(r'\d+\.\d+\.\d+', qt_version):
        _fail('invalid pinned Qt version')
    for (name, _, publisher), signature in zip(supplier_pe, signatures):
        if not isinstance(signature, dict):
            _fail('invalid supplier signature result object')
        subject, version = signature.get('subject'), signature.get('version')
        if signature.get('valid') is not True or not isinstance(subject, str) or not isinstance(version, str) or len(subject) > 2048 or len(version) > 128 or not re.fullmatch(r'\d+(?:\.\d+){2,3}', version) or any(ord(c) < 32 for c in subject + version):
            _fail('invalid supplier signature/version metadata')
        if publisher == 'Qt':
            expected_publisher = r'(?:^|,)\s*CN\s*=\s*The Qt Company Oy\s*(?:,|$)'
            if version != qt_version and not version.startswith(qt_version + '.'):
                _fail('authenticated Qt version differs from the pinned source version')
        else:
            expected_publisher = r'(?:^|,)\s*O\s*=\s*Microsoft Corporation\s*(?:,|$)'
        if not re.search(expected_publisher, subject, re.I):
            _fail('unexpected authenticated supplier publisher: ' + name)
        exceptions.append({'file': name, 'publisher': subject, 'version': version,
                           'reason': 'unchanged authenticated supplier CodeView/source provenance'})
    return {'schema': 1, 'policy': POLICY, 'files': records, 'fileCount': len(records),
            'supplierExceptions': exceptions, 'privatePrefixFindings': 0,
            'signaturePolicy': 'cached Windows trust; no online revocation or AIA retrieval',
            'standardOsDefaults': ['Windows system IPC/runtime fallbacks are not host build prefixes']}


def audit_runtime(repo: Path, runtime: Path, qt_root: Path, crt_root: Path) -> dict:
    """Audit the fixed deployment set, allowing build evidence outside that set."""
    try:
        return _audit_runtime(repo, runtime, qt_root, crt_root)
    except (OSError, KeyError, TypeError, UnicodeError, struct.error, configparser.Error) as exc:
        raise ValueError('Release path audit: unreadable/invalid runtime metadata') from exc


def _forbidden(name):
    parts = name.lower().split('/')
    base = parts[-1]
    return (any(p in {'.git', '.idea', '.idea-to-code', 'cmakefiles', 'downloads', 'test-downloads'} for p in parts)
            or base in {'cmakecache.txt', 'compile_commands.json', 'maskpilot-install.json',
                        'browser-registration.json', 'native-bridge.json', 'ipc.token', 'admin.token'}
            or base.endswith(('.pdb', '.log', '.dmp', '.db', '.sqlite', '.sqlite3', '.db-wal', '.db-shm', '.cache'))
            or ('stress' in base or 'qualification' in base or 'baseline' in base) and base.endswith('.json'))


def _inventory(files):
    if 'SHA256SUMS.txt' not in files or files['SHA256SUMS.txt'].stat().st_size > 8 * 1024 * 1024:
        _fail('complete package inventory is absent/excessive')
    entries = {}
    for line in files['SHA256SUMS.txt'].read_text(encoding='utf-8-sig').splitlines():
        match = re.fullmatch(r'([a-fA-F0-9]{64})  (.+)', line)
        if not match:
            _fail('invalid package inventory line')
        name = _relative(match[2])
        if name.casefold() in entries or name == 'SHA256SUMS.txt':
            _fail('duplicate/reserved inventory entry')
        entries[name.casefold()] = (name, match[1].lower())
    if {name for name, _ in entries.values()} != set(files) - {'SHA256SUMS.txt'}:
        _fail('package inventory is not complete')
    for name, digest in entries.values():
        if _digest(files[name]) != digest:
            _fail('package inventory hash mismatch: ' + name)


def audit_directory(repo, directory, qt_root, crt_root, runtime_only=False):
    if runtime_only:
        return audit_runtime(repo, directory, qt_root, crt_root)
    directory = Path(directory).absolute()
    _plain(directory)
    files = _walk(directory)
    _inventory(files)
    patterns = _patterns(_private_prefixes(Path(repo), Path(qt_root), Path(crt_root)))
    catalog = _catalog(repo)
    archives = {entry['file']: entry for entry in catalog['archives']}
    verified_archives = set()
    portable_ports = _walk(Path(repo) / 'cmake/ports')
    if 'LICENSE.vcpkg.txt' not in portable_ports:
        _fail('repository portable-port license/source set is absent')
    verified_ports = set()
    verified_catalog = False
    for name, path in files.items():
        if _forbidden(name):
            _fail('private/debug artifact in package: ' + name)
        if name == 'dependency-sources/sources.json':
            if _digest(path) != _digest(Path(repo) / 'third_party/sources.json'):
                _fail('packaged source catalog differs from repository bytes')
            verified_catalog = True
            with path.open('rb') as stream:
                _scan(stream, patterns, name)
        elif name.startswith('dependency-sources/portable-ports/'):
            relative = name[len('dependency-sources/portable-ports/'):]
            if relative not in portable_ports or _digest(path) != _digest(portable_ports[relative]):
                _fail('unrecognized/modified portable-port source file')
            verified_ports.add(relative)
            with path.open('rb') as stream:
                _scan(stream, patterns, name)
        elif name.startswith('dependency-sources/'):
            entry = archives.get(PurePosixPath(name).name)
            if entry is None or name != 'dependency-sources/' + entry['file'] or path.stat().st_size != entry['size'] or _digest(path) != entry['sha256']:
                _fail('unrecognized/unpinned supplier source archive')
            verified_archives.add(entry['file'])
        else:
            if name.lower().endswith(('.zip', '.tar', '.tar.xz', '.tar.gz', '.7z')):
                _fail('unclassified compressed package payload')
            with path.open('rb') as stream:
                _scan(stream, patterns, name)
    if verified_archives != set(archives):
        _fail('pinned source archive set is incomplete')
    if not verified_catalog or verified_ports != set(portable_ports):
        _fail('source catalog/portable-port source set is incomplete')
    report = audit_runtime(repo, directory, qt_root, crt_root)
    report.update(packageFileCount=len(files), verifiedSourceArchives=len(verified_archives),
                  verifiedPortablePortFiles=len(verified_ports))
    return report


def audit_archive(repo, archive, qt_root, crt_root, runtime_only=False):
    archive = Path(archive)
    _plain(archive)
    if archive.stat().st_size > MAX_FILE:
        _fail('ZIP archive exceeds bound')
    try:
        with archive.open('rb') as source, zipfile.ZipFile(source) as zipped, tempfile.TemporaryDirectory(prefix='maskpilot-artifact-audit-') as temporary:
            entries = zipped.infolist()
            if not entries or len(entries) > MAX_FILES:
                _fail('ZIP entry limit')
            seen, files, roots, total = set(), {}, set(), 0
            for entry in entries:
                name = _relative(entry.filename[:-1] if entry.is_dir() else entry.filename)
                if name.casefold() in seen or entry.flag_bits & 1:
                    _fail('duplicate/encrypted ZIP entry')
                seen.add(name.casefold())
                file_type = (entry.external_attr >> 16) & 0xf000
                if entry.external_attr & 0x400 or file_type not in (0, stat.S_IFREG, stat.S_IFDIR) or file_type == stat.S_IFDIR and not entry.is_dir() or file_type == stat.S_IFREG and entry.is_dir() or entry.external_attr & 0x10 and not entry.is_dir():
                    _fail('ZIP special/reparse entry')
                roots.add(name.split('/')[0])
                if entry.is_dir():
                    continue
                if '/' not in name or entry.file_size > MAX_FILE:
                    _fail('ZIP needs one package directory and bounded regular files')
                total += entry.file_size
                if total > MAX_TOTAL:
                    _fail('ZIP expanded size limit')
                files[name] = entry
            if len(roots) != 1:
                _fail('ZIP has multiple package roots')
            file_names = {name.casefold() for name in files}
            for name in seen:
                parent = PurePosixPath(name).parent
                while str(parent) != '.':
                    if str(parent) in file_names:
                        _fail('ZIP file/directory prefix collision')
                    parent = parent.parent
            root = Path(temporary)
            expanded = 0
            for name, entry in files.items():
                destination = root.joinpath(*PurePosixPath(name).parts)
                destination.parent.mkdir(parents=True, exist_ok=True)
                written = 0
                with zipped.open(entry) as input_file, destination.open('xb') as output:
                    for block in iter(lambda: input_file.read(1024 * 1024), b''):
                        written += len(block)
                        expanded += len(block)
                        if written > entry.file_size or expanded > MAX_TOTAL:
                            _fail('ZIP actual expansion exceeds advertised bound')
                        output.write(block)
                if written != entry.file_size:
                    _fail('ZIP actual size mismatch')
            return audit_directory(repo, root / next(iter(roots)), qt_root, crt_root, runtime_only)
    except (OSError, zipfile.BadZipFile, RuntimeError) as exc:
        raise ValueError('Release path audit: malformed/unreadable ZIP') from exc


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--directory', type=Path)
    source.add_argument('--archive', type=Path)
    parser.add_argument('--qt-root', type=Path, required=True)
    parser.add_argument('--crt-root', type=Path, required=True)
    parser.add_argument('--runtime-only', action='store_true')
    parser.add_argument('--report', type=Path)
    options = parser.parse_args()
    try:
        if options.directory:
            report = audit_directory(options.root, options.directory, options.qt_root, options.crt_root, options.runtime_only)
        else:
            report = audit_archive(options.root, options.archive, options.qt_root, options.crt_root, options.runtime_only)
        encoded = json.dumps(report, ensure_ascii=False, sort_keys=True, indent=2)
        if options.report:
            options.report.write_text(encoded + '\n', encoding='utf-8')
        print(json.dumps({'status': 'passed', 'policy': POLICY, 'files': report['fileCount']}))
        return 0
    except (OSError, ValueError, KeyError, UnicodeError, configparser.Error) as exc:
        print(str(exc))
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
