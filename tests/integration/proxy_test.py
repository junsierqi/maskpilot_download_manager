"""Real native CLI proxy authentication, routing, recovery and secret-lifecycle checks."""
import argparse
import contextlib
import http.server
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
import urllib.parse

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'support'))
from ftp_fixture import Server as FtpServer, certificates, cli_child_environment
from proxy_fixture import RecordingProxy

USER = 'proxy-user-private-8fa310'
PASSWORD = 'proxy-password-private-79d42e'
WRONG_PASSWORD = 'wrong-proxy-password-private-e730'
USER_ENV = 'MASKPILOT_PROXY_FIXTURE_USER'
PASSWORD_ENV = 'MASKPILOT_PROXY_FIXTURE_PASSWORD'
DATA = bytes(range(256)) * 12289


class Origin(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self):
        self.hits = []
        self.lock = threading.Lock()
        super().__init__(('127.0.0.1', 0), OriginHandler)
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)
        self.thread.start()

    def url(self, path='/file', host='127.0.0.1'):
        return f'http://{host}:{self.server_port}{path}'

    def close(self):
        self.shutdown()
        self.server_close()
        self.thread.join(timeout=5)


class OriginHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def do_GET(self):
        with self.server.lock:
            self.server.hits.append((self.path, bool(self.headers.get('Proxy-Authorization'))))
        path = urllib.parse.urlsplit(self.path).path
        if path == '/redirect':
            self.send_response(302)
            self.send_header('Location', self.server.url('/file', 'redirect.invalid'))
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        match = re.fullmatch(r'bytes=(\d+)-(\d*)', self.headers.get('Range', ''))
        begin, end = (int(match[1]), int(match[2] or len(DATA) - 1)) if match else (0, len(DATA) - 1)
        if not 0 <= begin <= end < len(DATA):
            self.send_response(416)
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        self.send_response(206 if match else 200)
        self.send_header('ETag', '"proxy-fixture-v1"')
        self.send_header('Content-Length', str(end - begin + 1))
        if match:
            self.send_header('Content-Range', f'bytes {begin}-{end}/{len(DATA)}')
        self.end_headers()
        try:
            for offset in range(begin, end + 1, 16384):
                self.wfile.write(DATA[offset:min(offset + 16384, end + 1)])
                if path == '/slow':
                    self.wfile.flush()
                    time.sleep(.02)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass


def assert_no_secrets(root, output=''):
    for secret in (USER, PASSWORD, WRONG_PASSWORD):
        assert secret not in output, 'Proxy secret leaked in native diagnostics'
    for path in root.rglob('*'):
        if path.is_file():
            content = path.read_bytes()
            assert all(secret.encode() not in content for secret in (USER, PASSWORD, WRONG_PASSWORD)), path


