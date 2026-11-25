"""Real native HTTP MIME observations without changing ordinary byte-transfer outcomes."""
import argparse
from contextlib import closing
import hashlib
import http.server
from pathlib import Path
import re
import sqlite3
import subprocess
import tempfile
import threading
import time

HTML = 'text/html; charset=UTF-8'
CSS = 'text/css; charset=UTF-8'
DATA = b'<html><head><title>Metadata fixture</title></head><body>extensionless</body></html>'
LARGE = b''.join(hashlib.sha256(str(n).encode()).digest() * 128 for n in range(1024))
ETAG = '"metadata-v1"'


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 128  # Accept the eight-connection burst before handlers are scheduled.

    def __init__(self):
        self.lock = threading.Lock()
        self.requests = []
        self.donor_sent = threading.Event()
        super().__init__(('127.0.0.1', 0), Handler)
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)
        self.thread.start()

    def url(self, path):
        return f'http://127.0.0.1:{self.server_port}{path}'

    def hits(self, path):
        with self.lock:
            return [dict(value) for value in self.requests if value['path'] == path]

    def close(self):
        self.shutdown()
        self.server_close()
        self.thread.join(timeout=5)


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def do_GET(self):
        path = self.path
        range_value = self.headers.get('Range', '')
        probe = range_value == 'bytes=0-0'
        with self.server.lock:
            self.server.requests.append(dict(path=path, range=range_value))
        if path == '/redirect':
            self.send_response(301)
            self.send_header('Location', '/extensionless')
            self.send_header('Content-Type', 'text/plain')
            self.send_header('Content-Length', '0')
            self.send_header('Connection', 'close')
            self.end_headers()
            self.close_connection = True
            return
        if path == '/zero416':
            self.send_response(416)
            self.send_header('Content-Range', 'bytes */0')
            self.send_header('Content-Type', HTML)
            self.send_header('Content-Length', '0')
            self.send_header('ETag', ETAG)
            self.send_header('Connection', 'close')
            self.end_headers()
            self.close_connection = True
            return
        ranged = path in ('/ranges', '/split', '/full-cache')
        data = LARGE if path in ('/ranges', '/split') else DATA
        if path in ('/zero200', '/unknown-empty'):
            data = b''
        match = re.fullmatch(r'bytes=(\d+)-(\d+)', range_value) if ranged else None
        begin, end = (int(match[1]), int(match[2])) if match else (0, len(data) - 1)
        body = data[begin:end + 1]
        self.send_response(206 if match else 200)
        mime = CSS if path in ('/stylesheet', '/unknown-empty') else HTML
        if path == '/mismatch' and not probe:
            mime = CSS
        if path == '/split' and begin == len(LARGE) // 2:
            mime = CSS
        if path == '/unsafe-tab':
            mime = 'text/html\t; charset=UTF-8'
        if path == '/unsafe-ascii':
            mime = 'text/html; note=\xe9'
        if path == '/oversized':
            mime = 'x' * 1025
        absent = path == '/missing-all' or (path == '/missing-body' and not probe) or (path == '/missing-probe' and probe) or (path == '/unknown-empty' and probe)
        if not absent:
            self.send_header('Content-Type', mime)
        if path in ('/duplicate-same', '/duplicate-conflict'):
            self.send_header('Content-Type', mime if path == '/duplicate-same' else CSS)
        if ranged:
            self.send_header('ETag', ETAG)
            if match:
                self.send_header('Content-Range', f'bytes {begin}-{end}/{len(data)}')
        chunked = path == '/trailer-conflict'
        if chunked:
            self.send_header('Transfer-Encoding', 'chunked')
            self.send_header('Trailer', 'Content-Type')
        elif path != '/unknown-empty':
            self.send_header('Content-Length', str(len(body)))
        self.send_header('Connection', 'close')
        self.end_headers()
        self.close_connection = True
        try:
            if path == '/split' and not probe and begin < len(LARGE) // 2:
                assert self.server.donor_sent.wait(5), 'Owned donor did not start'
                time.sleep(.15)
            for offset in range(0, len(body), 8192):
                block = body[offset:offset + 8192]
                self.wfile.write((f'{len(block):x}\r\n'.encode() + block + b'\r\n') if chunked else block)
                self.wfile.flush()
                if path == '/split' and not probe and begin >= len(LARGE) // 2:
                    self.server.donor_sent.set()
                    time.sleep(.008)
            if chunked:
                self.wfile.write(b'0\r\nContent-Type: text/css; charset=UTF-8\r\n\r\n')
        except (ConnectionError, OSError):
            pass


