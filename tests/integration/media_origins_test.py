"""Real parser, playable local media and exact-origin credential isolation."""
import argparse
import contextlib
import functools
import http.server
import json
import os
from pathlib import Path
import re
import sqlite3
import subprocess
import tempfile
import threading
import ssl
import datetime
import ipaddress

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID

from media_inventory_test import assets
from proxy_fixture import RecordingProxy

ROOT_COOKIE, ROOT_AUTH = 'owned_page_session=isolated-632', 'Bearer owned-page-894'
CDN_COOKIE, CDN_AUTH, CDN_HEADER = 'owned_cdn_session=isolated-127', 'Bearer owned-cdn-845', 'owned-cdn-token-963'
SECRETS = (ROOT_COOKIE, ROOT_AUTH, CDN_COOKIE, CDN_AUTH, CDN_HEADER)


class Origin(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, directory, credentials, tls=None):
        self.credentials = credentials
        self.requests = []
        self.connections = 0
        self.cdn = self.foreign = None
        super().__init__(('127.0.0.1', 0), functools.partial(Handler, directory=str(directory)))
        self.scheme = 'https' if tls else 'http'
        if tls:
            self.socket = tls.wrap_socket(self.socket, server_side=True)
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)
        self.thread.start()

    def url(self, path='', host='127.0.0.1'):
        return f'{self.scheme}://{host}:{self.server_port}/{path}'

    def close(self):
        self.shutdown()
        self.server_close()
        self.thread.join(timeout=5)

    def get_request(self):
        connection = super().get_request()
        self.connections += 1
        return connection


class Handler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def authorized(self):
        cookie, auth, header = self.server.credentials
        valid = (self.headers.get('Cookie') == cookie and self.headers.get('Authorization') == auth
                 and self.headers.get('X-Media-Token') == header and not self.headers.get('Proxy-Authorization'))
        self.server.requests.append((self.path, valid))
        if not valid:
            self.send_response(401)
            self.send_header('Content-Length', '0')
            self.end_headers()
        return valid

    def do_HEAD(self):
        if self.authorized():
            super().do_HEAD()

    def do_GET(self):
        if not self.authorized():
            return
        path = self.path.split('?', 1)[0]
        target = self.server.foreign if path.startswith('/foreign') else self.server.cdn
        if path in ('/redirect', '/foreign-redirect', '/foreign-ftp'):
            self.send_response(302)
            location = target.url('combined.mp4')
            if path == '/foreign-ftp':
                location = location.replace('http://', 'ftp://')
            self.send_header('Location', location)
            self.send_header('Content-Length', '0')
            self.send_header('Set-Cookie', 'owned_inherited=must-not-cross-ports; Path=/')
            self.end_headers()
            return
        if path in ('/page.html', '/foreign.html'):
            body = f'<html><title>Owned media</title><video controls src="{target.url("combined.mp4")}"></video></html>'.encode()
            mime = 'text/html'
        elif path == '/playlist.m3u8':
            body = ('#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n#EXTINF:2,\n'
                    + target.url('segment.ts') + '\n#EXT-X-ENDLIST\n').encode()
            mime = 'application/vnd.apple.mpegurl'
        elif path == '/manifest.mpd' and target:
            manifest = (Path(self.directory) / 'manifest.mpd').read_text()
            end = manifest.index('>', manifest.index('<MPD')) + 1
            body = (manifest[:end] + '<BaseURL>' + target.url() + '</BaseURL>' + manifest[end:]).encode()
            mime = 'application/dash+xml'
        else:
            with contextlib.suppress(OSError):
                super().do_GET()
            return
        self.send_response(200)
        self.send_header('Content-Type', mime)
        self.send_header('Content-Length', str(len(body)))
        if self.server.cdn is not None:
            self.send_header('Set-Cookie', 'owned_inherited=must-not-cross-ports; Path=/')
        self.end_headers()
        with contextlib.suppress(OSError):
            self.wfile.write(body)


