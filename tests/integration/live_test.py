"""Actual owned HLS/FLV recording through the native CLI; no external source or account."""
import argparse
import contextlib
import hashlib
import http.server
import json
import os
from pathlib import Path
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'support'))
from proxy_fixture import RecordingProxy

COOKIE = 'session=owned-live-private-942'
AUTH = 'Bearer owned-live-auth-852'


class Origin(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, root):
        self.root = root
        self.requests = []
        self.polls = {}
        self.foreign = None
        super().__init__(('127.0.0.1', 0), Handler)
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)
        self.thread.start()

    def url(self, name):
        return f'http://127.0.0.1:{self.server_port}/{name}'

    def close(self):
        self.shutdown()
        self.server_close()
        self.thread.join(5)


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, body, status=200, location=None, slow=False, truncated=False):
        self.send_response(status)
        self.send_header('Content-Length', str(len(body) + (100 if truncated else 0)))
        if location:
            self.send_header('Location', location)
        self.end_headers()
        with contextlib.suppress(OSError):
            for i in range(0, len(body), 2048):
                self.wfile.write(body[i:i + 2048])
                self.wfile.flush()
                if slow:
                    time.sleep(.02)

    def do_GET(self):
        self.server.requests.append((self.path, dict(self.headers)))
        name = self.path.lstrip('/').split('?')[0]
        if name.startswith('auth') and (self.headers.get('Cookie') != COOKIE or self.headers.get('Authorization') != AUTH):
            return self.reply(b'', 403)
        if name == 'foreign.m3u8':
            return self.reply(b'', 302, self.server.foreign.url('stolen.m3u8'))
        if name == 'redirect.m3u8':
            return self.reply(b'', 302, '/finite.m3u8')
        if name == 'master.m3u8':
            return self.reply(b'#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=900000\nfinite.m3u8\n')
        if name in ('encrypted.m3u8', 'map.m3u8', 'discontinuity.m3u8', 'cross-segment.m3u8'):
            extra = {'encrypted.m3u8': '#EXT-X-KEY:METHOD=AES-128,URI="secret.key"\n',
                     'map.m3u8': '#EXT-X-MAP:URI="init.mp4"\n',
                     'discontinuity.m3u8': '#EXT-X-DISCONTINUITY\n'}.get(name, '')
            segment = self.server.foreign.url('stolen.ts') if name == 'cross-segment.m3u8' else 'segment-0.ts'
            return self.reply(('#EXTM3U\n#EXT-X-TARGETDURATION:1\n' + extra + '#EXTINF:1,\n' + segment + '\n').encode())
        if name.endswith('.m3u8'):
            count = self.server.polls.get(name, 0)
            self.server.polls[name] = count + 1
            rolling = name in ('rolling.m3u8', 'gap.m3u8', 'auth-rolling.m3u8', 'forever.m3u8')
            seq = min(count, 5) if rolling else 0
            if name == 'gap.m3u8' and count:
                seq = 5
            indices = list(range(seq, seq + 3)) if rolling else list(range(8))
            lines = ['#EXTM3U', '#EXT-X-TARGETDURATION:1', f'#EXT-X-MEDIA-SEQUENCE:{seq}']
            for index in indices:
                lines += ['#EXTINF:1,', f'{"auth-" if name.startswith("auth") else ""}segment-{index}.ts']
            if not rolling or (count >= 5 and name != 'forever.m3u8'):
                lines += ['#EXT-X-ENDLIST']
            return self.reply(('\n'.join(lines) + '\n').encode())
        if re.fullmatch(r'(auth-)?segment-[0-7]\.ts', name):
            return self.reply((self.server.root / name.removeprefix('auth-')).read_bytes())
        if name in ('feed.flv', 'slow.flv', 'bad.flv', 'truncated.flv'):
            data = (self.server.root / 'feed.flv').read_bytes()
            if name == 'bad.flv':
                data = data[:-1] + bytes([data[-1] ^ 1])
            return self.reply(data, slow=name == 'slow.flv', truncated=name == 'truncated.flv')
        self.reply(b'', 404)


