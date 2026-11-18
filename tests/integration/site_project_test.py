"""Real project CLI -> actor -> native HTTP -> immutable offline revision acceptance."""
import argparse
from contextlib import ExitStack
import hashlib
import http.server
import json
import os
from pathlib import Path
import re
import sqlite3
import struct
import zlib
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'support'))
from proxy_fixture import RecordingProxy

def png_chunk(kind, body):
    return struct.pack('!I', len(body)) + kind + body + struct.pack('!I', zlib.crc32(kind + body) & 0xffffffff)


PNG = (b'\x89PNG\r\n\x1a\n' + png_chunk(b'IHDR', struct.pack('!IIBBBBB', 1, 1, 8, 6, 0, 0, 0))
       + png_chunk(b'IDAT', zlib.compress(b'\x00\x33\x66\x99\xff')) + png_chunk(b'IEND', b''))


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self):
        self.lock = threading.Lock()
        self.hits = []
        self.version = 1
        self.fail_css = False
        self.slow = False
        self.closed = False
        super().__init__(('127.0.0.1', 0), Handler)
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)
        self.thread.start()

    def url(self, path='/docs'):
        return f'http://127.0.0.1:{self.server_port}{path}'

    def count(self, path):
        with self.lock:
            return sum(item[0] == path for item in self.hits)

    def close(self):
        if not self.closed:
            self.closed = True
            self.shutdown()
            self.server_close()
            self.thread.join(timeout=5)


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def do_GET(self):
        with self.server.lock:
            self.server.hits.append((self.path, self.headers.get('Range', '')))
        if self.path == '/docs':
            self.send_response(301)
            self.send_header('Location', '/docs/index.html')
            self.send_header('Content-Length', '0')
            self.send_header('Connection', 'close')
            self.end_headers()
            self.close_connection = True
            return
        version = self.server.version
        data = {
            '/docs/index.html': (f'<!doctype html><html><head><title>Fixture v{version}</title><link rel="stylesheet" href="../style.css"></head><body><h1>Fixture v{version}</h1><a href="page">Next</a><a href="/docs/index.html#again">Cycle</a><a href="/blocked/page">Excluded</a><a href="../img.png"><img src="../img.png"></a><script>window.projectSentinel=1</script></body></html>'.encode(), 'text/html; charset=UTF-8'),
            '/docs/page': (b'<html><body><a href="index.html">Home</a><img src="../img.png"></body></html>', 'text/html; charset=UTF-8'),
            '/style.css': (b'@import url("/import.css");body{color:#123;background-image:url("/img.png")}', 'text/css; charset=UTF-8'),
            '/import.css': (b'@import "/style.css";h1{font-weight:bold}', 'text/css; charset=UTF-8'),
            '/img.png': (PNG + (b'padding' * 80000 if self.server.slow else bytes([version])), 'image/png'),
            '/blocked/page': (b'<html>excluded</html>', 'text/html'),
        }
        if self.path not in data or (self.path == '/style.css' and self.server.fail_css):
            self.send_response(500 if self.server.fail_css else 404)
            self.send_header('Content-Length', '0')
            self.send_header('Connection', 'close')
            self.end_headers()
            self.close_connection = True
            return
        body, mime = data[self.path]
        match = re.fullmatch(r'bytes=(\d+)-(\d+)', self.headers.get('Range', ''))
        begin, end = (int(match[1]), min(int(match[2]), len(body) - 1)) if match else (0, len(body) - 1)
        self.send_response(206 if match else 200)
        self.send_header('Content-Type', mime)
        self.send_header('Content-Length', str(end - begin + 1))
        self.send_header('ETag', f'"project-v{version}-{len(body)}"')
        if match:
            self.send_header('Content-Range', f'bytes {begin}-{end}/{len(body)}')
        self.send_header('Connection', 'close')
        self.end_headers()
        self.close_connection = True
        try:
            for offset in range(begin, end + 1, 4096):
                self.wfile.write(body[offset:min(end + 1, offset + 4096)])
                self.wfile.flush()
                if self.path == '/img.png' and self.server.slow and end > begin:
                    time.sleep(.025)
        except (ConnectionError, OSError):
            pass