def call(client, parser, muxer, case, url, rules, *options, inspect=False, success=True, parser_override=None,
         source_login=True):
    case.mkdir(parents=True, exist_ok=True)
    output = case / 'output.mkv'
    environment = os.environ.copy()
    environment['MASKPILOT_MEDIA_TEST_ORIGINS'] = json.dumps(rules)
    if source_login:
        environment.update(MASKPILOT_TEST_ROOT_COOKIE=ROOT_COOKIE, MASKPILOT_TEST_ROOT_AUTH=ROOT_AUTH)
    else:
        environment.pop('MASKPILOT_TEST_ROOT_COOKIE', None)
        environment.pop('MASKPILOT_TEST_ROOT_AUTH', None)
    root_login = ['--source-cookie-env', 'MASKPILOT_TEST_ROOT_COOKIE',
                  '--source-authorization-env', 'MASKPILOT_TEST_ROOT_AUTH'] if source_login else []
    command = list(map(str, [client, url, output, case / 'data', '--media', '--media-parser', parser_override or parser,
                            '--media-muxer', muxer, *root_login,
                            '--media-origins-env', 'MASKPILOT_MEDIA_TEST_ORIGINS', *options]))
    if inspect:
        command.append('--inspect-media')
    assert all(secret not in ' '.join(command) for secret in SECRETS)
    answer = subprocess.run(command, env=environment, capture_output=True, timeout=100)
    assert (answer.returncode == 0) == success, (answer.returncode, answer.stdout[-1200:], answer.stderr[-1500:])
    for secret in SECRETS:
        assert secret.encode() not in answer.stdout + answer.stderr, 'Source material leaked into logs'
    if success and inspect:
        assert json.loads(answer.stdout)['formats']
    if success and not inspect:
        probe = muxer.with_name('ffprobe.exe' if os.name == 'nt' else 'ffprobe')
        observed = subprocess.run([str(probe), '-v', 'error', '-show_streams', '-of', 'json', str(output)],
                                  capture_output=True, timeout=15)
        assert observed.returncode == 0, observed.stderr
        streams = json.loads(observed.stdout)['streams']
        assert {'audio', 'video'} <= {value['codec_type'] for value in streams}
    if not success:
        assert not output.exists()
    for file in (case / 'data').rglob('*'):
        if file.is_file():
            content = file.read_bytes()
            assert all(secret.encode() not in content for secret in SECRETS), file.name
    return answer


