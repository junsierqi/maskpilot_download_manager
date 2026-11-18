"""Real adaptive Range requests, durable layout transitions and process recovery."""
import argparse
import ctypes
from contextlib import contextmanager
import hashlib
import http.server
import json
import os
from pathlib import Path
import re
import queue
import select
import shlex
import socket
import socketserver
import subprocess
import tempfile
import threading
import time


# Distinct blocks detect incorrectly ordered extents; a repeated byte pattern would not.
DATA = b''.join(hashlib.sha256(n.to_bytes(4, 'little')).digest() * 128 for n in range(8192))
TOTAL = len(DATA)
DIGEST = hashlib.sha256(DATA).hexdigest()
ETAG = '"adaptive-v1"'


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True

    def __init__(self):
        self.lock = threading.Lock()
        self.requests = []
        super().__init__(('127.0.0.1', 0), Handler)

    def snapshot(self, route):
        with self.lock:
            return [dict(x, connection=None) for x in self.requests if x['route'] == route]

    def retire_disconnected(self):
        for item in self.requests:
            if item['done']:
                continue
            connection = item['connection']
            try:
                readable, _, _ = select.select([connection], [], [], 0)
                if readable and not connection.recv(1, socket.MSG_PEEK):
                    item['done'] = True
                    item['abandoned'] = True
            except (OSError, ValueError):
                item['done'] = True
                item['abandoned'] = True

    def handle_error(self, request, client_address):
        import sys
        if not isinstance(sys.exc_info()[1], (ConnectionError, OSError)):
            super().handle_error(request, client_address)


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def do_GET(self):
        match = re.fullmatch(r'bytes=(\d+)-(\d+)', self.headers.get('Range', ''))
        begin, end = (int(match[1]), int(match[2])) if match else (0, TOTAL - 1)
        if not 0 <= begin <= end < TOTAL:
            self.send_error(416)
            return
        probe = begin == end == 0
        with self.server.lock:
            self.server.retire_disconnected()
            active = sum(not x['done'] and not x['probe'] and x['route'] == self.path
                         for x in self.server.requests)
            item = dict(route=self.path, begin=begin, end=end, started=time.monotonic(),
                        ended=None, done=False, abandoned=False, probe=probe,
                        active=active + int(not probe), sent=0, connection=self.connection)
            self.server.requests.append(item)
        self.send_response(206 if match else 200)
        self.send_header('ETag', ETAG)
        self.send_header('Content-Length', str(end - begin + 1))
        self.send_header('Connection', 'close')
        self.close_connection = True
        if match:
            self.send_header('Content-Range', f'bytes {begin}-{end}/{TOTAL}')
        self.end_headers()
        slow_start = TOTAL * 7 // 8 if self.path.startswith('/eight/') else TOTAL // 2
        try:
            for offset in range(begin, end + 1, 16384):
                length = min(16384, end + 1 - offset)
                self.wfile.write(DATA[offset:offset + length])
                self.wfile.flush()
                with self.server.lock:
                    item['sent'] += length
                if not probe and begin >= slow_start:
                    time.sleep(.008)
        except (ConnectionError, OSError):
            with self.server.lock:
                item['abandoned'] = True
        finally:
            with self.server.lock:
                item['done'] = True
                item['ended'] = time.monotonic()


def layout(path):
    lines = shared_text(path).splitlines()
    assert lines[0] == 'MPHTTP 2', lines[0]
    tag, total, url, generation, count = shlex.split(lines[1])
    extents = [tuple(map(int, x.split())) for x in lines[2:]]
    assert int(count) == len(extents) <= 256
    assert tag == ETAG and int(total) == TOTAL and int(generation) >= 1
    assert len({x[0] for x in extents}) == len(extents)
    position = 0
    for identity, begin, end in extents:
        assert 0 <= identity < 256 and begin == position and begin < end <= TOTAL
        position = end
    assert position == TOTAL
    return dict(url=url, generation=int(generation), extents=extents)


def shared_text(path):
    if os.name != 'nt':
        return path.read_text(encoding='utf-8')
    # Request delete sharing and close promptly between observed generations.
    import msvcrt
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_ulong, ctypes.c_ulong,
                                  ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p]
    kernel.CreateFileW.restype = ctypes.c_void_p
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    handle = kernel.CreateFileW(str(path), 0x80000000, 7, None, 3, 0x80, None)
    if handle in (None, ctypes.c_void_p(-1).value):
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        descriptor = msvcrt.open_osfhandle(handle, os.O_RDONLY | os.O_BINARY)
    except Exception:
        kernel.CloseHandle(handle)
        raise
    with os.fdopen(descriptor, 'r', encoding='utf-8') as stream:
        text = stream.read(2 * 1024 * 1024 + 1)
        assert len(text) <= 2 * 1024 * 1024
        return text


