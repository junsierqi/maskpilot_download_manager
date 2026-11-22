"""Generated playable HLS through the actual native engine; no public sites or accounts."""
import argparse
import contextlib
import datetime
import http.server
import ipaddress
import json
import os
from pathlib import Path
import re
import sqlite3
import ssl
import subprocess
import sys
import tempfile
import threading
from urllib.parse import urlsplit

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.padding import PKCS7
from cryptography.x509.oid import NameOID

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'support'))
from proxy_fixture import RecordingProxy

ROOT_COOKIE = 'owned_live_page=1943'
CDN_COOKIE = 'owned_live_cdn=8252'
CDN_AUTH = 'Bearer owned-live-cdn-8219'
CDN_HEADER = 'owned-live-token-253'
SECRETS = (ROOT_COOKIE, CDN_COOKIE, CDN_AUTH, CDN_HEADER)


class Origin(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, tls=None):
        self.files, self.redirects, self.keys = {}, {}, []
        self.requests, self.connections = [], 0
        self.required = (None, None)
        self.bad_range = None
        self.hold_path = None
        self.held, self.release = threading.Event(), threading.Event()
        super().__init__(('127.0.0.1', 0), Handler)
        self.scheme = 'https' if tls else 'http'
        if tls:
            self.socket = tls.wrap_socket(self.socket, server_side=True)
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)
        self.thread.start()

    def url(self, name=''):
        return f'{self.scheme}://127.0.0.1:{self.server_port}/{name}'

    def get_request(self):
        result = super().get_request()
        self.connections += 1
        return result

    def close(self):
        self.shutdown()
        self.server_close()
        self.thread.join(5)


class Handler(http.server.BaseHTTPRequestHandler):
    def handle(self):
        with contextlib.suppress(ConnectionResetError, ConnectionAbortedError, BrokenPipeError):
            super().handle()

    def log_message(self, *args):
        pass

    def do_GET(self):
        valid = (self.headers.get('Cookie'), self.headers.get('Authorization')) == self.server.required
        valid &= not self.headers.get('Proxy-Authorization')
        valid &= self.headers.get('X-Live-Token') == (CDN_HEADER if self.server.required[0] == CDN_COOKIE else None)
        path = urlsplit(self.path).path.lstrip('/')
        self.server.requests.append((path, valid, dict(self.headers)))
        if path == self.server.hold_path:
            self.server.held.set()
            self.server.release.wait(20)
        if not valid:
            body, status = b'', 403
        elif path in self.server.redirects:
            body, status = b'', 302
        else:
            body = self.server.files.get(path)
            if path == 'rotating.key' and self.server.keys:
                body = self.server.keys.pop(0)
            status = 404 if body is None else 200
            body = body or b''
        content_range = None
        requested = self.headers.get('Range')
        if status == 200 and requested and self.server.bad_range != 'status':
            match = re.fullmatch(r'bytes=([0-9]+)-([0-9]+)', requested)
            assert match
            first, last = map(int, match.groups())
            content_range = f'bytes {first}-{last}/{len(body)}'
            body, status = body[first:last + 1], 206
            if self.server.bad_range == 'offset':
                content_range = f'bytes {first + 1}-{last + 1}/{last + 10}'
            if self.server.bad_range == 'short':
                body = body[:-1]
        self.send_response(status)
        self.send_header('Content-Length', str(len(body)))
        if path in self.server.redirects:
            self.send_header('Location', self.server.redirects[path])
        if content_range:
            self.send_header('Content-Range', content_range)
            if self.server.bad_range == 'duplicate':
                self.send_header('Content-Range', content_range)
        self.end_headers()
        with contextlib.suppress(OSError):
            self.wfile.write(body)


def run_tool(program, arguments, cwd=None):
    result = subprocess.run([str(program), *map(str, arguments)], capture_output=True, timeout=45, cwd=cwd)
    assert result.returncode == 0, result.stderr[-2000:]
    return result.stdout