def exercise(cli):
    cli = cli.resolve(strict=True)
    flags = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
    base_environment = cli_child_environment()
    # Explicit native routes must override both ambient proxies and ambient bypass rules.
    base_environment.update(NO_PROXY='*', no_proxy='*', http_proxy='http://127.0.0.1:1',
                            HTTP_PROXY='http://127.0.0.1:1', ALL_PROXY='socks5://127.0.0.1:1',
                            all_proxy='socks5://127.0.0.1:1')
    with tempfile.TemporaryDirectory(prefix='maskpilot-proxy-') as temporary, contextlib.ExitStack() as stack:
        root = Path(temporary)
        origin = Origin()
        stack.callback(origin.close)
        ftp = FtpServer()
        stack.callback(ftp.close)
        sequence = 0

        def proxy(kind, authenticated=True):
            nonlocal sequence
            sequence += 1
            server = RecordingProxy(kind, USER if authenticated else '', PASSWORD if authenticated else '',
                                    root / f'proxy-state-{sequence}.json',
                                    {'origin.invalid': '127.0.0.1', 'redirect.invalid': '127.0.0.1'})
            stack.callback(server.close)
            return server

        def invoke(url, route=None, auth='correct', expected=True, extra=(), environment=None):
            nonlocal sequence
            sequence += 1
            target, state = root / f'output-{sequence}.bin', root / f'data-{sequence}'
            arguments = [str(cli), url, str(target), str(state), '--connections', '2']
            child_environment = dict(base_environment if environment is None else environment)
            if route:
                arguments += ['--proxy', route.url()]
                if auth in ('correct', 'wrong'):
                    child_environment[USER_ENV] = USER
                    child_environment[PASSWORD_ENV] = PASSWORD if auth == 'correct' else WRONG_PASSWORD
                    arguments += ['--proxy-user-env', USER_ENV, '--proxy-password-env', PASSWORD_ENV]
                elif auth == 'required':
                    arguments += ['--proxy-auth-required']
            result = subprocess.run([*arguments, *extra], capture_output=True, text=True, timeout=40,
                                    creationflags=flags, env=child_environment)
            assert (result.returncode == 0) == expected, (result.returncode, result.stdout, result.stderr)
            if not expected:
                assert not target.exists(), 'Rejected proxy transfer published output'
            assert_no_secrets(state, result.stdout + result.stderr)
            return target, state, result

        for kind in ('http', 'socks5'):
            route = proxy(kind)
            target, _, _ = invoke(origin.url('/file', 'origin.invalid'), route)
            assert target.read_bytes() == DATA
            snapshot = route.snapshot()
            assert snapshot['auth_accepted'] > 0 and snapshot['forwarded_bytes'] > len(DATA)
            assert any(item['host'] == 'origin.invalid' for item in snapshot['destinations'])
            target, _, _ = invoke(origin.url('/redirect', 'origin.invalid'), route)
            assert target.read_bytes() == DATA
            assert any(item['host'] == 'redirect.invalid' for item in route.snapshot()['destinations'])
            assert not any(leaked for _, leaked in origin.hits), 'Proxy credentials reached HTTP origin'
            before = len(origin.hits)
            invoke(origin.url(), route, auth='wrong', expected=False)
            assert len(origin.hits) == before, 'Wrong proxy credentials bypassed proxy'
            invoke(origin.url(), route, auth='missing', expected=False)
            assert len(origin.hits) == before, 'Missing proxy credentials bypassed proxy'
            assert route.snapshot()['auth_rejected'] > 0
            anonymous = proxy(kind, authenticated=False)
            before_proxy, before_origin = anonymous.snapshot(), len(origin.hits)
            invoke(origin.url(), anonymous, auth='required', expected=False)
            assert anonymous.snapshot() == before_proxy and len(origin.hits) == before_origin
            target, _, _ = invoke(ftp.url('file.bin'), route)
            assert target.read_bytes() == ftp.files['file.bin']
            assert len({item['port'] for item in route.snapshot()['destinations']}) >= 3
            before = len(ftp.commands)
            invoke(ftp.url('file.bin'), route, auth='wrong', expected=False)
            assert len(ftp.commands) == before, 'Wrong proxy credentials reached FTP origin'
            invoke(ftp.url('file.bin'), route, extra=['--ftp-active'], expected=False)
            assert len(ftp.commands) == before, 'Rejected active FTP contacted origin'
            print(f'PASS {kind}: authenticated HTTP/FTP, remote DNS, redirects, no bypass, required marker', flush=True)

        # Empty endpoint explicitly means direct, even when every ambient route is unusable.
        poison = proxy('http')
        direct_environment = dict(base_environment)
        for key in ('HTTP_PROXY', 'http_proxy', 'HTTPS_PROXY', 'https_proxy', 'FTP_PROXY', 'ftp_proxy',
                    'ALL_PROXY', 'all_proxy'):
            direct_environment[key] = poison.url()
        direct_environment.update(NO_PROXY='', no_proxy='')
        target, _, _ = invoke(origin.url(), environment=direct_environment)
        assert target.read_bytes() == DATA and poison.snapshot()['auth_attempts'] == 0
        target, _, _ = invoke(ftp.url('file.bin'), environment=direct_environment)
        assert target.read_bytes() == ftp.files['file.bin'] and poison.snapshot()['auth_attempts'] == 0
        print('PASS explicit direct HTTP/FTP ignores ambient proxy configuration', flush=True)

        # Hold an owned port without listening, so the configured proxy cannot connect or fall back.
        with socket.socket() as unavailable:
            unavailable.bind(('127.0.0.1', 0))

            class UnavailableRoute:
                def url(self):
                    return f'http://127.0.0.1:{unavailable.getsockname()[1]}'

            before_http, before_ftp = len(origin.hits), len(ftp.commands)
            invoke(origin.url(), UnavailableRoute(), expected=False)
            invoke(ftp.url('file.bin'), UnavailableRoute(), expected=False)
            assert len(origin.hits) == before_http and len(ftp.commands) == before_ftp
        print('PASS unreachable configured proxy never contacts HTTP/FTP origin', flush=True)

        certificate, key = certificates(root)
        for mode in ('explicit', 'implicit'):
            secure = FtpServer(mode, certificate, key)
            stack.callback(secure.close)
            for kind in ('http', 'socks5'):
                route = proxy(kind)
                extra = ['--ftp-ca', str(certificate)]
                if mode == 'explicit':
                    extra.append('--ftp-explicit-tls')
                target, _, _ = invoke(secure.url('file.bin'), route, extra=extra)
                assert target.read_bytes() == secure.files['file.bin']
                assert secure.transfers[-1][2:] == (True, True)
                assert route.snapshot()['forwarded_bytes'] > len(secure.files['file.bin'])
            print(f'PASS authenticated HTTP/SOCKS5 proxy with FTPS {mode} encrypted control/data', flush=True)

        # Crash a real authenticated transfer after durable bytes, then reopen without memory-only secrets.
        route = proxy('http')
        target, state = root / 'recovery-output.bin', root / 'recovery-data'
        environment = dict(base_environment, **{USER_ENV: USER, PASSWORD_ENV: PASSWORD})
        process = subprocess.Popen([str(cli), origin.url('/slow'), str(target), str(state), '--connections', '1',
                                    '--proxy', route.url(), '--proxy-user-env', USER_ENV,
                                    '--proxy-password-env', PASSWORD_ENV], stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True, creationflags=flags, env=environment)
        try:
            deadline = time.monotonic() + 15
            checkpoint = None
            while time.monotonic() < deadline:
                for candidate in (state / 'parts').glob('*/offset-0'):
                    try:
                        if int(candidate.read_text()) > 0:
                            checkpoint = candidate
                            break
                    except (OSError, ValueError):
                        pass
                if checkpoint:
                    break
                assert process.poll() is None, 'Slow proxy transfer ended before a durable checkpoint'
                time.sleep(.02)
            assert checkpoint, 'Authenticated proxy transfer did not persist partial progress'
            process.kill()
            output, error = process.communicate(timeout=10)
            assert_no_secrets(state, output + error)
            task_id = checkpoint.parent.name
            with contextlib.closing(sqlite3.connect((state / 'tasks.db').as_uri() + '?mode=ro',
                                                   uri=True)) as database:
                assert database.execute('SELECT proxy_auth_required FROM tasks WHERE id=?', (task_id,)).fetchone() == (1,)
            before_proxy, before_origin = route.snapshot(), len(origin.hits)
            result = subprocess.run([str(cli), '--resume', task_id, str(state)], capture_output=True, text=True,
                                    timeout=15, creationflags=flags, env=base_environment)
            assert result.returncode != 0 and not target.exists(), (result.stdout, result.stderr)
            after_proxy = route.snapshot()
            assert after_proxy['destinations'] == before_proxy['destinations']
            assert after_proxy['auth_attempts'] == before_proxy['auth_attempts']
            assert len(origin.hits) == before_origin
            assert_no_secrets(state, result.stdout + result.stderr)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate(timeout=10)
        assert_no_secrets(root)
        print('PASS durable required marker, crash recovery rejects missing memory-only credentials, no secret persistence', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exercise', type=Path, required=True)
    exercise(parser.parse_args().exercise)