def wait_until(predicate, seconds=15):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(.005)
    raise AssertionError('Adaptive product fixture exceeded its fixed deadline')


@contextmanager
def owned_processes(processes):
    try:
        yield
    finally:
        for process in processes:
            if process.poll() is None:
                process.kill()
                process.communicate(timeout=10)


def task_line(process):
    lines = queue.Queue()
    reader = threading.Thread(target=lambda: lines.put(process.stdout.readline()), daemon=True)
    reader.start()
    try:
        line = lines.get(timeout=10)
    except queue.Empty:
        process.kill()
        reader.join(timeout=2)
        process.communicate(timeout=10)
        raise AssertionError('Owned CLI did not publish TASK_ID within 10 seconds') from None
    reader.join(timeout=2)
    fields = line.split()
    assert len(fields) == 2 and fields[0] == 'TASK_ID', fields
    return fields[1]


def exercise(executable):
    server = Server()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    processes = []
    try:
        with tempfile.TemporaryDirectory(prefix='maskpilot-adaptive-') as temporary, owned_processes(processes):
            root = Path(temporary)
            base = f'http://127.0.0.1:{server.server_port}'

            def start(route, name, connections):
                state, target = root / (name + '-state'), root / (name + '.bin')
                process = subprocess.Popen([str(executable), base + route, str(target), str(state),
                                            str(connections), DIGEST], stdout=subprocess.PIPE,
                                           stderr=subprocess.PIPE, text=True)
                processes.append(process)
                return process, task_line(process), state, target

            def finish(process, target):
                output, error = process.communicate(timeout=30)
                assert process.returncode == 0, (output, error)
                assert hashlib.sha256(target.read_bytes()).hexdigest() == DIGEST

            def run_resume(task, state, *options):
                return subprocess.run([str(executable), '--resume', task, str(state), *options],
                                      capture_output=True, text=True, timeout=30)

            for count, route in [(2, '/adaptive/basic'), (8, '/eight/basic')]:
                process, task, state, target = start(route, f'basic-{count}', count)
                metadata = state / 'parts' / task / 'identity'

                def split_seen():
                    assert process.poll() is None, process.communicate()
                    try:
                        value = layout(metadata)
                        return value if len(value['extents']) > count else None
                    except (FileNotFoundError, PermissionError, AssertionError, ValueError):
                        return None

                snapshot = wait_until(split_seen)
                finish(process, target)
                requests = [x for x in server.snapshot(route) if not x['probe']]
                slow_begin = TOTAL * (count - 1) // count
                original = next(x for x in requests if x['begin'] == slow_begin and x['end'] == TOTAL - 1)
                fast_finished = min(x['ended'] for x in requests
                                    if x['begin'] < slow_begin and x['ended'] is not None)
                child_starts = {begin for identity, begin, _ in snapshot['extents'] if identity >= count}
                assert any(x['begin'] in child_starts and slow_begin < x['begin'] < TOTAL - 1
                           and x['started'] >= fast_finished for x in requests), requests
                assert original['abandoned'], 'Original active tail was never detached'
                assert max(x['active'] for x in requests) <= count, requests
                print(f'PASS actual tail split: {count} live slots, {len(snapshot["extents"])} stored extents, digest', flush=True)

            process, task, state, target = start('/adaptive/crash', 'crash', 2)
            metadata = state / 'parts' / task / 'identity'

            def committed_split():
                assert process.poll() is None, process.communicate()
                try:
                    value = layout(metadata)
                    if value['generation'] > 1:
                        for identity, begin, end in value['extents']:
                            checkpoint = metadata.parent / f'offset-{identity}'
                            if checkpoint.exists() and 0 < int(shared_text(checkpoint)) < end - begin:
                                return value
                    return None
                except (FileNotFoundError, PermissionError, AssertionError, ValueError):
                    return None

            previous = wait_until(committed_split)
            process.kill()
            process.communicate(timeout=10)
            assert not target.exists()
            assert layout(metadata)['generation'] >= previous['generation']
            with_offset = []
            for identity, begin, end in previous['extents']:
                checkpoint = metadata.parent / f'offset-{identity}'
                if checkpoint.exists():
                    saved = int(checkpoint.read_text())
                    if 0 < saved < end - begin:
                        with_offset.append(begin + saved)
            before = len(server.snapshot('/adaptive/crash'))
            result = run_resume(task, state, '--connections', '1')
            assert result.returncode == 0, (result.stdout, result.stderr)
            assert hashlib.sha256(target.read_bytes()).hexdigest() == DIGEST
            resumed = server.snapshot('/adaptive/crash')[before:]
            assert with_offset and any(x['begin'] in with_offset for x in resumed), resumed
            assert max(x['active'] for x in resumed if not x['probe']) <= 1, resumed
            print('PASS committed split kill/reopen with one live slot and retained durable prefix', flush=True)

            def seeded(name, route, manifest):
                state, target = root / (name + '-state'), root / (name + '.bin')
                result = subprocess.run([str(executable), base + route, str(target), str(state), '2',
                                         DIGEST, '--origin-auth-required'], capture_output=True,
                                        text=True, timeout=10)
                assert result.returncode != 0 and not target.exists()
                task = result.stdout.splitlines()[0].split()[1]
                folder = state / 'parts' / task
                folder.mkdir(parents=True)
                folder.joinpath('identity').write_text(manifest(base + route))
                for identity, begin, saved in [(0, 0, 123456), (1, TOTAL // 2, 234567)]:
                    folder.joinpath(f'part-{identity}').write_bytes(DATA[begin:begin + saved])
                    folder.joinpath(f'offset-{identity}').write_text(str(saved))
                return task, state, target, folder

            def legacy(url):
                return f'{json.dumps(ETAG)} {TOTAL} 2 {json.dumps(url)}'

            for name, count in [('legacy', 1), ('legacy-orphan', 2)]:
                route = '/adaptive/' + name
                task, state, target, folder = seeded(name, route, legacy)
                if count == 2:
                    folder.joinpath('part-2').write_bytes(b'stale unreferenced child bytes')
                    folder.joinpath('offset-2').write_text('12')
                    folder.joinpath('offset-2.new').write_text('uncommitted stale checkpoint')
                result = run_resume(task, state, '--allow-anonymous', '--connections', str(count))
                assert result.returncode == 0, (result.stdout, result.stderr)
                assert hashlib.sha256(target.read_bytes()).hexdigest() == DIGEST
                requests = [x for x in server.snapshot(route) if not x['probe']]
                assert {123456, TOTAL // 2 + 234567} <= {x['begin'] for x in requests}, requests
                assert max(x['active'] for x in requests) <= count, requests
                if count == 2:
                    assert len(requests) > 2, 'Stale child allocation was not exercised'
                print(f'PASS {name}: durable offsets, {count} live slot(s), complete digest', flush=True)

            task, state, target, folder = seeded('corrupt', '/adaptive/corrupt', lambda _: 'MPHTTP 99\n')
            folder.joinpath('identity.new').write_text('MPHTTP 2\nnot authoritative')
            folder.joinpath('part-255').write_bytes(b'wrong resource')
            folder.joinpath('offset-255').write_text('14')
            folder.joinpath('notes.txt').write_bytes(b'preserve unrelated cache entry')
            result = run_resume(task, state, '--allow-anonymous', '--connections', '1')
            assert result.returncode == 0, (result.stdout, result.stderr)
            assert hashlib.sha256(target.read_bytes()).hexdigest() == DIGEST
            assert folder.joinpath('notes.txt').read_bytes() == b'preserve unrelated cache entry'
            assert not folder.joinpath('part-255').exists()
            print('PASS unknown layout and uncommitted metadata restart safely; high IDs cleaned, unrelated file preserved', flush=True)

            if os.name == 'nt':
                def modern(url):
                    return (f'MPHTTP 2\n{json.dumps(ETAG)} {TOTAL} {json.dumps(url)} 1 2\n'
                            f'0 0 {TOTAL // 2}\n1 {TOTAL // 2} {TOTAL}\n')
                task, state, target, folder = seeded('locked', '/adaptive/locked', modern)
                kernel = ctypes.WinDLL('kernel32', use_last_error=True)
                kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_ulong, ctypes.c_ulong,
                                              ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p]
                kernel.CreateFileW.restype = ctypes.c_void_p
                kernel.CloseHandle.argtypes = [ctypes.c_void_p]
                handle = kernel.CreateFileW(str(folder / 'identity'), 0x80000000, 3, None, 3, 0, None)
                assert handle not in (None, ctypes.c_void_p(-1).value), ctypes.get_last_error()
                old = folder.joinpath('identity').read_bytes()
                try:
                    result = run_resume(task, state, '--allow-anonymous')
                    assert result.returncode != 0 and not target.exists(), (result.stdout, result.stderr)
                    assert folder.joinpath('identity').read_bytes() == old
                    assert folder.joinpath('part-1').exists()
                finally:
                    assert kernel.CloseHandle(handle)
                result = run_resume(task, state, '--connections', '1')
                assert result.returncode == 0, (result.stdout, result.stderr)
                assert hashlib.sha256(target.read_bytes()).hexdigest() == DIGEST
                print('PASS actual Windows manifest replacement failure preserves authoritative layout and recoverable bytes', flush=True)
    finally:
        for process in processes:
            if process.poll() is None:
                process.kill()
                process.communicate(timeout=10)
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--exercise', type=Path, required=True)
    exercise(parser.parse_args().exercise.resolve())