def encrypted(data, key, iv):
    pad = PKCS7(128).padder()
    cipher = Cipher(algorithms.AES(key), modes.CBC(iv)).encryptor()
    return cipher.update(pad.update(data) + pad.finalize()) + cipher.finalize()


def manifest(names, duration=1):
    return ('#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:42\n' +
            ''.join(f'#EXTINF:{duration},\n{name}\n' for name in names) + '#EXT-X-ENDLIST\n').encode()


def exercise(client, muxer):
    probe = muxer.with_name('ffprobe.exe' if os.name == 'nt' else 'ffprobe')
    with tempfile.TemporaryDirectory(prefix='maskpilot-hls-compatibility-') as temporary:
        root = Path(temporary)
        assets = root / 'assets'
        assets.mkdir()
        prefix = ['-nostdin', '-hide_banner', '-loglevel', 'error']
        video = ['-f', 'lavfi', '-i', 'testsrc2=size=160x96:rate=10', '-t', '3',
                 '-c:v', 'libx264', '-preset', 'ultrafast', '-g', '10', '-sc_threshold', '0']
        run_tool(muxer, [*prefix, *video, '-hls_time', '1', '-hls_list_size', '0',
                        '-hls_segment_filename', assets / 'ts-%d.ts', assets / 'ts.m3u8'], cwd=assets)
        run_tool(muxer, [*prefix, *video, '-hls_time', '1', '-hls_list_size', '0',
                        '-hls_segment_type', 'fmp4', '-hls_fmp4_init_filename', 'video-init.mp4',
                        '-hls_segment_filename', assets / 'video-%d.m4s', assets / 'video.m3u8'], cwd=assets)
        run_tool(muxer, [*prefix, '-f', 'lavfi', '-i', 'sine=frequency=700:sample_rate=48000',
                        '-t', '3', '-c:a', 'aac', '-f', 'hls', '-hls_time', '1', '-hls_list_size', '0',
                        '-hls_segment_type', 'fmp4', '-hls_fmp4_init_filename', 'audio-init.mp4',
                        '-hls_segment_filename', assets / 'audio-%d.m4s', assets / 'audio.m3u8'], cwd=assets)
        source, cdn, public, foreign = [Origin() for _ in range(4)]
        source.required = (ROOT_COOKIE, None)
        cdn.required = (CDN_COOKIE, CDN_AUTH)
        for file in assets.iterdir():
            source.files[file.name] = cdn.files[file.name] = public.files[file.name] = file.read_bytes()
        environment = dict(os.environ, OWNED_LIVE_SOURCE=ROOT_COOKIE)
        rules = [dict(origin=cdn.url(), cookie=CDN_COOKIE, authorization=CDN_AUTH, headers={'X-Live-Token': CDN_HEADER})]
        environment['MASKPILOT_MEDIA_LIVE_ACCESS'] = json.dumps(rules)
        servers = [source, cdn, public, foreign]

        def call(name, playlist, success=True, processor=False, extra=(), url=None, origin_rules=True, resume=None):
            destination, data = root / f'{name}.ts', root / name
            if resume:
                command = [client, '--resume', resume, data]
            else:
                command = [client, url or source.url(playlist), destination, data, '--live-hls']
            command += ['--source-cookie-env', 'OWNED_LIVE_SOURCE']
            if origin_rules:
                command += ['--live-origins-env', 'MASKPILOT_MEDIA_LIVE_ACCESS']
            if processor:
                command += ['--live-muxer', muxer]
            result = subprocess.run(list(map(str, [*command, *extra])), capture_output=True, env=environment, timeout=60)
            assert (result.returncode == 0) == success, (name, result.returncode, result.stdout[-1500:], result.stderr[-1500:],
                [(r[0], r[1]) for server in servers for r in server.requests[-5:]], sorted(cdn.files))
            assert all(secret.encode() not in result.stdout + result.stderr for secret in SECRETS)
            for file in data.rglob('*'):
                if file.is_file():
                    assert all(secret.encode() not in file.read_bytes() for secret in SECRETS), file
            if success:
                run_tool(muxer, [*prefix, '-i', destination, '-f', 'null', '-'])
                streams = json.loads(run_tool(probe, ['-v', 'error', '-show_entries', 'stream=codec_type', '-of', 'json', destination]))
                assert any(s['codec_type'] == 'video' for s in streams['streams'])
            else:
                assert not destination.exists()
                streams = {}
            return destination, data, result, streams

        try:
            # Explicit and sequence-derived IVs, same key URI rotating every segment.
            key1, key2 = bytes(range(16)), bytes(range(16, 32))
            cdn.files['encrypted-0.bin'] = encrypted(cdn.files['ts-0.ts'], key1, (42).to_bytes(16, 'big'))
            cdn.files['encrypted-1.bin'] = encrypted(cdn.files['ts-1.ts'], key2, (43).to_bytes(16, 'big'))
            source.files['aes.m3u8'] = manifest([cdn.url('encrypted-0.bin'), cdn.url('encrypted-1.bin')]).replace(
                b'#EXTINF:1,', ('#EXT-X-KEY:METHOD=AES-128,URI="' + cdn.url('rotating.key') + '"\n#EXTINF:1,').encode())
            cdn.keys = [key1, key2]
            output, *_ = call('aes', 'aes.m3u8')
            assert output.read_bytes() == cdn.files['ts-0.ts'] + cdn.files['ts-1.ts']
            source.files['explicit.m3u8'] = source.files['aes.m3u8'].replace(b'METHOD=AES-128,', b'METHOD=AES-128,IV=0x2a,', 1)
            cdn.keys = [key1, key2]
            call('explicit', 'explicit.m3u8')
            print('PASS actual decoded AES128, sequence/explicit IV, same-URI key rotation and exact-origin credentials', flush=True)

            first, second = cdn.files['ts-0.ts'], cdn.files['ts-1.ts']
            cdn.files['ranges.bin'] = b'prefix' + first + second
            # Replace with explicit first and implicit second byte offsets.
            source.files['ranges.m3u8'] = (
                '#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXTINF:1,\n'
                f'#EXT-X-BYTERANGE:{len(first)}@6\n{cdn.url("ranges.bin")}\n#EXTINF:1,\n'
                f'#EXT-X-BYTERANGE:{len(second)}\n{cdn.url("ranges.bin")}\n#EXT-X-ENDLIST\n').encode()
            output, *_ = call('ranges', 'ranges.m3u8')
            assert output.read_bytes() == first + second
            for invalid in ('status', 'offset', 'short', 'duplicate'):
                cdn.bad_range = invalid
                call('bad-range-' + invalid, 'ranges.m3u8', success=False)
            cdn.bad_range = None
            print('PASS exact206 ranges including implicit offset; status200, wrong/short/duplicate Content-Range refuse publication', flush=True)

            source.files['fmp4.m3u8'] = manifest([cdn.url('video-0.m4s'), cdn.url('video-1.m4s')]).replace(
                b'#EXTINF:1,', ('#EXT-X-MAP:URI="' + cdn.url('video-init.mp4') + '"\n#EXTINF:1,').encode(), 1)
            call('fmp4', 'fmp4.m3u8', processor=True)
            initialization = cdn.files['video-init.mp4']
            fragment = cdn.files['video-0.m4s']
            cdn.files['fmp4-ranges.bin'] = initialization + fragment
            source.files['map-range.m3u8'] = manifest([cdn.url('fmp4-ranges.bin')]).replace(
                b'#EXTINF:1,', (f'#EXT-X-MAP:URI="{cdn.url("fmp4-ranges.bin")}",BYTERANGE="{len(initialization)}@0"\n'
                    f'#EXTINF:1,\n#EXT-X-BYTERANGE:{len(fragment)}@{len(initialization)}').encode() + b'\n')
            call('map-range', 'map-range.m3u8', processor=True)
            # Encrypted initialization must carry an explicit IV; segment keys remain memory-only.
            explicit_iv = (99).to_bytes(16, 'big')
            cdn.files['map.key'] = key1
            cdn.files['map-encrypted.bin'] = encrypted(cdn.files['video-init.mp4'], key1, explicit_iv)
            cdn.files['fragment-encrypted.bin'] = encrypted(cdn.files['video-0.m4s'], key1, explicit_iv)
            source.files['encrypted-map.m3u8'] = manifest([cdn.url('fragment-encrypted.bin')]).replace(
                b'#EXTINF:1,', (f'#EXT-X-KEY:METHOD=AES-128,URI="{cdn.url("map.key")}",IV=0x63\n'
                                f'#EXT-X-MAP:URI="{cdn.url("map-encrypted.bin")}"\n#EXTINF:1,').encode())
            call('encrypted-map', 'encrypted-map.m3u8', processor=True)
            cdn.files['disguised.m4s'] = ('#EXTM3U\n#EXTINF:1,\n' + foreign.url('stolen.ts') + '\n').encode()
            source.files['disguised.m3u8'] = source.files['fmp4.m3u8'].replace(b'video-0.m4s', b'disguised.m4s')
            call('disguised', 'disguised.m3u8', success=False, processor=True)
            source.files['bounded-output.m3u8'] = manifest([cdn.url('video-0.m4s')] * 100).replace(
                b'#EXTINF:1,', ('#EXT-X-DISCONTINUITY\n#EXT-X-MAP:URI="' + cdn.url('video-init.mp4') + '"\n#EXTINF:1,').encode())
            _, _, bounded, _ = call('bounded-output', 'bounded-output.m3u8', success=False, processor=True,
                                    extra=['--live-max-bytes', '1048576'])
            assert b'exceeds' in bounded.stderr, bounded.stderr
            # A forced exit can leave an owned local assembly; normal restart/completion reclaims it.
            crash_data, crash_output = root / 'crash', root / 'crash.ts'
            cdn.hold_path = 'video-1.m4s'
            process = subprocess.Popen(list(map(str, [client, source.url('fmp4.m3u8'), crash_output, crash_data,
                '--live-hls', '--live-muxer', muxer, '--source-cookie-env', 'OWNED_LIVE_SOURCE',
                '--live-origins-env', 'MASKPILOT_MEDIA_LIVE_ACCESS'])), stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=environment)
            try:
                assert cdn.held.wait(20), 'Native recorder did not reach the owned held fragment'
                process.kill()
                process.communicate(timeout=15)
                assert not crash_output.exists()
                assert list(crash_data.rglob('live-local-*')), 'Expected a crash-left owned assembly'
                for file in crash_data.rglob('*'):
                    if file.is_file():
                        content = file.read_bytes()
                        assert key1 not in content and key2 not in content, 'Raw HLS key persisted in owned data'
                with contextlib.closing(sqlite3.connect(crash_data / 'tasks.db')) as database:
                    identifier = database.execute('SELECT id FROM tasks').fetchone()[0]
                cdn.hold_path = None
                cdn.release.set()
                before = sum(len(server.requests) for server in servers)
                call('crash', 'fmp4.m3u8', success=False, resume=identifier, origin_rules=False)
                assert before == sum(len(server.requests) for server in servers)
                call('crash', 'fmp4.m3u8', processor=True, resume=identifier)
                assert not list(crash_data.rglob('live-local-*'))
            finally:
                cdn.hold_path = None
                cdn.release.set()
                if process.poll() is None:
                    process.kill()
                    process.communicate(timeout=15)
            before = len(cdn.requests)
            call('missing-processor', 'fmp4.m3u8', success=False)
            assert len(cdn.requests) == before
            source.files['periods.m3u8'] = manifest([cdn.url('ts-0.ts'), cdn.url('ts-0.ts')]).replace(
                b'#EXTINF:1,', b'#EXT-X-DISCONTINUITY\n#EXTINF:1,', 2)
            call('periods', 'periods.m3u8', processor=True)
            print('PASS local-only fMP4 initialization and discontinuity remux, decoded output, missing processor before segment contact', flush=True)

            # Aligned complete fMP4 segments with distinct audio rendition.
            source.files['audio-aligned.m3u8'] = manifest([cdn.url('audio-0.m4s'), cdn.url('audio-1.m4s')]).replace(
                b'#EXTINF:1,', ('#EXT-X-MAP:URI="' + cdn.url('audio-init.mp4') + '"\n#EXTINF:1,').encode(), 1)
            clock = b'#EXT-X-PROGRAM-DATE-TIME:2026-01-01T00:00:00.000Z\n'
            source.files['fmp4.m3u8'] = source.files['fmp4.m3u8'].replace(b'#EXTINF:1,', clock + b'#EXTINF:1,', 1)
            source.files['audio-aligned.m3u8'] = source.files['audio-aligned.m3u8'].replace(b'#EXTINF:1,', clock + b'#EXTINF:1,', 1)
            source.files['master.m3u8'] = (
                '#EXTM3U\n#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="a",NAME="audio",DEFAULT=YES,URI="audio-aligned.m3u8"\n'
                '#EXT-X-STREAM-INF:BANDWIDTH=100000,AUDIO="a"\nfmp4.m3u8\n').encode()
            _, _, _, streams = call('audio', 'master.m3u8', processor=True)
            assert {'audio', 'video'} <= {s['codec_type'] for s in streams['streams']}
            source.files['bad-audio.m3u8'] = source.files['audio-aligned.m3u8'].replace(b'#EXTINF:1,', b'#EXTINF:1.5,')
            source.files['bad-master.m3u8'] = source.files['master.m3u8'].replace(b'audio-aligned', b'bad-audio')
            call('bad-audio', 'bad-master.m3u8', success=False, processor=True)
            source.files['no-clock.m3u8'] = source.files['audio-aligned.m3u8'].replace(clock, b'')
            source.files['no-clock-master.m3u8'] = source.files['master.m3u8'].replace(b'audio-aligned', b'no-clock')
            source.files['wrong-clock.m3u8'] = source.files['audio-aligned.m3u8'].replace(b'2026-01-01', b'2027-01-01')
            source.files['wrong-clock-master.m3u8'] = source.files['master.m3u8'].replace(b'audio-aligned', b'wrong-clock')
            for case in ('no-clock', 'wrong-clock'):
                before = len(cdn.requests)
                call(case, case + '-master.m3u8', success=False, processor=True)
                assert len(cdn.requests) == before
            print('PASS separate audio/video with common program clock; duration mismatch and same-sequence missing/conflicting clocks refuse before segment contact', flush=True)

            source.files['ll.m3u8'] = manifest([cdn.url('ts-0.ts')]).replace(b'#EXTINF:1,',
                ('#EXT-X-PART-INF:PART-TARGET=0.5\n#EXT-X-PART:DURATION=0.5,URI="' + foreign.url('partial.ts') + '"\n#EXTINF:1,').encode())
            call('ll', 'll.m3u8')
            source.files['delta.m3u8'] = source.files['ll.m3u8'].replace(b'#EXTINF:1,', b'#EXT-X-SKIP:SKIPPED-SEGMENTS=1\n#EXTINF:1,')
            call('delta', 'delta.m3u8', success=False)
            source.files['foreign.m3u8'] = manifest([foreign.url('stolen.ts')])
            call('foreign', 'foreign.m3u8', success=False)
            source.files['redirect.m3u8'] = manifest([cdn.url('redirect')])
            cdn.redirects['redirect'] = public.url('ts-0.ts')
            environment['MASKPILOT_MEDIA_LIVE_ACCESS'] = json.dumps([*rules, dict(origin=public.url())])
            call('anonymous', 'redirect.m3u8')
            assert public.requests and all(row[1] for row in public.requests)
            environment['MASKPILOT_MEDIA_LIVE_ACCESS'] = json.dumps(rules)
            source.files['drm.m3u8'] = source.files['aes.m3u8'].replace(b'AES-128', b'SAMPLE-AES')
            call('drm', 'drm.m3u8', success=False)
            cdn.keys = [key1[:-1]]
            call('short-key', 'aes.m3u8', success=False)
            cdn.keys = [key1, key2]
            saved = cdn.files['encrypted-0.bin']
            cdn.files['encrypted-0.bin'] = saved[:-1]
            call('bad-cipher', 'aes.m3u8', success=False)
            cdn.files['encrypted-0.bin'] = saved
            assert foreign.connections == 0 and not foreign.requests
            print('PASS complete-segment LL fallback without partial contacts, forbidden origin/DRM/delta/bad key/cipher refusal, redirect to anonymous origin without credentials', flush=True)

            for scheme in ('http', 'socks5'):
                proxy = RecordingProxy(scheme, 'route-user', 'route-pass', mappings={'live.fixture.invalid': '127.0.0.1'})
                try:
                    call('proxy-' + scheme, 'ranges.m3u8',
                         url=source.url('ranges.m3u8').replace('127.0.0.1', 'live.fixture.invalid'),
                         extra=['--proxy', proxy.url(), '--proxy-user', 'route-user', '--proxy-password', 'route-pass'])
                    assert proxy.snapshot()['auth_accepted'] > 0
                    before = sum(len(s.requests) for s in servers)
                    call('wrong-proxy-' + scheme, 'ranges.m3u8', success=False,
                         extra=['--proxy', proxy.url(), '--proxy-user', 'wrong', '--proxy-password', 'wrong'])
                    assert before == sum(len(s.requests) for s in servers)
                finally:
                    proxy.close()
            print('PASS selected authenticated HTTP/SOCKS5 route and wrong proxy without direct fallback', flush=True)

            # Verify TLS remains enforced before source HTTP credentials can be sent.
            key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
            subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'owned-live')])
            now = datetime.datetime.now(datetime.timezone.utc)
            cert = (x509.CertificateBuilder().subject_name(subject).issuer_name(subject).public_key(key.public_key())
                    .serial_number(x509.random_serial_number()).not_valid_before(now - datetime.timedelta(minutes=1))
                    .not_valid_after(now + datetime.timedelta(hours=1))
                    .add_extension(x509.SubjectAlternativeName([x509.IPAddress(ipaddress.ip_address('127.0.0.1'))]), False)
                    .sign(key, hashes.SHA256()))
            cert_path, key_path = root / 'cert.pem', root / 'key.pem'
            cert_path.write_bytes(cert.public_bytes(serialization.Encoding.PEM))
            key_path.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
            tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            tls.load_cert_chain(cert_path, key_path)
            untrusted = Origin(tls)
            servers.append(untrusted)
            source.files['tls.m3u8'] = manifest([untrusted.url('ts-0.ts')])
            environment['MASKPILOT_MEDIA_LIVE_ACCESS'] = json.dumps([dict(origin=untrusted.url(), cookie=CDN_COOKIE)])
            call('tls', 'tls.m3u8', success=False)
            assert not untrusted.requests
            print('PASS invalid TLS cannot receive source HTTP credentials', flush=True)
            assert all(row[1] for row in source.requests + cdn.requests + public.requests)
        finally:
            for server in servers:
                server.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--exercise', type=Path, required=True)
    parser.add_argument('--muxer', type=Path, required=True)
    options = parser.parse_args()
    exercise(options.exercise.resolve(), options.muxer.resolve())