def exercise(executable, scanner):
    ffmpeg, ffprobe = shutil.which('ffmpeg'), shutil.which('ffprobe')
    assert ffmpeg and ffprobe, 'Actual playable live fixtures require ffmpeg and ffprobe on the test PATH.'
    with tempfile.TemporaryDirectory(prefix='maskpilot-live-') as temporary:
        root = Path(temporary)
        subprocess.run([ffmpeg, '-nostdin', '-hide_banner', '-loglevel', 'error', '-f', 'lavfi', '-i',
                        'testsrc2=size=160x96:rate=10', '-t', '8', '-c:v', 'mpeg2video', '-g', '10',
                        '-sc_threshold', '0', '-hls_time', '1', '-hls_list_size', '0',
                        '-hls_segment_filename', str(root / 'segment-%d.ts'), str(root / 'fixture.m3u8')], check=True)
        subprocess.run([ffmpeg, '-nostdin', '-hide_banner', '-loglevel', 'error', '-f', 'lavfi', '-i',
                        'testsrc2=size=320x192:rate=20', '-t', '15', '-c:v', 'flv', '-b:v', '1000k',
                        '-f', 'flv', str(root / 'feed.flv')], check=True)
        expected_ts = b''.join((root / f'segment-{i}.ts').read_bytes() for i in range(8))
        expected_flv = (root / 'feed.flv').read_bytes()
        assert len(expected_flv) > 1048576
        origin, foreign = Origin(root), Origin(root)
        origin.foreign = foreign
        environment = dict(os.environ, MASKPILOT_LIVE_COOKIE=COOKIE, MASKPILOT_LIVE_AUTH=AUTH)

        def play(output):
            result = subprocess.run([ffprobe, '-v', 'error', '-show_entries', 'stream=codec_type',
                                     '-of', 'json', str(output)], capture_output=True, timeout=20)
            assert result.returncode == 0 and json.loads(result.stdout).get('streams'), result.stderr

        def run(name, source, extra=(), good=True, options=(), identifier=None):
            data = root / name
            suffix = '.flv' if '.flv' in source else '.ts'
            output = root / (name + suffix)
            command = [str(executable), *options]
            command += ['--resume', identifier, str(data)] if identifier else [source, str(output), str(data)]
            if not identifier:
                command += ['--live-flv' if suffix == '.flv' else '--live-hls']
            command += list(extra)
            result = subprocess.run(command, capture_output=True, env=environment, timeout=45)
            assert (result.returncode == 0) == good, (name, result.returncode, result.stdout[-1500:], result.stderr[-1500:])
            assert COOKIE.encode() not in result.stdout + result.stderr and AUTH.encode() not in result.stdout + result.stderr
            for file in data.rglob('*'):
                if file.is_file():
                    content = file.read_bytes()
                    assert COOKIE.encode() not in content and AUTH.encode() not in content, (name, file)
            if good:
                assert output.is_file() and 0 < output.stat().st_size
                play(output)
            else:
                assert not output.exists(), (name, 'unexpected publication')
            found = re.findall(rb'TASK_ID ([a-f0-9]{32})', result.stdout)
            return output, data, found[0].decode() if found else None

        try:
            for name in ('finite', 'rolling', 'redirect', 'master'):
                output, _, _ = run(name, origin.url(name + '.m3u8'))
                assert output.read_bytes() == expected_ts, (name, len(output.read_bytes()), len(expected_ts))
            output, _, _ = run('flv-exact', origin.url('feed.flv'), ['--sha256', hashlib.sha256(expected_flv).hexdigest()])
            assert output.read_bytes() == expected_flv
            print('PASS real rolling HLS refresh/dedup, finite/master/relative redirect and complete playable TS/FLV exact bytes', flush=True)

            start = time.monotonic()
            output, _, _ = run('duration', origin.url('slow.flv'), ['--live-duration', '1'])
            assert time.monotonic() - start < 8 and output.stat().st_size < len(expected_flv)
            output, _, _ = run('size', origin.url('feed.flv'), ['--live-max-bytes', '1048576'])
            assert output.stat().st_size <= 1048576 and output.stat().st_size < len(expected_flv)
            output, _, _ = run('hls-duration', origin.url('forever.m3u8'), ['--live-duration', '2'])
            assert output.stat().st_size < len(expected_ts)
            print('PASS duration and byte limits save complete playable media units without publishing partial tags/segments', flush=True)

            for source in ('encrypted.m3u8', 'map.m3u8', 'discontinuity.m3u8', 'gap.m3u8',
                           'cross-segment.m3u8', 'foreign.m3u8', 'bad.flv', 'truncated.flv'):
                run('refuse-' + source.replace('.', '-'), origin.url(source), good=False)
            assert not foreign.requests
            assert not any('secret.key' in name or 'init.mp4' in name for name, _ in origin.requests)
            run('wrong-hash', origin.url('finite.m3u8'), ['--sha256', '0' * 64], good=False)
            print('PASS encryption/init/discontinuity/gap/cross-origin before contact, malformed/truncated FLV and digest failure never publish', flush=True)

            auth = ['--source-cookie-env', 'MASKPILOT_LIVE_COOKIE', '--source-authorization-env', 'MASKPILOT_LIVE_AUTH']
            output, data, identifier = run('login', origin.url('auth-rolling.m3u8'), auth)
            assert output.read_bytes() == expected_ts
            # A separate interrupted process leaves a durable auth requirement and must not reconnect anonymously.
            output = root / 'restart.ts'
            data = root / 'restart'
            origin.polls['auth-rolling.m3u8'] = 0
            process = subprocess.Popen([str(executable), origin.url('auth-rolling.m3u8'), str(output), str(data),
                                        '--live-hls', *auth], stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=environment)
            try:
                line = process.stdout.readline()
                identifier = re.search(rb'TASK_ID ([a-f0-9]{32})', line).group(1).decode()
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline:
                    raw = data / 'parts' / identifier / 'live-recording.tmp'
                    if raw.exists() and raw.stat().st_size > 0:
                        break
                    time.sleep(.03)
                assert raw.exists() and raw.stat().st_size > 0
                process.kill()
                process.communicate(timeout=10)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate(timeout=10)
            before = len(origin.requests)
            run('restart', origin.url('auth-rolling.m3u8'), identifier=identifier, good=False)
            assert len(origin.requests) == before
            run('restart', origin.url('auth-rolling.m3u8'), auth, identifier=identifier)
            print('PASS same-origin login on playlists/segments, secrets absent from data/logs and killed-process restart requires fresh credentials', flush=True)

            for scheme in ('http', 'socks5'):
                proxy = RecordingProxy(scheme, 'route-user', 'route-pass', mappings={'live.fixture.invalid': '127.0.0.1'})
                try:
                    url = origin.url('finite.m3u8').replace('127.0.0.1', 'live.fixture.invalid')
                    args = ['--proxy', proxy.url(), '--proxy-user', 'route-user', '--proxy-password', 'route-pass']
                    run('proxy-' + scheme, url, args)
                    assert proxy.snapshot()['auth_accepted'] > 0 and proxy.snapshot()['destinations']
                finally:
                    proxy.close()
            # Native traffic accounting is durable; a live stop discards publication, not spent credits.
            import native_budget_test as budget
            cap = 257 * 1024 + 3
            data = root / 'quota'
            output = root / 'quota.flv'
            budget.seed(executable, data, cap)
            result = subprocess.run([str(executable), origin.url('feed.flv'), str(output), str(data),
                                     '--live-flv'], capture_output=True, timeout=30)
            assert result.returncode == 4, (result.returncode, result.stderr)
            identifier = budget.identity(result)
            assert not output.exists() and budget.usage(data) == (cap, cap), budget.usage(data)
            before = len(origin.requests)
            budget.call(executable, '--resume', identifier, data, expected=4)
            assert len(origin.requests) == before and budget.usage(data) == (cap, cap)
            budget.policy(data, len(expected_flv) * 3)
            budget.call(executable, '--resume', identifier, data, expected=0)
            assert output.read_bytes() == expected_flv
            assert budget.usage(data) == (cap + len(expected_flv), cap + len(expected_flv))
            assert not (data / 'parts' / identifier / 'live-recording.tmp').exists()
            print('PASS exact durable live payload quota, no anonymous/direct contact or refund on restart, fresh recording after policy repair and owned cache cleanup', flush=True)
            if scanner:
                for mode in ('pass', 'reject'):
                    name = 'scan-' + mode
                    output = root / (name + '.ts')
                    report = root / (name + '.json')
                    options = ['--scan-program', str(scanner)]
                    for value in ('--mode', mode, '--record', str(report), '--final', str(output), '{file}'):
                        options += ['--scan-arg', value]
                    run(name, origin.url('finite.m3u8'), good=mode == 'pass', options=options)
                    assert not json.loads(report.read_text())['finalExists']
            print('PASS selected authenticated HTTP/SOCKS5 proxy DNS and actual completion scanner gate', flush=True)
        finally:
            origin.close()
            foreign.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--exercise', type=Path, required=True)
    parser.add_argument('--scanner', type=Path)
    arguments = parser.parse_args()
    exercise(arguments.exercise.resolve(), arguments.scanner.resolve() if arguments.scanner else None)
