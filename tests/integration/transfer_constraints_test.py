"""Real CLI HTTP stored-body bounds, redirect policy and final-response metadata."""
import argparse
from contextlib import ExitStack, closing
import hashlib
import http.server
import ipaddress
import os
from pathlib import Path
import re
import socketserver
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'support'))
from proxy_fixture import RecordingProxy

SMALL = b''.join(hashlib.sha256(str(n).encode()).digest() for n in range(2048))
LARGE = b''.join(hashlib.sha256(n.to_bytes(4, 'little')).digest() * 128 for n in range(1024))


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True

    def __init__(self):
        self.lock = threading.Lock()
        self.requests = []
        self.accepted = 0
        self.foreign = ''
        super().__init__(('127.0.0.1', 0), Handler)
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)
        self.thread.start()

    def get_request(self):
        result = super().get_request()
        with self.lock:
            self.accepted += 1
        return result

    def hits(self, route):
        with self.lock:
            return [dict(item) for item in self.requests if item['route'] == route]

    def url(self, path):
        return f'http://127.0.0.1:{self.server_port}{path}'

    def close(self):
        self.shutdown()
        self.server_close()
        self.thread.join(timeout=5)


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def do_GET(self):
        route = urllib.parse.urlsplit(self.path).path
        value = self.headers.get('Range', '')
        with self.server.lock:
            self.server.requests.append(dict(route=route, range=value))
        locations = {
            '/docs': '/docs/index.html',
            '/redirect-foreign': self.server.foreign + '/foreign',
            '/redirect-host': f'http://localhost:{self.server.server_port}/foreign-host',
            '/redirect-scheme': f'https://127.0.0.1:{self.server.server_port}/foreign-scheme',
            '/redirect-default': 'http://CONSTRAINT.invalid:80/final',
            '/redirect-ipv6': 'http://[::1]:80/final',
        }
        if route in locations:
            self.send_response(301)
            self.send_header('Location', locations[route])
            self.send_header('Content-Length', '0')
            self.send_header('Connection', 'close')
            self.end_headers()
            self.close_connection = True
            return
        ranged = route in ('/range', '/slow', '/lying-range')
        data = LARGE if route in ('/range', '/slow') else SMALL
        match = re.fullmatch(r'bytes=(\d+)-(\d+)', value) if ranged else None
        begin, end = (int(match[1]), int(match[2])) if match else (0, len(data) - 1)
        if begin > end or end >= len(data):
            self.send_error(416)
            return
        body = data[begin:end + 1]
        if route == '/lying-range' and value != 'bytes=0-0':
            body += b'exceeds-declared-range'
        self.send_response(206 if match else 200)
        self.send_header('Content-Type', 'application/octet-stream')
        if ranged:
            self.send_header('ETag', '"constraints-v1"')
            if match:
                self.send_header('Content-Range', f'bytes {begin}-{end}/{len(data)}')
        chunked = route == '/chunked'
        unknown = route == '/unknown'
        if chunked:
            self.send_header('Transfer-Encoding', 'chunked')
        elif not unknown:
            self.send_header('Content-Length', str(len(body)))
        self.send_header('Connection', 'close')
        self.end_headers()
        self.close_connection = True
        try:
            for offset in range(0, len(body), 8192):
                block = body[offset:offset + 8192]
                if chunked:
                    self.wfile.write(f'{len(block):x}\r\n'.encode() + block + b'\r\n')
                else:
                    self.wfile.write(block)
                self.wfile.flush()
                if route == '/slow' and value != 'bytes=0-0':
                    time.sleep(.008)
            if chunked:
                self.wfile.write(b'0\r\n\r\n')
        except (ConnectionError, OSError):
            pass


class VirtualOriginProxy(RecordingProxy):
    """Controlled port80/IPv6 URL routing without binding privileged or user-owned ports."""
    def __init__(self, kind, origin):
        self.origin_port = origin.server_port
        super().__init__(kind=kind, mappings={'constraint.invalid': '127.0.0.1'})

    def connect_target(self, host, port, command):
        try:
            virtual_loopback = ipaddress.IPv6Address(host.strip('[]')) == ipaddress.IPv6Address('::1')
        except ValueError:
            virtual_loopback = False
        if port == 80 and (host.lower() == 'constraint.invalid' or virtual_loopback):
            return super().connect_target('127.0.0.1', self.origin_port, command)
        return super().connect_target(host, port, command)