def wait_for(predicate, timeout=15):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        if predicate():
            return
        time.sleep(.025)
    raise AssertionError('Owned project fixture condition timed out')


def inventory(path):
    return {str(file.relative_to(path)): hashlib.sha256(file.read_bytes()).hexdigest()
            for file in path.rglob('*') if file.is_file()}


def stop_owned(process):
    # Registered on the inner ExitStack immediately after Popen, before any assertions.
    # Release pipes/database handles before TemporaryDirectory attempts Windows cleanup.
    if process.poll() is None:
        process.kill()
    try:
        process.communicate(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
        process.communicate(timeout=5)


def exercise(executable, browser=None):
    server = Server()
    try:
        with tempfile.TemporaryDirectory(prefix='maskpilot-site-project-') as temporary, ExitStack() as stack:
            root = Path(temporary)
            data = root / 'state'
            sequence = 0

            def request(action, identity=None, revision=None, options=None, *, good=True, env=None):
                nonlocal sequence
                sequence += 1
                value = dict(action=action, timeoutSeconds=60)
                if identity is not None:
                    value['id'] = identity
                if revision is not None:
                    value['revision'] = revision
                if options is not None:
                    value['options'] = options
                if env:
                    value.update(proxyUsernameEnv='MASKPILOT_PROJECT_USER', proxyPasswordEnv='MASKPILOT_PROJECT_PASSWORD')
                path = root / f'request-{sequence}.json'
                path.write_text(json.dumps(value), encoding='utf-8')
                child_env = os.environ.copy()
                if env:
                    child_env.update(env)
                result = subprocess.run([str(executable), '--project', str(path), str(data)],
                                        capture_output=True, text=True, timeout=85, env=child_env)
                try:
                    answer = json.loads(result.stdout)
                except ValueError:
                    raise AssertionError((result.returncode, result.stdout, result.stderr)) from None
                if good:
                    assert result.returncode == 0 and answer.get('ok'), (value, answer, result.stderr)
                else:
                    assert result.returncode != 0 and not answer.get('ok'), (value, answer)
                return answer

            options = dict(name='Owned static project', rootUrl=server.url(), outputDirectory=str(root / 'exports'),
                           maxDepth=2, maxPages=8, maxResources=16, maxPageBytes=65536,
                           maxAssetBytes=2 * 1024 * 1024, maxStoredBytes=8 * 1024 * 1024,
                           maxOutputBytes=16 * 1024 * 1024, excludePrefixes=['/blocked'], fonts=False)
            created = request('create', options=options)['project']
            identity = created['id']
            assert (data / 'site-projects' / (identity + '.json')).is_file(), 'Project manifest is outside its dedicated namespace'
            sentinel = data / 'ordinary-config.json'
            sentinel.write_text(json.dumps({'fixture': 'unrelated application configuration'}), encoding='utf-8')
            assert request('status', identity)['project']['state'] == 'Draft', 'Unrelated app JSON disabled project loading'
            assert created['state'] == 'Draft' and not server.hits
            preview = request('preview', identity, 1)
            assert preview['project']['state'] == 'PreviewReady', preview
            assert server.count('/img.png') == 0 and server.count('/blocked/page') == 0
            assert server.count('/docs/page') > 0 and server.count('/page') == 0
            assert len(preview['entries']) <= 16
            print('PASS create contacts nothing; final-base HTML/CSS preview resolves cycles/filters without identified binary fetch', flush=True)

            completed = request('start', identity, 1)
            assert completed['project']['state'] == 'Completed'
            first = Path(request('open', identity)['path'])
            assert first.is_file() and 'Fixture v1' in first.read_text(encoding='utf-8')
            old = inventory(first.parent)
            assert server.count('/img.png') > 0
            print('PASS explicit Start uses native payload tasks and publishes a verified offline revision', flush=True)

            before_images = server.count('/img.png')
            server.version = 2
            updated = request('update', identity, 1, options=options)
            assert updated['project']['revision'] == 2 and updated['project']['state'] == 'PreviewReady'
            assert server.count('/img.png') == before_images
            assert Path(request('open', identity)['path']) == first
            assert inventory(first.parent) == old
            request('start', identity, 2)
            second = Path(request('open', identity)['path'])
            assert second != first and second.is_file() and 'Fixture v2' in second.read_text(encoding='utf-8')
            assert inventory(first.parent) == old
            second_inventory = inventory(second.parent)
            print('PASS Update previews a new revision; explicit Start preserves every older exported byte', flush=True)

            server.fail_css = True
            failed = request('update', identity, 2, options=options, good=False)
            assert failed['project']['state'] == 'NeedsAttention'
            assert Path(request('open', identity)['path']) == second
            request('cancel', identity, 3)
            assert Path(request('open', identity)['path']) == second
            assert inventory(second.parent) == second_inventory
            print('PASS failed/cancelled update retains the previous verified Open path', flush=True)

            server.fail_css = False
            server.version = 4
            request('update', identity, 3, options=options)
            server.slow = True
            before = len(server.hits)
            path = root / 'crash-request.json'
            path.write_text(json.dumps(dict(action='start', id=identity, revision=4, timeoutSeconds=60)), encoding='utf-8')
            process = subprocess.Popen([str(executable), '--project', str(path), str(data)],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            stack.callback(stop_owned, process)
            wait_for(lambda: any(route == '/img.png' and value != 'bytes=0-0' for route, value in server.hits[before:]))
            process.kill()
            process.communicate(timeout=10)
            contacts = len(server.hits)
            status = request('status', identity)
            assert status['project']['state'] == 'Paused', status
            assert len(server.hits) == contacts, 'Restart/status auto-started a project child'
            resumed = request('resume', identity, 4)
            assert resumed['project']['state'] == 'Completed', resumed
            final = Path(request('open', identity)['path'])
            assert final.is_file() and inventory(second.parent) == second_inventory
            print('PASS kill during binary transfer leaves startup inert until explicit Resume', flush=True)

            # Actual authenticated route remains memory-only across separate CLI invocations.
            proxy = RecordingProxy('socks5', username='project-user', password='project-pass')
            stack.callback(proxy.close)
            credentials = {'MASKPILOT_PROJECT_USER': 'project-user', 'MASKPILOT_PROJECT_PASSWORD': 'project-pass'}
            proxied_options = dict(options, name='Proxied public project', proxy=proxy.url(), proxyAuthenticationRequired=True)
            proxied_options['images'] = False
            proxied = request('create', options=proxied_options, env=credentials)['project']
            request('preview', proxied['id'], 1, env=credentials)
            request('start', proxied['id'], 1, env=credentials)
            assert proxy.snapshot()['auth_accepted'] > 0 and proxy.snapshot()['destinations']
            for file in data.rglob('*'):
                if file.is_file():
                    content = file.read_bytes()
                    assert b'project-user' not in content and b'project-pass' not in content
            print('PASS authenticated SOCKS route uses private credentials and persists no proxy secrets', flush=True)

            request('remove', identity, 4)
            assert final.is_file() and inventory(first.parent) == old
            assert all(item['id'] != identity for item in request('list')['projects'])
            assert json.loads(sentinel.read_text(encoding='utf-8')) == {'fixture': 'unrelated application configuration'}
            print('PASS removing history preserves exported revisions', flush=True)
            if browser:
                server.close()
                result = subprocess.run(['node', str(browser), '--index', str(final)], capture_output=True,
                                        text=True, timeout=60)
                assert result.returncode == 0, (result.stdout, result.stderr)
                print(result.stdout, end='')
    finally:
        server.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--exercise', type=Path, required=True)
    parser.add_argument('--browser', type=Path)
    arguments = parser.parse_args()
    exercise(arguments.exercise.resolve(), arguments.browser.resolve() if arguments.browser else None)
