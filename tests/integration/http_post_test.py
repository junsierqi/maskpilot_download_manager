"""Owned HTTP POST product acceptance. Never contacts external services."""
import argparse
import contextlib
import http.server
import json
import os
from pathlib import Path
import re
import socket
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'support'))
from proxy_fixture import RecordingProxy

BODY = 'owned-post-secret-947=\u4e2d\u6587&format=json'
PAYLOAD = b'owned-result\x00\xff' * 8192


class Origin(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self):
        self.requests = []
        self.foreign = None
        super().__init__(('127.0.0.1', 0), Handler)
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)
        self.thread.start()

    def url(self, path):
        return f'http://127.0.0.1:{self.server_port}/{path}'

    def close(self):
        self.shutdown()
        self.server_close()
        self.thread.join(5)


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def record(self, body=b''):
        self.server.requests.append((self.command, self.path, body, dict(self.headers)))

    def response(self, status, body=b'', location=None):
        self.send_response(status)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Content-Type', 'application/octet-stream')
        if location:
            self.send_header('Location', location)
        self.end_headers()
        with contextlib.suppress(OSError):
            self.wfile.write(body)

    def do_HEAD(self):
        self.record()
        self.response(405)

    def do_GET(self):
        self.record()
        if self.path != '/result':
            self.response(405)
        else:
            self.response(200, PAYLOAD)

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get('Content-Length', '0')))
        self.record(body)
        path = self.path[1:]
        if path == 'lost':
            self.connection.shutdown(socket.SHUT_RDWR)
            self.connection.close()
        elif path == 'slow':
            self.send_response(200)
            self.send_header('Content-Length', str(len(PAYLOAD)))
            self.end_headers()
            for offset in range(0, len(PAYLOAD), 1024):
                try:
                    self.wfile.write(PAYLOAD[offset:offset + 1024])
                    self.wfile.flush()
                except OSError:
                    break
                time.sleep(.04)
        elif path == 'malformed':
            self.connection.sendall(b'HTTP/1.1 303 See Other\r\nLocation: /result\r\nContent-Length: 0\r\n')
            self.close_connection = True
        elif path == 'conflicting':
            self.connection.sendall(b'HTTP/1.1 303 See Other\r\nLocation: /result\r\nContent-Length: 0\r\nContent-Length: 1\r\n\r\n')
            self.close_connection = True
        elif path == 'foreign':
            self.response(303, location=self.server.foreign.url('result'))
        elif path in ('301', '302', '303', '307', '308'):
            self.response(int(path), location='/result')
        elif path == 'fail':
            self.response(500)
        elif path == 'empty':
            self.response(204)
        else:
            self.response(200, PAYLOAD)