def exercise(client, parser, muxer):
    with tempfile.TemporaryDirectory(prefix='maskpilot-media-origins-') as name:
        root = Path(name)
        media = root / 'assets'
        media.mkdir()
        assets(muxer, media)
        source = Origin(media, (ROOT_COOKIE, ROOT_AUTH, None))
        cdn = Origin(media, (CDN_COOKIE, CDN_AUTH, CDN_HEADER))
        public = Origin(media, (None, None, None))
        foreign = Origin(media, (None, None, None))
        key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'owned-media-test')])
        now = datetime.datetime.now(datetime.timezone.utc)
        certificate = (x509.CertificateBuilder().subject_name(subject).issuer_name(subject).public_key(key.public_key())
                       .serial_number(x509.random_serial_number()).not_valid_before(now - datetime.timedelta(minutes=1))
                       .not_valid_after(now + datetime.timedelta(hours=1))
                       .add_extension(x509.SubjectAlternativeName([x509.IPAddress(ipaddress.ip_address('127.0.0.1'))]), critical=False)
                       .sign(key, hashes.SHA256()))
        cert_path, key_path = root / 'owned-cert.pem', root / 'owned-key.pem'
        cert_path.write_bytes(certificate.public_bytes(serialization.Encoding.PEM))
        key_path.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                               serialization.NoEncryption()))
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(cert_path, key_path)
        untrusted = Origin(media, (CDN_COOKIE, CDN_AUTH, CDN_HEADER), tls)
        source.cdn, source.foreign = cdn, foreign
        rules = [dict(origin=cdn.url(), cookie=CDN_COOKIE, authorization=CDN_AUTH, headers={'X-Media-Token': CDN_HEADER})]
        try:
            for path in ('page.html', 'playlist.m3u8', 'manifest.mpd', 'redirect'):
                call(client, parser, muxer, root / path / 'inspect', source.url(path), rules, inspect=True)
                call(client, parser, muxer, root / path / 'download', source.url(path), rules)
                print('PASS independent page/CDN credentials and playable audio/video:', path, flush=True)
            assert source.requests and cdn.requests and all(valid for _, valid in source.requests + cdn.requests)
            for path in ('foreign-redirect', 'foreign.html', 'foreign-ftp'):
                call(client, parser, muxer, root / path, source.url(path), rules, success=False)
                assert not foreign.requests
                assert not foreign.connections
            print('PASS unapproved origins receive no requests or credentials', flush=True)
            source.cdn = public
            call(client, parser, muxer, root / 'public-cdn', source.url('playlist.m3u8'), [dict(origin=public.url())])
            assert public.requests and all(valid for _, valid in public.requests)
            source.cdn = cdn
            print('PASS anonymous CDN receives no page login or inherited custom headers', flush=True)
            source.cdn = untrusted
            call(client, parser, muxer, root / 'untrusted-tls', source.url('playlist.m3u8'),
                 [dict(origin=untrusted.url(), cookie=CDN_COOKIE, authorization=CDN_AUTH,
                       headers={'X-Media-Token': CDN_HEADER})], success=False)
            assert not untrusted.requests, 'Invalid TLS certificate received HTTP credentials'
            source.cdn = cdn
            print('PASS invalid CDN TLS certificate receives no HTTP request or credentials', flush=True)
            for kind in ('http', 'socks5'):
                proxy = RecordingProxy(kind, 'owned-user', 'owned-password')
                try:
                    call(client, parser, muxer, root / kind, source.url('playlist.m3u8'), rules,
                         '--proxy', proxy.url(), '--proxy-user', 'owned-user', '--proxy-password', 'owned-password')
                    report = proxy.snapshot()
                    assert report['auth_accepted'] > 0
                    assert {value['port'] for value in report['destinations']} == {source.server_port, cdn.server_port}
                    before = len(source.requests) + len(cdn.requests)
                    call(client, parser, muxer, root / (kind + '-wrong'), source.url('playlist.m3u8'), rules,
                         '--proxy', proxy.url(), '--proxy-user', 'owned-user', '--proxy-password', 'wrong', success=False)
                    assert len(source.requests) + len(cdn.requests) == before
                    print('PASS exact-origin media retains', kind, 'route and refuses fallback', flush=True)
                finally:
                    proxy.close()
            before = len(source.requests) + len(cdn.requests)
            case = root / 'wrong-cdn'
            source.credentials = (None, None, None)
            failed = call(client, parser, muxer, case, source.url('playlist.m3u8'),
                          [dict(origin=cdn.url(), cookie='wrong', authenticationRequired=True)], success=False,
                          source_login=False)
            match = re.search(rb'TASK_ID ([a-f0-9]{32})', failed.stdout)
            assert match
            before = len(source.requests) + len(cdn.requests)
            resumed = subprocess.run([str(client), '--resume', match[1].decode(), str(case / 'data'),
                                      '--media-parser', str(parser), '--media-muxer', str(muxer)], capture_output=True, timeout=25)
            assert resumed.returncode != 0 and before == len(source.requests) + len(cdn.requests)
            with contextlib.closing(sqlite3.connect(case / 'data' / 'tasks.db')) as db:
                value = db.execute('SELECT media_origins FROM tasks').fetchone()[0]
                assert value.startswith('1 ') and CDN_COOKIE not in value
                assert db.execute('SELECT origin_auth_required FROM tasks').fetchone() == (0,)
            print('PASS missing additional login and approval after restart refuses before any origin contact', flush=True)
            before = len(source.requests) + len(cdn.requests)
            source.credentials = (ROOT_COOKIE, ROOT_AUTH, None)
            call(client, parser, muxer, root / 'incompatible-parser', source.url('playlist.m3u8'), rules,
                 success=False, parser_override=muxer)
            assert before == len(source.requests) + len(cdn.requests)
            print('PASS incompatible component cannot bypass adapter readiness', flush=True)
            assert not foreign.requests
            assert not foreign.connections
        finally:
            for server in (source, cdn, public, foreign, untrusted):
                server.close()


if __name__ == '__main__':
    arguments = argparse.ArgumentParser()
    arguments.add_argument('--exercise', required=True, type=Path)
    arguments.add_argument('--parser', required=True, type=Path)
    arguments.add_argument('--muxer', required=True, type=Path)
    options = arguments.parse_args()
    exercise(options.exercise.resolve(), options.parser.resolve(), options.muxer.resolve())