def row(state):
    with closing(sqlite3.connect(state / 'tasks.db')) as database:
        database.row_factory = sqlite3.Row
        rows = database.execute('SELECT * FROM tasks').fetchall()
        assert len(rows) == 1
        return dict(rows[0])


def completed(result, target, data, state, context=None):
    assert result.returncode == 0, (context, result.stdout, result.stderr)
    assert target.read_bytes() == data
    saved = row(state)
    assert saved['state'] == 9 and saved['response_url']
    return saved


def rejected(result, target, state):
    assert result.returncode == 1, (result.stdout, result.stderr)
    assert not target.exists()
    assert row(state)['state'] == 13, (result.stdout, result.stderr, row(state))


def wait_until(predicate, timeout=12):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        if predicate():
            return
        time.sleep(.025)
    raise AssertionError('Timed out waiting for durable owned transfer evidence')


def exercise(executable):
    origin, foreign = Server(), Server()
    origin.foreign = foreign.url('')
    processes = []
    def stop(process):
        if process.poll() is None:
            process.kill()
        process.communicate(timeout=10)

    try:
        with tempfile.TemporaryDirectory(prefix='maskpilot-http-constraints-') as temporary, ExitStack() as stack:
            root = Path(temporary)

            def run(name, route, cap=0, *options):
                state, target = root / (name + '-state'), root / (name + '.bin')
                url = route if '://' in route else origin.url(route)
                result = subprocess.run([str(executable), url, str(target), str(state),
                                         '--max-stored-bytes', str(cap), *options],
                                        capture_output=True, text=True, timeout=35)
                return result, state, target

            result, state, target = run('exact', '/known', len(SMALL))
            saved = completed(result, target, SMALL, state)
            assert saved['max_stored_bytes'] == len(SMALL)
            assert saved['response_url'] == origin.url('/known')
            before = len(origin.hits('/known'))
            result, state, target = run('known-over', '/known', len(SMALL) - 1)
            rejected(result, target, state)
            assert len(origin.hits('/known')) == before + 1, 'Known oversize launched a body request'
            assert not (state / 'parts' / row(state)['id']).exists(), 'Known oversize created recovery work'
            print('PASS exact known size and declared oversize rejection before body/cache', flush=True)

            for route in ('/chunked', '/unknown'):
                result, state, target = run(route[1:] + '-exact', route, len(SMALL))
                completed(result, target, SMALL, state)
                result, state, target = run(route[1:] + '-over', route, len(SMALL) - 1)
                rejected(result, target, state)
                parts = list((state / 'parts').rglob('part-*'))
                assert all(path.stat().st_size <= len(SMALL) - 1 for path in parts)
                result, state, target = run(route[1:] + '-off', route)
                completed(result, target, SMALL, state)
            print('PASS chunked/close-delimited exact limits, oversize safety and disabled defaults', flush=True)

            result, state, target = run('parallel', '/range', len(LARGE), '--connections', '8')
            completed(result, target, LARGE, state)
            body_ranges = [item['range'] for item in origin.hits('/range') if item['range'] != 'bytes=0-0']
            assert len(set(body_ranges)) >= 8, body_ranges
            result, state, target = run('lying', '/lying-range', len(SMALL), '--connections', '8')
            rejected(result, target, state)
            print('PASS parallel exact coverage and lying range body rejected without publication', flush=True)

            state, target = root / 'resume-state', root / 'resume.bin'
            process = subprocess.Popen([str(executable), origin.url('/slow'), str(target), str(state),
                                        '--max-stored-bytes', str(len(LARGE)), '--connections', '1'],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            processes.append(process)
            stack.callback(stop, process)
            def checkpoint():
                if process.poll() is not None:
                    raise AssertionError(('Transfer exited before crash checkpoint', process.communicate()))
                for path in (state / 'parts').glob('*/offset-*'):
                    try:
                        value = int(path.read_text())
                        if 0 < value < len(LARGE):
                            return True
                    except (ValueError, OSError):
                        pass
                return False
            wait_until(checkpoint)
            process.kill()
            process.communicate(timeout=10)
            task = row(state)['id']
            folder = state / 'parts' / task
            previous = {path.name: path.read_bytes() for path in folder.iterdir() if path.is_file()}
            before = len(origin.hits('/slow'))
            result = subprocess.run([str(executable), '--resume', task, str(state),
                                     '--max-stored-bytes', str(len(LARGE) - 1)], capture_output=True,
                                    text=True, timeout=20)
            rejected(result, target, state)
            assert previous == {path.name: path.read_bytes() for path in folder.iterdir() if path.is_file()}
            assert len(origin.hits('/slow')) == before + 1
            before = len(origin.hits('/slow'))
            result = subprocess.run([str(executable), '--resume', task, str(state),
                                     '--max-stored-bytes', str(len(LARGE)), '--connections', '1'],
                                    capture_output=True, text=True, timeout=35)
            completed(result, target, LARGE, state)
            assert any(int(item['range'].split('=')[1].split('-')[0]) > 0
                       for item in origin.hits('/slow')[before:] if item['range'])
            print('PASS crash recovery, stricter cap preserves cached bytes, restoring cap resumes actual tail', flush=True)

            state, target = root / 'collision-state', root / 'collision.bin'
            process = subprocess.Popen([str(executable), origin.url('/slow'), str(target), str(state),
                                        '--max-stored-bytes', str(len(LARGE)), '--connections', '1'],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            processes.append(process)
            stack.callback(stop, process)
            wait_until(lambda: (state / 'tasks.db').exists() and bool(list((state / 'parts').glob('*/part-*'))))
            target.write_bytes(b'preserve existing final')
            output, error = process.communicate(timeout=35)
            assert process.returncode == 1, (output, error)
            assert target.read_bytes() == b'preserve existing final'
            assert row(state)['state'] == 13
            print('PASS late final-path collision preserves the existing file', flush=True)

            result, state, target = run('relative', '/docs', len(SMALL), '--same-origin-redirects')
            saved = completed(result, target, SMALL, state)
            assert saved['url'] == origin.url('/docs')
            assert saved['response_url'] == origin.url('/docs/index.html')
            for route in ('/redirect-foreign', '/redirect-host', '/redirect-scheme'):
                before_origin, before_foreign = origin.accepted, foreign.accepted
                result, state, target = run(route[1:], route, len(SMALL), '--same-origin-redirects')
                rejected(result, target, state)
                assert foreign.accepted == before_foreign, 'Foreign origin received a connection'
                assert origin.accepted == before_origin + 1, 'Rejected target opened another connection'
            result, state, target = run('unrestricted', '/redirect-foreign', len(SMALL))
            completed(result, target, SMALL, state)
            print('PASS relative redirects persist final URL; foreign host/port/scheme receive no contact', flush=True)

            for kind in ('http', 'socks5'):
                proxy = VirtualOriginProxy(kind, origin)
                stack.callback(proxy.close)
                result, state, target = run(kind + '-default', 'http://constraint.invalid/redirect-default',
                                            len(SMALL), '--same-origin-redirects', '--proxy', proxy.url())
                saved = completed(result, target, SMALL, state, (kind, 'default', proxy.snapshot()))
                final = urllib.parse.urlsplit(saved['response_url'])
                assert final.hostname == 'constraint.invalid' and final.port == 80 and final.path == '/final'
                result, state, target = run(kind + '-ipv6', 'http://[0:0:0:0:0:0:0:1]/redirect-ipv6',
                                            len(SMALL), '--same-origin-redirects', '--proxy', proxy.url())
                completed(result, target, SMALL, state, (kind, 'ipv6', proxy.snapshot()))
                assert proxy.snapshot()['destinations']
                print(f'PASS {kind} virtual default-port and expanded IPv6 routing', flush=True)
            print('PASS actual HTTP/SOCKS routing with default-port/host-case/IPv6 canonical origin equality', flush=True)
    finally:
        for process in processes:
            if process.poll() is None:
                process.kill()
                process.communicate(timeout=10)
        origin.close()
        foreign.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('executable', type=Path, nargs='?')
    parser.add_argument('--exercise', type=Path, dest='exercise_path')
    arguments = parser.parse_args()
    selected = arguments.exercise_path or arguments.executable
    if selected is None:
        parser.error('Provide --exercise CLI_PATH or a positional CLI_PATH')
    exercise(selected.resolve())