def rows(data, query):
    with contextlib.closing(sqlite3.connect(data / 'tasks.db')) as db:
        return db.execute(query).fetchall()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exercise', required=True, type=Path)
    parser.add_argument("--scanner", type=Path)
    args = parser.parse_args()
    client = args.exercise.resolve()
    origin, foreign = Origin(), Origin()
    origin.foreign = foreign
    try:
        with tempfile.TemporaryDirectory(prefix='maskpilot-post-') as temporary:
            root = Path(temporary)
            env = dict(os.environ, MASKPILOT_POST_BODY=BODY)
            base = ['--post-body-env', 'MASKPILOT_POST_BODY', '--post-content-type', 'application/json; charset=utf-8']

            def invoke(name, path='ok', options=None, expected=True, existing=None, prefix=None, credentials=False, address=None):
                case = root / name
                case.mkdir(exist_ok=True)
                output, data = case / 'result.bin', case / 'data'
                command = ([str(client), address or origin.url(path), str(output), str(data)] if existing is None else
                           [str(client), '--resume', existing, str(data)])
                if credentials:
                    command += ['1', '', '', 'owned-cookie=private', 'Bearer owned-source']
                command[1:1] = prefix or []
                command += base if options is None else options
                before = len(origin.requests)
                result = subprocess.run(command, env=env, capture_output=True, timeout=40)
                assert (result.returncode == 0) == expected, (name, result.returncode, result.stderr[-1500:])
                assert BODY.encode() not in result.stdout + result.stderr
                if expected:
                    assert output.read_bytes() == (b'' if path == 'empty' else PAYLOAD), name
                for file in data.rglob('*'):
                    if file.is_file():
                        assert BODY.encode() not in file.read_bytes(), (name, file)
                identifiers = re.findall(rb'TASK_ID ([a-f0-9]{32})', result.stdout)
                return (identifiers[0].decode() if identifiers else None), origin.requests[before:], data

            for name, options in [('json', base), ('form', base[:2]), ('empty', ['--confirm-empty-post'])]:
                _, hits, data = invoke(name, options=options)
                assert len(hits) == 1 and hits[0][0] == 'POST'
                assert hits[0][2] == (b'' if name == 'empty' else BODY.encode())
                assert 'Range' not in hits[0][3]
                assert rows(data, 'SELECT http_method,post_content_type FROM tasks')[0][0] == 1
            invoke('no-content', path='empty', options=['--confirm-empty-post'])
            print('PASS actual form/JSON/Unicode/explicit empty/204 single POST, no probe/range, memory-only body', flush=True)

            for status in ('301', '302', '303'):
                _, hits, _ = invoke('redirect-' + status, status, credentials=True)
                assert [h[0] for h in hits] == ['POST', 'GET', 'GET'], hits
                assert all(not h[2] for h in hits[1:])
                assert all(h[3].get('Cookie') == 'owned-cookie=private' and
                           h[3].get('Authorization') == 'Bearer owned-source' for h in hits)
            for status in ('307', '308', 'foreign', 'fail', 'lost', 'malformed', 'conflicting'):
                identifier, hits, data = invoke(status, status, expected=False)
                assert len(hits) == 1 and hits[0][0] == 'POST', (status, hits)
                assert rows(data, 'SELECT retries FROM tasks') == [(0,)]
                _, again, _ = invoke(status, options=[], expected=False, existing=identifier)
                assert not again, status
            assert not foreign.requests
            print('PASS same-origin GET redirects; replay/cross-origin/500/connection loss refused; restart has no body authority', flush=True)

            for kind in ('http', 'socks5'):
                proxy = RecordingProxy(kind, 'owned-route-user', 'owned-route-password',
                                       mappings={'post.invalid': '127.0.0.1'})
                try:
                    route = ['--proxy', proxy.url(), '--proxy-user', 'owned-route-user',
                             '--proxy-password', 'owned-route-password']
                    address = origin.url('proxy').replace('127.0.0.1', 'post.invalid')
                    _, hits, _ = invoke('proxy-' + kind, address=address, options=base + route)
                    assert len(hits) == 1 and hits[0][0] == 'POST' and hits[0][2] == BODY.encode()
                    assert not hits[0][3].get('Proxy-Authorization')
                    report = proxy.snapshot()
                    assert report['auth_accepted'] > 0 and report['destinations']
                    assert all(item['host'] == 'post.invalid' and item['port'] == origin.server_port
                               for item in report['destinations'])
                    route[-1] = 'wrong'
                    _, hits, _ = invoke('proxy-wrong-' + kind, address=address,
                                        options=base + route, expected=False)
                    assert not hits
                finally:
                    proxy.close()
            print('PASS POST source credentials and HTTP/SOCKS5 authenticated proxy DNS; wrong route never falls back', flush=True)

            _, hits, _ = invoke('bound', options=base + ['--max-stored-bytes', '10'], expected=False)
            assert len(hits) == 1
            _, hits, _ = invoke('hash', options=base + ['--sha256', '0' * 64], expected=False)
            assert len(hits) == 1 and not (root / 'hash' / 'result.bin').exists()
            for name in ('existing', 'parent-file'):
                case = root / name
                case.mkdir()
                if name == 'existing':
                    (case / 'result.bin').write_bytes(b'preserve')
                else:
                    (case / 'blocked').write_bytes(b'preserve')
                output = case / 'result.bin' if name == 'existing' else case / 'blocked' / 'result.bin'
                before = len(origin.requests)
                result = subprocess.run([str(client), origin.url('ok'), str(output), str(case / 'data'), *base],
                                        env=env, capture_output=True, timeout=30)
                assert result.returncode != 0 and len(origin.requests) == before
                assert (case / ('result.bin' if name == 'existing' else 'blocked')).read_bytes() == b'preserve'
            print('PASS body size/integrity and destination preflight preserve existing files', flush=True)

            identifier, hits, data = invoke('stage-unavailable', options=['--post'], expected=False)
            assert not hits
            stage = root / 'stage-unavailable' / ('result.bin.maskpilot-' + identifier + '.tmp')
            stage.mkdir()
            _, hits, _ = invoke('stage-unavailable', existing=identifier, expected=False)
            assert not hits
            print('PASS destination staging failure refuses POST before source contact', flush=True)

            if args.scanner:
                for mode in ('pass', 'reject'):
                    name = 'scan-' + mode
                    case = root / name
                    case.mkdir()
                    record = case / 'scan.json'
                    prefix = ['--scan-program', str(args.scanner.resolve())]
                    for value in ('--mode', mode, '--record', str(record), '--final',
                                  str(case / 'result.bin'), '{file}'):
                        prefix += ['--scan-arg', value]
                    _, hits, data = invoke(name, prefix=prefix, expected=mode == 'pass')
                    assert len(hits) == 1
                    scan = json.loads(record.read_text())
                    assert not scan['finalExists']
                    assert (case / 'result.bin').exists() == (mode == 'pass')
                    assert rows(data, 'SELECT scan_state FROM tasks')[0][0] == (3 if mode == 'pass' else 4)
                print('PASS actual owned scanner gates POST publication on pass/reject', flush=True)

            for name, options in [('missing', ['--post']), ('bad-mime', base + ['--post-content-type', 'text/plain\r\nX: bad'])]:
                _, hits, _ = invoke(name, options=options, expected=False)
                assert not hits
            case = root / 'interrupted'
            case.mkdir()
            data = case / 'data'
            before = len(origin.requests)
            process = subprocess.Popen([str(client), origin.url('slow'), str(case / 'result.bin'), str(data), *base],
                                       env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                deadline = time.monotonic() + 15
                while len(origin.requests) == before and time.monotonic() < deadline:
                    time.sleep(.05)
                assert len(origin.requests) == before + 1
            finally:
                process.kill()
                process.communicate(timeout=10)
            identifier = rows(data, 'SELECT id FROM tasks')[0][0]
            _, hits, _ = invoke('interrupted', options=[], expected=False, existing=identifier)
            assert not hits
            # Explicit reauthorization is a new submission; replace the slow endpoint only in the owned task.
            with contextlib.closing(sqlite3.connect(data / 'tasks.db')) as db:
                db.execute('UPDATE tasks SET url=?', (origin.url('ok'),))
                db.commit()
            _, hits, _ = invoke('interrupted', existing=identifier)
            assert len(hits) == 1 and hits[0][0] == 'POST'
            print('PASS missing/invalid material never contacts source; killed transfer requires explicit new submission', flush=True)
    finally:
        origin.close()
        foreign.close()


if __name__ == '__main__':
    main()