def task(state):
    with closing(sqlite3.connect(state / 'tasks.db')) as database:
        database.row_factory = sqlite3.Row
        rows = database.execute('SELECT * FROM tasks').fetchall()
        assert len(rows) == 1
        return dict(rows[0])


def exercise(executable):
    server = Server()
    try:
        with tempfile.TemporaryDirectory(prefix='maskpilot-response-metadata-') as temporary:
            root = Path(temporary)

            def run(name, path, expected, data=DATA, connections=1):
                state, output = root / (name + '-state'), root / (name + '.bin')
                result = subprocess.run([str(executable), server.url(path), str(output), str(state),
                                         '--connections', str(connections)], capture_output=True,
                                        text=True, timeout=35)
                assert result.returncode == 0, (
                    path, result.stdout, result.stderr,
                    {'serverAlive': server.thread.is_alive(), 'requests': server.hits(path)})
                assert output.read_bytes() == data, path
                observed = task(state)
                assert observed['state'] == 9
                assert observed['response_content_type'] == expected, (path, observed)
                assert observed['response_url'] == server.url('/extensionless' if path == '/redirect' else path)
                return observed

            run('html', '/extensionless', HTML)
            run('css', '/stylesheet', CSS)
            run('redirect', '/redirect', HTML)
            print('PASS extensionless HTML/CSS charset and final redirected URL survive real CLI/SQLite', flush=True)

            for path, expected in [('/mismatch', ''), ('/missing-body', ''), ('/missing-probe', HTML),
                                   ('/missing-all', ''), ('/duplicate-same', HTML), ('/duplicate-conflict', ''),
                                   ('/unsafe-tab', ''), ('/unsafe-ascii', ''), ('/oversized', ''),
                                   ('/trailer-conflict', '')]:
                run(path[1:], path, expected)
            print('PASS unsafe/duplicate/missing/mismatched/trailer MIME becomes unknown without failing bytes', flush=True)

            run('zero200', '/zero200', HTML, b'')
            run('zero416', '/zero416', '', b'')
            run('unknown-empty', '/unknown-empty', CSS, b'')
            print('PASS zero200 probe fallback, zero416 error MIME exclusion and actual unknown-empty DONE headers', flush=True)

            run('ranges', '/ranges', HTML, LARGE, 8)
            body_ranges = [entry['range'] for entry in server.hits('/ranges') if entry['range'] != 'bytes=0-0']
            assert len(set(body_ranges)) >= 8
            run('split', '/split', '', LARGE, 2)
            split_ranges = [entry['range'] for entry in server.hits('/split') if entry['range'] != 'bytes=0-0']
            assert len(set(split_ranges)) > 2, split_ranges
            assert any(int(value.split('=')[1].split('-')[0]) > len(LARGE) // 2 for value in split_ranges)
            print('PASS eight response headers agree and detached/reopened split cannot erase MIME ambiguity', flush=True)

            state, output = root / 'full-cache-state', root / 'full-cache.bin'
            result = subprocess.run([str(executable), server.url('/full-cache'), str(output), str(state),
                                     '--origin-auth-required'], capture_output=True, text=True, timeout=15)
            assert result.returncode == 1 and not server.hits('/full-cache')
            identity = task(state)['id']
            folder = state / 'parts' / identity
            folder.mkdir(parents=True, exist_ok=True)
            # Product recovery encoding is intentionally seeded; MIME is still obtained only from live HTTP.
            import json
            manifest = f'MPHTTP 2\n{json.dumps(ETAG)} {len(DATA)} {json.dumps(server.url("/full-cache"))} 1 1\n0 0 {len(DATA)}\n'
            (folder / 'identity').write_text(manifest, encoding='ascii')
            (folder / 'part-0').write_bytes(DATA)
            (folder / 'offset-0').write_text(str(len(DATA)), encoding='ascii')
            result = subprocess.run([str(executable), '--resume', identity, str(state), '--allow-anonymous'],
                                    capture_output=True, text=True, timeout=20)
            assert result.returncode == 0, (result.stdout, result.stderr)
            assert output.read_bytes() == DATA
            observed = task(state)
            assert observed['response_content_type'] == HTML and observed['response_url'] == server.url('/full-cache')
            assert server.hits('/full-cache') == [dict(path='/full-cache', range='bytes=0-0')]
            print('PASS fully recovered payload with no new Sink uses only fresh successful probe MIME', flush=True)
    finally:
        server.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--exercise', type=Path, required=True)
    arguments = parser.parse_args()
    exercise(arguments.exercise.resolve())
