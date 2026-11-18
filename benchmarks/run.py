"""Windows stress/soak: 100 HTTP tasks, 8 active, 8 segments; stdlib only.

Run after scripts/bootstrap.ps1, supplying built --desktop and --native-host.
Default 300s is a quick stress check; --duration 28800 requests an eight-hour run.
Optional media tools add periodic local synthetic MP4/HLS/DASH phases.
No browser registration/profile changes. An interrupted/short run cannot PASS.
"""
import argparse
import concurrent.futures
import contextlib
import ctypes as ct
from ctypes import wintypes as wt
import hashlib
import functools
import http.server
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import shutil
import socketserver
import sqlite3
import statistics
import struct
import subprocess
import sys
import tempfile
import threading
import time
import traceback
import uuid

SIZE, CHUNK, COUNT = 2 * 1024 * 1024, 16384, 100
ACTIVE = {'Probing', 'Downloading', 'Pausing', 'Verifying', 'Finalizing', 'Cancelling'}
ORIGIN = 'chrome-extension://epdfgbhpbpkpephcfmemmadlblpodnfo/'
kernel, psapi, user = ct.WinDLL('kernel32', use_last_error=True), ct.WinDLL('psapi'), ct.WinDLL('user32')
kernel.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, ct.c_void_p, wt.DWORD, wt.DWORD, wt.HANDLE]
kernel.CreateFileW.restype = wt.HANDLE
kernel.WaitNamedPipeW.argtypes = [wt.LPCWSTR, wt.DWORD]
kernel.CloseHandle.argtypes = [wt.HANDLE]
kernel.PeekNamedPipe.argtypes = [wt.HANDLE, ct.c_void_p, wt.DWORD, ct.c_void_p, ct.POINTER(wt.DWORD), ct.c_void_p]
kernel.ReadFile.argtypes = kernel.WriteFile.argtypes = [wt.HANDLE, ct.c_void_p, wt.DWORD, ct.POINTER(wt.DWORD), ct.c_void_p]
kernel.OpenProcess.argtypes, kernel.OpenProcess.restype = [wt.DWORD, wt.BOOL, wt.DWORD], wt.HANDLE
kernel.SetThreadExecutionState.argtypes, kernel.SetThreadExecutionState.restype = [wt.DWORD], wt.DWORD

class Memory(ct.Structure):
    _fields_ = [('cb', wt.DWORD), ('faults', wt.DWORD)] + [(name, ct.c_size_t) for name in
        ['peak_working', 'working', 'peak_paged', 'paged', 'peak_nonpaged', 'nonpaged', 'pagefile', 'peak_pagefile', 'private']]

psapi.GetProcessMemoryInfo.argtypes = [wt.HANDLE, ct.POINTER(Memory), wt.DWORD]
psapi.EnumProcessModulesEx.argtypes = [wt.HANDLE, ct.POINTER(wt.HMODULE), wt.DWORD, ct.POINTER(wt.DWORD), wt.DWORD]
psapi.GetModuleFileNameExW.argtypes = [wt.HANDLE, wt.HMODULE, wt.LPWSTR, wt.DWORD]
psapi.GetModuleFileNameExW.restype = wt.DWORD
user.GetGuiResources.argtypes, user.GetGuiResources.restype = [wt.HANDLE, wt.DWORD], wt.DWORD


def is_system_module(path, system_root):
    return any(path.is_relative_to(system_root / directory)
               for directory in ('System32', 'SysWOW64', 'WinSxS'))


