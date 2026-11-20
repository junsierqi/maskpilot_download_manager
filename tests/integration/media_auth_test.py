"""Actual owned media authentication and origin isolation; no external sites or user profiles."""
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

from media_inventory_test import assets
from proxy_fixture import RecordingProxy

COOKIE = 'owned_session=media-secret-731'
AUTH = 'Bearer owned-media-auth-947'


class Origin(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, directory):
        self.requests = []
        self.foreign = None
        super().__init__(('127.0.0.1', 0), functools.partial(Handler, directory=str(directory)))
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)
        self.thread.start()

    def url(self, path, host='127.0.0.1'):
        return f'http://{host}:{self.server_port}/{path}'

    def close(self):
        self.shutdown()
        self.server_close()
        self.thread.join(timeout=5)


class Handler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def authorized(self):
        valid = self.headers.get('Cookie') == COOKIE and self.headers.get('Authorization') == AUTH
        self.server.requests.append((self.path, valid, bool(self.headers.get('Proxy-Authorization'))))
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
        if self.path.startswith('/foreign-redirect'):
            self.send_response(302)
            self.send_header('Location', self.server.foreign.url('combined.mp4'))
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        if self.path.startswith('/foreign.m3u8'):
            body = ('#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n'
                    '#EXTINF:2,\n' + self.server.foreign.url('segment.ts') + '\n#EXT-X-ENDLIST\n').encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/vnd.apple.mpegurl')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        with contextlib.suppress(OSError):
            super().do_GET()


def call(client, parser, muxer, case, url, *options, inspect=False, authenticated=True, expected=True,
         credentials=None):
    case.mkdir(parents=True, exist_ok=True)
    output = case / 'output.mkv'
    cookie, authorization = credentials if credentials is not None else (
        COOKIE if authenticated else '', AUTH if authenticated else '')
    command = [str(client), url, str(output), str(case / 'data'), '1', '', '', cookie, authorization,
               '--media', '--media-parser', str(parser), '--media-muxer', str(muxer), *map(str, options)]
    if inspect:
        command.append('--inspect-media')
    result = subprocess.run(command, capture_output=True, timeout=100)
    assert (result.returncode == 0) == expected, (result.returncode, result.stdout[-1200:], result.stderr[-1200:])
    for private in (COOKIE.encode(), AUTH.encode()):
        assert private not in result.stdout + result.stderr
    if not inspect and expected:
        probe = muxer.with_name('ffprobe.exe' if os.name == 'nt' else 'ffprobe')
        answer = subprocess.run([str(probe), '-v', 'error', '-show_streams', '-of', 'json', str(output)],
                                capture_output=True, timeout=15)
        assert answer.returncode == 0, answer.stderr
        streams = json.loads(answer.stdout)['streams']
        assert any(s['codec_type'] == 'audio' for s in streams)
        assert any(s['codec_type'] == 'video' for s in streams)
        match = re.search(rb'TASK_ID ([a-f0-9]{32})', result.stdout)
        assert match
        data = case / 'data'
        with contextlib.closing(sqlite3.connect(data / 'tasks.db')) as db:
            row = db.execute('SELECT state,origin_auth_required FROM tasks WHERE id=?',
                             (match[1].decode(),)).fetchone()
            assert row == (9, 1), row
        for file in data.rglob('*'):
            if file.is_file():
                content = file.read_bytes()
                assert COOKIE.encode() not in content and AUTH.encode() not in content, file.name
    elif not inspect:
        assert not output.exists()
    return result


def exercise(client, parser, muxer):
    with tempfile.TemporaryDirectory(prefix='maskpilot-authenticated-media-') as name:
        root = Path(name)
        media = root / 'assets'
        media.mkdir()
        assets(muxer, media)
        (media / 'playlist.m3u8').write_text('#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n'
                                           '#EXTINF:2,\nsegment.ts\n#EXT-X-ENDLIST\n', encoding='utf-8')
        source, foreign = Origin(media), Origin(media)
        source.foreign = foreign
        try:
            for resource in ('combined.mp4', 'playlist.m3u8', 'manifest.mpd'):
                case = root / '媒体输出' / resource.replace('.', '-')
                inventory = json.loads(call(client, parser, muxer, case / 'inspect', source.url(resource), inspect=True).stdout)
                assert inventory['formats']
                call(client, parser, muxer, case / 'download', source.url(resource))
                print('PASS actual authenticated inspection/download and audio/video output:', resource, flush=True)
            assert source.requests and all(valid and not proxy for _, valid, proxy in source.requests)
            rejected = root / 'wrong-source-login'
            failed = call(client, parser, muxer, rejected, source.url('playlist.m3u8'),
                          credentials=('owned_session=wrong', AUTH), expected=False)
            match = re.search(rb'TASK_ID ([a-f0-9]{32})', failed.stdout)
            assert match
            before = len(source.requests)
            resume = subprocess.run([str(client), '--resume', match[1].decode(), str(rejected / 'data'),
                                     '--media-parser', str(parser), '--media-muxer', str(muxer)],
                                    capture_output=True, timeout=25)
            assert resume.returncode != 0 and len(source.requests) == before
            with contextlib.closing(sqlite3.connect(rejected / 'data' / 'tasks.db')) as db:
                assert db.execute('SELECT origin_auth_required FROM tasks').fetchone() == (1,)
            print('PASS source login rejection never publishes; restart without credentials refuses before origin', flush=True)
            for path in ('foreign-redirect', 'foreign.m3u8'):
                call(client, parser, muxer, root / path, source.url(path), expected=False)
                assert not foreign.requests, 'Cross-origin helper request reached an unapproved origin'
            print('PASS authenticated redirect and HLS fragment cannot contact a different origin', flush=True)
            for kind in ('http', 'socks5'):
                proxy = RecordingProxy(kind, 'owned-route-user', 'owned-route-password',
                                       mappings={'media.invalid': '127.0.0.1'})
                try:
                    options = ['--proxy', proxy.url(), '--proxy-user', 'owned-route-user',
                               '--proxy-password', 'owned-route-password']
                    call(client, parser, muxer, root / kind, source.url('playlist.m3u8', 'media.invalid'), *options)
                    report = proxy.snapshot()
                    assert report['auth_accepted'] > 0 and report['destinations']
                    assert all(item['host'] == 'media.invalid' and item['port'] == source.server_port
                               for item in report['destinations'])
                    before = len(source.requests)
                    call(client, parser, muxer, root / (kind + '-wrong'),
                         source.url('playlist.m3u8', 'media.invalid'),
                         '--proxy', proxy.url(), '--proxy-user', 'owned-route-user',
                         '--proxy-password', 'wrong', expected=False)
                    assert len(source.requests) == before
                    print('PASS authenticated media retains', kind, 'proxy-side DNS and login; wrong route never falls back', flush=True)
                finally:
                    proxy.close()
            assert not foreign.requests
            assert all(not proxy for _, _, proxy in source.requests)
        finally:
            source.close()
            foreign.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--exercise', type=Path, required=True)
    parser.add_argument('--parser', type=Path, required=True)
    parser.add_argument('--muxer', type=Path, required=True)
    options = parser.parse_args()
    exercise(options.exercise.resolve(), options.parser.resolve(), options.muxer.resolve())