def verify_loaded_runtime(handle, manifest, build):
    """Check the actual desktop mappings, including accidental developer PATH fallback."""
    expected = {build / name for name in manifest['runtime']}
    expected.update(Path(record['path']).resolve() for record in manifest['external'].values())
    pinned_names = {path.name.casefold() for path in expected}
    system_root = Path(os.environ['SystemRoot']).resolve()
    modules = (wt.HMODULE * 2048)()
    needed = wt.DWORD()
    if not psapi.EnumProcessModulesEx(handle, modules, ct.sizeof(modules), ct.byref(needed), 3):
        raise ct.WinError(ct.get_last_error())
    if needed.value > ct.sizeof(modules) or needed.value % ct.sizeof(wt.HMODULE):
        raise RuntimeError('Loaded module inventory exceeds its bounded buffer')
    result = {}
    for module in modules[:needed.value // ct.sizeof(wt.HMODULE)]:
        name = ct.create_unicode_buffer(32768)
        count = psapi.GetModuleFileNameExW(handle, module, name, len(name))
        if not count or count >= len(name) - 1:
            raise RuntimeError('Cannot identify a loaded desktop module')
        path = Path(name.value).resolve(strict=True)
        if path in expected:
            result[str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
        elif path.name.casefold() in pinned_names or not is_system_module(path, system_root):
            raise RuntimeError('Unqualified desktop module loaded: ' + path.name)
    return result

def write_report(path, value):
    """Publish complete JSON, tolerating brief Windows reader/scan handles."""
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2), encoding='utf-8')
    deadline = time.monotonic() + 2
    while True:
        try:
            temporary.replace(path)
            return
        except PermissionError as error:
            if error.winerror not in (5, 32, 33) or time.monotonic() >= deadline:
                raise
            time.sleep(.025)

def failure_dump(process, directory):
    """Capture the isolated fixture desktop before cleanup; never mask its failure."""
    target = directory / 'failure.dmp'
    handle = kernel.OpenProcess(0x0410, False, process.pid)
    if not handle:
        raise ct.WinError(ct.get_last_error())
    output = kernel.CreateFileW(str(target), 0x40000000, 0, None, 2, 0, None)
    if output == ct.c_void_p(-1).value:
        kernel.CloseHandle(handle)
        raise ct.WinError(ct.get_last_error())
    try:
        library = ct.WinDLL(str(Path(os.environ['SystemRoot']) / 'System32/dbghelp.dll'), use_last_error=True)
        write = library.MiniDumpWriteDump
        write.argtypes = [wt.HANDLE, wt.DWORD, wt.HANDLE, wt.DWORD, ct.c_void_p, ct.c_void_p, ct.c_void_p]
        write.restype = wt.BOOL
        if not write(handle, process.pid, output, 0x1160, None, None, None):
            raise ct.WinError(ct.get_last_error())
        return str(target)
    finally:
        kernel.CloseHandle(output)
        kernel.CloseHandle(handle)

def frame(value):
    payload = json.dumps(value, ensure_ascii=False).encode()
    return struct.pack('<I', len(payload)) + payload

class Bridge:
    def __init__(self, data):
        name = hashlib.sha256(data.resolve().as_posix().encode()).hexdigest()[:24]
        self.pipe = r'\\.\pipe\maskpilot-' + name
        self.token = (data / 'ipc.token').read_text().strip()

    def call(self, command, *, read_delay=0, **fields):
        request = dict(version=1, requestId=uuid.uuid4().hex, command=command, token=self.token, **fields)
        message = frame(request)
        if len(message) > 4096:
            raise ValueError('Benchmark request exceeds its bounded synchronous pipe-write size')
        kernel.WaitNamedPipeW(self.pipe, 5000)
        handle = kernel.CreateFileW(self.pipe, 0xC0000000, 0, None, 3, 0, None)
        if handle == ct.c_void_p(-1).value:
            raise ct.WinError(ct.get_last_error())
        try:
            count = wt.DWORD()
            if not kernel.WriteFile(handle, message, len(message), ct.byref(count), None) or count.value != len(message):
                raise ct.WinError(ct.get_last_error())
            if read_delay:
                time.sleep(read_delay)
            def read_exact(length):
                result, deadline = bytearray(), time.monotonic() + 10
                while len(result) < length:
                    available = wt.DWORD()
                    if not kernel.PeekNamedPipe(handle, None, 0, None, ct.byref(available), None):
                        raise ct.WinError(ct.get_last_error())
                    if not available.value:
                        if time.monotonic() > deadline:
                            raise TimeoutError('GUI-thread IPC response exceeded ten seconds')
                        time.sleep(.002)
                        continue
                    buffer = ct.create_string_buffer(min(length - len(result), available.value))
                    if not kernel.ReadFile(handle, buffer, len(buffer), ct.byref(count), None) or not count.value:
                        raise ct.WinError(ct.get_last_error())
                    result.extend(buffer.raw[:count.value])
                return bytes(result)
            length = struct.unpack('<I', read_exact(4))[0]
            if not 0 < length <= 1024 * 1024:
                raise ValueError('Invalid IPC response length')
            response = json.loads(read_exact(length))
            if response.get('version') != 1 or response.get('requestId') != request['requestId'] or not response.get('ok'):
                raise RuntimeError('Rejected benchmark command: ' + str(response.get('error', response)))
            return response
        finally:
            kernel.CloseHandle(handle)

class Fixture(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads, request_queue_size = True, 128
    def __init__(self):
        super().__init__(('127.0.0.1', 0), Handler)
        self.lock, self.ranges, self.inflight, self.peak = threading.Lock(), {}, 0, 0
    def handle_error(self, *args):
        pass  # A cancelled client intentionally closes its local fixture connection.

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *args):
        pass
    def do_GET(self):
        if not re.fullmatch(r'/file/\d+/\d+', self.path):
            self.send_error(404); return
        match = re.fullmatch(r'bytes=(\d+)-(\d*)', self.headers.get('Range', ''))
        begin, end = (int(match[1]), int(match[2] or SIZE - 1)) if match else (0, SIZE - 1)
        if not 0 <= begin <= end < SIZE:
            self.send_response(416); self.send_header('Content-Length', '0'); self.end_headers(); return
        seed = hashlib.sha256(self.path.encode()).digest()
        self.send_response(206 if match else 200)
        for name, value in [('Content-Type', 'application/octet-stream'), ('Content-Length', str(end - begin + 1)), ('ETag', '"' + seed.hex() + '"')]:
            self.send_header(name, value)
        if match:
            self.send_header('Content-Range', f'bytes {begin}-{end}/{SIZE}')
        self.end_headers()
        with self.server.lock:
            self.server.inflight += 1; self.server.peak = max(self.server.peak, self.server.inflight)
            if match and end > 0:
                self.server.ranges.setdefault(self.path, set()).add(begin)
        try:
            for position in range(begin, end + 1, CHUNK):
                length = min(CHUNK, end + 1 - position); offset = position % len(seed)
                self.wfile.write((seed * ((length + offset + 31) // 32))[offset:offset + length])
                self.wfile.flush(); time.sleep(.01)
        except (ConnectionError, OSError):
            pass
        finally:
            with self.server.lock:
                self.server.inflight -= 1

class MediaHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args): pass

class MediaFixture(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    def handle_error(self, *args): pass

def run(options):
    root = Path(tempfile.mkdtemp(prefix='maskpilot-stress-')).resolve()
    data, outputs, hostdir = root / 'data', root / 'outputs', root / 'host'
    for folder in (data / 'MaskPilot', outputs, hostdir): folder.mkdir(parents=True, exist_ok=True)
    (data / 'MaskPilot/MaskPilot.ini').write_text('[General]\nlanguage=en\nconcurrentTasks=8\nconnections=8\ncompletionAction=0\ndownloadDirectory=' + outputs.as_posix() + '\n')
    if options.media_parser:
        with (data / 'MaskPilot/MaskPilot.ini').open('a', encoding='utf-8') as settings:
            settings.write('mediaDownloader=' + options.media_parser.as_posix() + '\nmediaMuxer=' + options.media_muxer.as_posix() + '\n')
    host = hostdir / options.native_host.name; shutil.copyfile(options.native_host, host)
    (hostdir / 'native-bridge.json').write_text(json.dumps(dict(allowedOrigins=[ORIGIN], desktopPath=str(options.desktop), dataDirectory=str(data))))
    environment = dict(os.environ, QT_QPA_PLATFORM=options.qt_platform, NO_PROXY='127.0.0.1,localhost', no_proxy='127.0.0.1,localhost')
    environment['PATH'] = str(options.desktop.parent) + os.pathsep + str(options.native_host.parent) + os.pathsep + environment.get('PATH', '')
    binaries = [options.desktop, options.native_host] + ([options.media_parser, options.media_muxer, options.media_muxer.with_name('ffprobe.exe')] if options.media_parser else [])
    report = dict(status='starting', mode='HTTP cycles with periodic local synthetic MP4/HLS/DASH phase' if options.media_parser else 'HTTP Range only; no media/TLS soak claim', requestedSeconds=options.duration,
                  command=[sys.executable, str(Path(__file__).resolve()), *sys.argv[1:]], artifacts=str(root),
                  python=sys.version, platform=sys.platform, noExternalSiteOrTlsClaim=True,
                  binaries={str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in binaries},
                  thresholds=dict(memoryMiB=512, quarterDriftMiB=64, guiPingP95ms=200, maxSampleGapSeconds=30), maxSampleGapSeconds=0, cycles=0, verifiedFiles=0,
                  pausedResumed=0, peakActive=0, segmentLayouts=0, mediaCycles=0, mediaOutputs=[], samples=[], pingMs=[])
    harness_path = Path(__file__).resolve()
    report['harnessSha256'] = hashlib.sha256(harness_path.read_bytes()).hexdigest()
    qualification = None
    qualification_path = getattr(options, 'release_manifest', None)
    started, last_sample, last_write, last_console = None, 0, 0, 0
    process, memory_handle, fixture, media_fixture, accepted = None, None, None, None, False
    def save(status=None):
        nonlocal last_write, last_console
        if status: report['status'] = status
        report['elapsedSeconds'] = time.monotonic() - started if started else 0
        write_report(options.report, report)
        last_write = time.monotonic()
        if report['status'] == 'running' and last_write - last_console >= 45:
            progress = {key: report[key] for key in
                        ['status', 'elapsedSeconds', 'cycles', 'verifiedFiles',
                         'pausedResumed', 'peakActive', 'mediaCycles']}
            print(json.dumps(dict(event='progress', **progress)), flush=True)
            last_console = last_write
    try:
        if getattr(options, 'isolated_runtime', False):
            if not qualification_path or not (options.desktop.parent / 'Qt6Core.dll').is_file():
                raise ValueError('Isolated qualification requires a baseline and locally deployed Qt runtime')
            environment['PATH'] = str(options.desktop.parent) + os.pathsep + str(options.native_host.parent) + os.pathsep + os.environ['SystemRoot'] + '/System32' + os.pathsep + os.environ['SystemRoot']
            for name in ('QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH', 'QML2_IMPORT_PATH', 'QT_ROOT_DIR'):
                environment.pop(name, None)
            environment['QT_PLUGIN_PATH'] = str(options.desktop.parent / 'plugins')
        if qualification_path:
            repository = harness_path.parents[1]
            build = (getattr(options, 'build_directory', None) or options.desktop.parent).resolve(strict=True)
            if not options.desktop.samefile(build / 'maskpilot.exe') or not options.native_host.samefile(build / 'maskpilot_native_host.exe'):
                raise ValueError('Qualification must bind the actual desktop and native host build directory')
            qualification_path = Path(qualification_path).resolve(strict=True)
            if qualification_path.stat().st_size > 4 * 1024**2:
                raise ValueError('Qualification manifest exceeds 4 MiB')
            raw = qualification_path.read_bytes()
            manifest = json.loads(raw)
            helper_path = repository / 'scripts/release_manifest.py'
            spec = importlib.util.spec_from_file_location('maskpilot_release_qualification', helper_path)
            helper = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(helper)
            helper.verify(manifest, repository, build)
            external = {Path(value['path']).resolve() for value in manifest['external'].values()}
            pinned = external | {build / name for name in manifest['runtime']}
            required_external = [options.media_parser, options.media_muxer, options.media_muxer.with_name('ffprobe.exe')] if options.media_parser else []
            for name in ('QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH'):
                for directory in environment.get(name, '').split(os.pathsep):
                    if directory:
                        required_external.extend(Path(directory).resolve(strict=True).rglob('*.dll'))
            for required in required_external:
                if not any(required.samefile(value) for value in pinned):
                    raise ValueError('Actual helper or Qt plugin is absent from the qualification baseline: ' + required.name)
            qualification = (helper, manifest, repository, build, helper_path,
                             hashlib.sha256(helper_path.read_bytes()).hexdigest())
            report['qualification'] = dict(manifestSha256=hashlib.sha256(raw).hexdigest(),
                                          sourceCommit=manifest['sourceCommit'], sourceDirty=manifest['sourceDirty'],
                                          version=manifest['version'], environment=manifest['environment'],
                                          helperSha256=qualification[-1], verifiedBeforeLaunch=True,
                                          verifiedAfterCleanup=False)
        if not kernel.SetThreadExecutionState(0x80000001):
            raise ct.WinError(ct.get_last_error())
        def tool(binary, arguments):
            result = subprocess.run([str(binary), '-hide_banner', '-loglevel', 'error', '-nostdin', *arguments], capture_output=True, env=environment, timeout=30, creationflags=subprocess.CREATE_NO_WINDOW)
            if result.returncode: raise RuntimeError('Media verification/generation failed: ' + result.stderr.decode(errors='replace')[-700:])
            return result.stdout
        def decoded_video_hash(filename):
            return tool(options.media_muxer, ['-i', str(filename), '-map', '0:v:0', '-an', '-c:v', 'rawvideo', '-pix_fmt', 'yuv420p', '-f', 'hash', '-hash', 'sha256', '-']).decode().strip()
        if options.media_parser:
            media_inputs = root / 'media-inputs'; media_inputs.mkdir()
            source = options.media_fixtures / 'source.mp4'
            if not source.is_file() or source.stat().st_size > 100 * 1024**2: raise ValueError('Expected a bounded source.mp4 fixture below 100 MiB')
            tool(options.media_muxer, ['-y', '-i', str(source), '-t', '3', '-map', '0', '-c', 'copy', str(media_inputs / 'source.mp4')])
            for arguments in (['-hls_time', '1', '-f', 'hls', (media_inputs / 'playlist.m3u8').as_posix()], ['-f', 'dash', (media_inputs / 'manifest.mpd').as_posix()]):
                tool(options.media_muxer, ['-y', '-i', str(media_inputs / 'source.mp4'), '-t', '3', '-map', '0', '-c', 'copy', *arguments])
            reference_video = decoded_video_hash(media_inputs / 'source.mp4')
            report['mediaSourceSha256'] = hashlib.sha256(source.read_bytes()).hexdigest()
            media_fixture = MediaFixture(('127.0.0.1', 0), functools.partial(MediaHandler, directory=str(media_inputs)))
            threading.Thread(target=media_fixture.serve_forever, daemon=True).start()
        log = (root / 'desktop.log').open('wb')
        process = subprocess.Popen([str(options.desktop), '--background', '--data-dir', str(data)], env=environment,
                                   stdout=log, stderr=log, creationflags=subprocess.CREATE_NO_WINDOW)
        report['desktopPid'] = process.pid
        deadline = time.monotonic() + 30
        while not (data / 'ipc.token').exists():
            if process.poll() is not None or time.monotonic() > deadline: raise RuntimeError('Desktop startup failed')
            time.sleep(.05)
        probe = dict(version=1, requestId=uuid.uuid4().hex, command='ping')
        before = time.perf_counter()
        result = subprocess.run([str(host), ORIGIN], input=frame(probe), capture_output=True, env=environment,
                                timeout=20, creationflags=subprocess.CREATE_NO_WINDOW)
        report['nativeHostSpawnPingMs'] = (time.perf_counter() - before) * 1000
        if len(result.stdout) < 4 or struct.unpack('<I', result.stdout[:4])[0] != len(result.stdout) - 4: raise RuntimeError('Native framing failed')
        response = json.loads(result.stdout[4:])
        if not response.get('ok') or response.get('requestId') != probe['requestId']: raise RuntimeError('Actual native host handshake failed')
        bridge = Bridge(data)
        memory_handle = kernel.OpenProcess(0x410, False, process.pid)
        if not memory_handle: raise ct.WinError(ct.get_last_error())
        if qualification:
            report['mappedRuntimeBeforeLoad'] = verify_loaded_runtime(memory_handle, qualification[1], qualification[3])
        fixture = Fixture(); threading.Thread(target=fixture.serve_forever, daemon=True).start()
        started = time.monotonic(); save('running')
        def observe(force=False):
            nonlocal last_sample
            if options.stop_file and options.stop_file.exists():
                raise InterruptedError('Stopped by the benchmark stop file; this run is not accepted')
            if process.poll() is not None: raise RuntimeError('Persistent desktop crashed')
            now = time.monotonic()
            if force or now - last_sample >= 1:
                if last_sample:
                    gap = now - last_sample
                    report['maxSampleGapSeconds'] = max(report['maxSampleGapSeconds'], gap)
                    if gap > 30:
                        raise RuntimeError('Sampling gap exceeded 30 seconds; suspended or unobserved time cannot count as soak load')
                before = time.perf_counter(); bridge.call('ping'); report['pingMs'].append((time.perf_counter() - before) * 1000)
                memory = Memory(); memory.cb = ct.sizeof(memory)
                if not psapi.GetProcessMemoryInfo(memory_handle, ct.byref(memory), ct.sizeof(memory)): raise ct.WinError(ct.get_last_error())
                report['samples'].append(dict(seconds=now - started, private=memory.private, working=memory.working,
                                             gdi=user.GetGuiResources(memory_handle, 0), user=user.GetGuiResources(memory_handle, 1)))
                if max(memory.private, memory.working) >= 512 * 1024**2: raise RuntimeError('Sampled desktop memory exceeded 512 MiB')
                last_sample = now
            if now - last_write >= 15: save()
        with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
            def bulk(command, values):
                futures = [pool.submit(bridge.call, command, **value) for value in values]
                results = []
                for future in concurrent.futures.as_completed(futures): results.append(future.result()); observe()
                return results
            while time.monotonic() - started < options.duration:
                cycle = report['cycles'] + 1
                specs = [dict(url=f'http://127.0.0.1:{fixture.server_port}/file/{cycle}/{index}', filename=f'cycle-{cycle}-{index}.bin') for index in range(COUNT)]
                added = bulk('prepare', specs); identifiers = {item['taskId'] for item in added}
                with contextlib.closing(sqlite3.connect(data / 'tasks.db')) as database:
                    count, low, high, state = database.execute('SELECT count(*),min(connections),max(connections),min(state) FROM tasks').fetchone()
                if (count, low, high, state) != (COUNT, 8, 8, 5): raise AssertionError('Incorrect durable prepared workload')
                bulk('resume', [dict(taskId=identifier) for identifier in identifiers])
                paused, attempted, deadline = set(), False, time.monotonic() + 180
                while True:
                    observe(); tasks = bridge.call('list').get('tasks', [])
                    live = [task for task in tasks if task.get('state') in ACTIVE]
                    report['peakActive'] = max(report['peakActive'], len(live))
                    if len(live) > 8: raise AssertionError('Active-task limit exceeded')
                    if not attempted and live:
                        paused = {task['id'] for task in live[:3]}; bulk('pause', [dict(taskId=i) for i in paused]); attempted = True
                    for task in tasks:
                        if task['id'] in paused and task.get('state') == 'Paused':
                            bridge.call('resume', taskId=task['id']); paused.remove(task['id']); report['pausedResumed'] += 1
                        if task.get('state') in {'Failed', 'Needs attention', 'Cancelled'}: raise AssertionError(str(task))
                    if len(tasks) == COUNT and all(task.get('state') == 'Completed' for task in tasks): break
                    if time.monotonic() > deadline: raise TimeoutError('Cycle did not complete within 180 seconds')
                    time.sleep(.05)
                with contextlib.closing(sqlite3.connect(data / 'tasks.db')) as database: records = list(database.execute('SELECT url,destination FROM tasks'))
                for url, filename in records:
                    seed = hashlib.sha256(url.split(f':{fixture.server_port}', 1)[1].encode()).digest()
                    expected = hashlib.sha256(seed * (SIZE // 32)).digest(); target = Path(filename).resolve()
                    if target.parent != outputs.resolve() or target.stat().st_size != SIZE or hashlib.sha256(target.read_bytes()).digest() != expected: raise AssertionError('Output identity/bytes mismatch')
                    report['verifiedFiles'] += 1; target.unlink()  # Delete only hash-verified, run-owned files.
                bulk('remove', [dict(taskId=i) for i in identifiers])
                deadline = time.monotonic() + 10
                while bridge.call('list').get('tasks'):
                    if time.monotonic() > deadline: raise AssertionError('Own history was not removed')
                    time.sleep(.05)
                if any(p.is_file() for p in (data / 'parts').rglob('*')): raise AssertionError('Completed cache was not reclaimed')
                with fixture.lock:
                    expected_starts = set(range(0, SIZE, SIZE // 8))
                    report['segmentLayouts'] += sum(expected_starts <= starts for starts in fixture.ranges.values()); fixture.ranges.clear()
                    report['peakFixtureConnections'] = fixture.peak
                if media_fixture and (cycle == 1 or cycle % options.media_every == 0):
                    for name in ('source.mp4', 'playlist.m3u8', 'manifest.mpd'):
                        item = bridge.call('add', url=f'http://127.0.0.1:{media_fixture.server_port}/{name}', filename=f'media-{cycle}-{name}.mkv', media=True)
                        deadline = time.monotonic() + 120
                        while True:
                            observe()
                            with contextlib.closing(sqlite3.connect(data / 'tasks.db')) as database:
                                row = database.execute('SELECT state,destination,error FROM tasks WHERE id=?', (item['taskId'],)).fetchone()
                            if row and row[0] == 9: break
                            if row and row[0] in (10, 13): raise RuntimeError(row[2])
                            if time.monotonic() > deadline: raise TimeoutError('Media fixture completion exceeded 120 seconds')
                            time.sleep(.1)
                        target = Path(row[1]).resolve()
                        if target.parent != outputs.resolve() or not target.is_file(): raise AssertionError('Invalid media destination')
                        media_hash = decoded_video_hash(target)
                        probe = options.media_muxer.with_name('ffprobe.exe')
                        checked = subprocess.run([str(probe), '-v', 'error', '-show_entries', 'stream=codec_type', '-of', 'json', str(target)], capture_output=True, env=environment, timeout=15, creationflags=subprocess.CREATE_NO_WINDOW)
                        if checked.returncode or not {'audio', 'video'} <= {s['codec_type'] for s in json.loads(checked.stdout)['streams']} or media_hash != reference_video: raise AssertionError('Media audio/video content validation failed')
                        report['mediaOutputs'].append(dict(cycle=cycle, source=name, sha256=hashlib.sha256(target.read_bytes()).hexdigest(), decodedVideo=media_hash))
                        target.unlink(); bridge.call('remove', taskId=item['taskId'])
                        deadline = time.monotonic() + 10
                        while bridge.call('list').get('tasks'):
                            if time.monotonic() > deadline: raise AssertionError('Media history was not removed')
                            time.sleep(.05)
                        if any(p.is_file() for p in (data / 'parts').rglob('*')): raise AssertionError('Media cache was not reclaimed')
                    report['mediaCycles'] += 1
                report['cycles'] = cycle; observe(True); save()
        elapsed = time.monotonic() - started; samples = report['samples']
        first = [s['private'] for s in samples if s['seconds'] <= elapsed / 4]
        last = [s['private'] for s in samples if s['seconds'] >= elapsed * 3 / 4]
        ping = sorted(report['pingMs']); report['guiPingP95ms'] = ping[math.ceil(len(ping) * .95) - 1]
        report['quarterDriftMiB'] = (statistics.median(last) - statistics.median(first)) / 1024**2
        if not (elapsed >= options.duration and report['cycles'] and report['verifiedFiles'] >= COUNT and report['pausedResumed'] and report['peakActive'] == 8 and report['segmentLayouts'] and len(samples) >= 8 and report['quarterDriftMiB'] < 64 and report['guiPingP95ms'] < 200 and (not options.media_parser or report['mediaCycles'])): raise AssertionError('Duration/load/pause/drift/latency/media acceptance failed')
        report['workloadElapsedSeconds'] = elapsed
        if qualification:
            report['mappedRuntimeAfterLoad'] = verify_loaded_runtime(memory_handle, qualification[1], qualification[3])
        save('finishing')
        accepted = True
    except BaseException as error:
        accepted = False
        report['failure'] = f'{type(error).__name__}: {error}'
        report['traceback'] = traceback.format_exc()
        report['desktopExitBeforeCleanup'] = process.poll() if process else None
        if process and process.poll() is None:
            try:
                report['failureDump'] = failure_dump(process, root)
            except Exception as diagnostic_error:
                report['failureDumpError'] = f'{type(diagnostic_error).__name__}: {diagnostic_error}'
        save('failed')
    finally:
        kernel.SetThreadExecutionState(0x80000000)
        try:
            if memory_handle: kernel.CloseHandle(memory_handle)
            if process and process.poll() is None:
                process.terminate()
                try: process.wait(timeout=10)
                except subprocess.TimeoutExpired: process.kill(); process.wait(timeout=5)
            if fixture: fixture.shutdown(); fixture.server_close()
            if media_fixture: media_fixture.shutdown(); media_fixture.server_close()
            if 'log' in locals(): log.close()
            report['processCleaned'] = process is None or process.poll() is not None
            if accepted and qualification:
                helper, manifest, repository, build, helper_path, helper_hash = qualification
                if (hashlib.sha256(qualification_path.read_bytes()).hexdigest() != report['qualification']['manifestSha256'] or
                        hashlib.sha256(helper_path.read_bytes()).hexdigest() != helper_hash or
                        hashlib.sha256(harness_path.read_bytes()).hexdigest() != report['harnessSha256']):
                    raise RuntimeError('Qualification baseline or harness changed during the load')
                helper.verify(manifest, repository, build)
                report['qualification']['verifiedAfterCleanup'] = True
            if accepted and not options.keep_artifacts:
                if root.parent != Path(tempfile.gettempdir()).resolve() or not root.name.startswith('maskpilot-stress-'): raise RuntimeError('Refusing unsafe artifact cleanup')
                shutil.rmtree(root); report['artifactsRemoved'] = True
            if accepted: report['status'] = 'passed'
        except BaseException as error:
            report['status'] = 'failed'; report['cleanupFailure'] = f'{type(error).__name__}: {error}'
        save(); print(json.dumps({key:report.get(key) for key in ['status','elapsedSeconds','cycles','verifiedFiles','pausedResumed','peakActive','guiPingP95ms','quarterDriftMiB','processCleaned','failure']}, indent=2))
    return 0 if report['status'] == 'passed' else 1

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--desktop', type=Path, required=True); parser.add_argument('--native-host', type=Path, required=True)
    parser.add_argument('--duration', type=int, default=300); parser.add_argument('--report', type=Path, default=Path('out/qa/stress.json'))
    parser.add_argument('--qt-platform', default='offscreen'); parser.add_argument('--keep-artifacts', action='store_true')
    parser.add_argument('--stop-file', type=Path, help='Gracefully stop without accepting this run when the file appears')
    parser.add_argument('--release-manifest', type=Path, help='Verify an immutable local runtime/source baseline before launch and after cleanup')
    parser.add_argument('--build-directory', type=Path, help='Build root bound by --release-manifest; defaults to the actual desktop directory')
    parser.add_argument('--isolated-runtime', action='store_true', help='Run a deployed qualification runtime with developer PATH and Qt overrides removed')
    parser.add_argument('--media-parser', type=Path); parser.add_argument('--media-muxer', type=Path)
    parser.add_argument('--media-fixtures', type=Path, default=Path('out/qa/media-fixtures')); parser.add_argument('--media-every', type=int, default=10)
    arguments = parser.parse_args()
    if arguments.duration < 30: parser.error('--duration must be at least 30 seconds; 300 is the quick profile')
    if bool(arguments.media_parser) != bool(arguments.media_muxer) or arguments.media_every < 1: parser.error('Supply both media tools and a positive media cycle interval')
    if arguments.media_parser:
        arguments.media_parser = arguments.media_parser.resolve(strict=True); arguments.media_muxer = arguments.media_muxer.resolve(strict=True)
        arguments.media_fixtures = arguments.media_fixtures.resolve(strict=True)
        if not arguments.media_muxer.with_name('ffprobe.exe').is_file(): parser.error('ffprobe.exe must accompany the media muxer')
    arguments.desktop = arguments.desktop.resolve(strict=True); arguments.native_host = arguments.native_host.resolve(strict=True)
    sys.exit(run(arguments))
