"""Actual supported parser/proxy/selection and Qt media system acceptance; no public sites."""
import argparse
import contextlib
import functools
import http.server
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'support'))
from proxy_fixture import RecordingProxy

USERNAME = 'owned media user@fixture'
PASSWORD = 'owned-media-private:#%?573'


class Origin(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, directory):
        self.directory = directory
        self.hits = []
        self.requests = []
        self.switch_after = None
        self.manifests = 0
        self.hold = False
        self.seen = threading.Event()
        self.release = threading.Event()
        super().__init__(('127.0.0.1', 0), functools.partial(Handler, directory=str(directory)))
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)
        self.thread.start()

    def url(self, name, host='127.0.0.1'):
        return f'http://{host}:{self.server_port}/{name}'

    def close(self):
        self.release.set()
        self.shutdown()
        self.server_close()
        self.thread.join(timeout=5)


class Handler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        path = self.path.split('?', 1)[0]
        self.server.requests.append(('GET', path))
        self.server.hits.append((path, bool(self.headers.get('Proxy-Authorization'))))
        if self.server.hold:
            self.server.seen.set()
            self.server.release.wait(timeout=12)
        if path == '/switch.m3u8':
            self.server.manifests += 1
            encrypted = self.server.switch_after is not None and self.server.manifests > self.server.switch_after
            key = '#EXT-X-KEY:METHOD=SAMPLE-AES,URI="key.bin"\n' if encrypted else ''
            body = ('#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:0\n'
                    + key + '#EXTINF:2,\nsegment.ts\n#EXT-X-ENDLIST\n').encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/vnd.apple.mpegurl')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            with contextlib.suppress(OSError):
                self.wfile.write(body)
            return
        with contextlib.suppress(OSError, ConnectionError):
            super().do_GET()

    def do_HEAD(self):
        self.server.requests.append(('HEAD', self.path.split('?', 1)[0]))
        super().do_HEAD()


def execute(arguments, *, environment=None, expected=0, timeout=65, cwd=None):
    result = subprocess.run(list(map(str, arguments)), env=environment, capture_output=True, timeout=timeout, cwd=cwd)
    assert result.returncode == expected, (result.returncode, result.stdout[-1200:], result.stderr[-1200:])
    return result


def assets(muxer, directory):
    base = [muxer, '-v', 'error', '-y', '-f', 'lavfi', '-i', 'testsrc=size=320x180:rate=10',
            '-f', 'lavfi', '-i', 'sine=frequency=700:sample_rate=48000', '-t', '2']
    execute(base + ['-map', '0:v', '-map', '0:v', '-map', '1:a', '-filter:v:0', 'scale=160:90',
                    '-filter:v:1', 'scale=320:180', '-c:v', 'libx264', '-threads', '1', '-pix_fmt', 'yuv420p',
                    '-b:v:0', '100k', '-b:v:1', '220k', '-c:a', 'aac', '-use_template', '1', '-use_timeline', '1',
                    '-seg_duration', '1', '-adaptation_sets', 'id=0,streams=v id=1,streams=a', '-f', 'dash',
                    'manifest.mpd'], cwd=directory)
    execute(base + ['-c:v', 'libx264', '-threads', '1', '-pix_fmt', 'yuv420p', '-c:a', 'aac',
                    directory / 'combined.mp4'])
    execute([muxer, '-v', 'error', '-y', '-i', directory / 'combined.mp4', '-c', 'copy', '-bsf:v',
             'h264_mp4toannexb', '-f', 'mpegts', directory / 'segment.ts'])
    execute([muxer, '-v', 'error', '-y', '-i', directory / 'combined.mp4', '-vf', 'scale=160:90',
             '-c:v', 'libx264', '-threads', '1', '-c:a', 'copy', directory / 'component-low.mp4'])
    execute([muxer, '-v', 'error', '-y', '-i', directory / 'combined.mp4', '-c:v', 'libvpx-vp9',
             '-threads', '1', '-deadline', 'realtime', '-cpu-used', '8', '-c:a', 'libopus',
             directory / 'combined.webm'], cwd=directory, timeout=25)
    webm_dash = directory / 'webm-dash'
    webm_dash.mkdir()
    execute([muxer, '-v', 'error', '-y', '-i', directory / 'combined.webm', '-c', 'copy',
             '-use_template', '1', '-use_timeline', '1', '-seg_duration', '1',
             '-adaptation_sets', 'id=0,streams=v id=1,streams=a', '-f', 'dash', 'manifest.mpd'],
            cwd=webm_dash, timeout=25)


def request(client, parser, muxer, root, url, *options, inspect=False, environment=None, expected=0,
            filename='selected.mkv'):
    root.mkdir(parents=True, exist_ok=True)
    destination = root / (filename if not inspect else 'inspection.mkv')
    command = [client, url, destination, root / 'data', '--media', '--media-parser', parser,
               '--media-muxer', muxer, *options]
    if inspect:
        command.append('--inspect-media')
    return execute(command, environment=environment, expected=expected, cwd=root)


def inspect_case(client, parser, muxer, root, origin, options=(), environment=None, host='127.0.0.1'):
    url = origin.url("manifest.mpd?private_query_marker=owned-853&label=O'Reilly", host)
    before = len(origin.hits)
    result = request(client, parser, muxer, root, url, *options, inspect=True, environment=environment)
    assert all(path == '/manifest.mpd' for path, auth in origin.hits[before:]), 'Inspection downloaded payload fragments'
    inventory = json.loads(result.stdout)
    assert set(inventory) == {'title', 'formats'}
    assert inventory['formats'] and len(inventory['formats']) <= 2000
    assert all(set(value) == {'id', 'extension', 'videoCodec', 'audioCodec', 'protocol', 'width', 'height',
                              'fps', 'bitrate', 'bytes', 'estimated', 'hasVideo', 'hasAudio'}
               for value in inventory['formats'])
    serialized = json.dumps(inventory)
    for private in (USERNAME, PASSWORD, 'private_query_marker', 'owned-853', '://'):
        assert private not in serialized, 'Inventory exposed a private request field'
    videos = [value for value in inventory['formats'] if value['hasVideo'] and not value['hasAudio']]
    audios = [value for value in inventory['formats'] if value['hasAudio'] and not value['hasVideo']]
    assert {value['height'] for value in videos} >= {90, 180} and audios
    chosen = min(videos, key=lambda value: value['height'])
    request(client, parser, muxer, root, url, *options, '--media-mode', 'video-audio', '--media-format-id',
            chosen['id'], '--media-audio-id', audios[0]['id'], '--media-container', 'mkv', environment=environment)
    probe = muxer.with_name('ffprobe.exe' if os.name == 'nt' else 'ffprobe')
    streams = json.loads(execute([probe, '-v', 'error', '-show_streams', '-of', 'json', root / 'selected.mkv']).stdout)['streams']
    assert any(value['codec_type'] == 'video' and value['width'] == 160 and value['height'] == 90 for value in streams)
    assert any(value['codec_type'] == 'audio' for value in streams)
    return inventory


def argv_case(client, parser, muxer, root, origin):
    if os.name != 'nt':
        raise AssertionError('This actual argv qualification requires the Windows test target')
    root.mkdir()
    origin.hold = True
    environment = os.environ.copy()
    environment.update(MASKPILOT_MEDIA_PROXY_USER=USERNAME, MASKPILOT_MEDIA_PROXY_PASS=PASSWORD)
    proxy = RecordingProxy('http', USERNAME, PASSWORD)
    url = origin.url('manifest.mpd?private_query_marker=owned-argv-164')
    command = [str(client), url, str(root / 'unused.mkv'), str(root / 'data'), '--media', '--inspect-media',
               '--media-parser', str(parser), '--media-muxer', str(muxer), '--proxy', proxy.url(),
               '--proxy-auth-required', '--proxy-user-env', 'MASKPILOT_MEDIA_PROXY_USER',
               '--proxy-password-env', 'MASKPILOT_MEDIA_PROXY_PASS']
    process = subprocess.Popen(command, env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        assert origin.seen.wait(timeout=15), 'Actual helper never reached the held proxy origin'
        script = (f'Get-CimInstance Win32_Process -Filter "ParentProcessId = {process.pid}" | '
                  'Select-Object ProcessId,CommandLine | ConvertTo-Json -Compress')
        snapshot = execute(['powershell', '-NoProfile', '-NonInteractive', '-Command', script], timeout=10)
        children = json.loads(snapshot.stdout.decode('utf-8-sig'))
        if isinstance(children, dict): children = [children]
        assert children, 'No actual owned helper argv was observed'
        arguments = json.dumps(children)
        for private in (USERNAME, PASSWORD, 'private_query_marker', 'owned-argv-164'):
            assert private not in arguments, 'Actual helper argv exposed private input'
        assert '--config-locations' in arguments
        origin.release.set()
        output, error = process.communicate(timeout=45)
        assert process.returncode == 0, error[-800:]
        assert json.loads(output)['formats']
        print('PASS actual held helper argv contains no private source/proxy input')
    finally:
        origin.hold = False
        origin.release.set()
        if process.poll() is None:
            process.kill(); process.communicate(timeout=10)
        proxy.close()


def extra_formats(client, parser, muxer, root, origin, inventory):
    video = min((value for value in inventory['formats'] if value['hasVideo'] and not value['hasAudio']),
                key=lambda value: value['height'])
    audio = next(value for value in inventory['formats'] if value['hasAudio'] and not value['hasVideo'])
    url = origin.url('manifest.mpd')
    request(client, parser, muxer, root, url, '--media-mode', 'video-audio', '--media-format-id', video['id'],
            '--media-audio-id', audio['id'], '--media-container', 'mp4', filename='selected.mp4')
    probe = muxer.with_name('ffprobe.exe' if os.name == 'nt' else 'ffprobe')
    result = json.loads(execute([probe, '-v', 'error', '-show_streams', '-show_format', '-of', 'json',
                                 root / 'selected.mp4']).stdout)
    assert 'mp4' in result['format']['format_name']
    for container, codec, format_name in (('m4a', 'aac', 'mov'), ('mp3', 'mp3', 'mp3'),
                                          ('opus', 'opus', 'ogg'), ('flac', 'flac', 'flac')):
        filename = 'selected.' + container
        request(client, parser, muxer, root, url, '--media-mode', 'audio', '--media-format-id', audio['id'],
                '--media-container', container, filename=filename)
        output = root / filename
        assert output.is_file() and output.suffix == '.' + container
        result = json.loads(execute([probe, '-v', 'error', '-show_streams', '-show_format', '-of', 'json',
                                     output], cwd=root, timeout=10).stdout)
        assert result['streams'] and all(value['codec_type'] == 'audio' and value['codec_name'] == codec
                                        for value in result['streams']), (container, result)
        assert format_name in result['format']['format_name'].split(','), (container, result['format'])
    before = len(origin.hits)
    request(client, parser, muxer, root / 'missing-choice', url, '--media-mode', 'video-audio',
            '--media-format-id', 'owned-missing-format', '--media-audio-id', audio['id'], expected=1)
    assert not (root / 'missing-choice' / 'selected.mkv').exists()
    assert all(path == '/manifest.mpd' for path, auth in origin.hits[before:]), 'A stale explicit choice fell back to payload'
    combined = json.loads(request(client, parser, muxer, root / 'combined', origin.url('combined.mp4'),
                                  inspect=True).stdout)
    chosen = next(value for value in combined['formats'] if value['hasVideo'] and value['hasAudio'])
    request(client, parser, muxer, root / 'combined', origin.url('combined.mp4'), '--media-mode', 'muxed',
            '--media-format-id', chosen['id'], '--media-container', 'mkv')
    webm_case = root / 'combined-webm'
    before_inventory = len(origin.requests)
    combined_webm = json.loads(request(client, parser, muxer, webm_case, origin.url('combined.webm'),
                                       inspect=True).stdout)
    inventory_requests = origin.requests[before_inventory:]
    chosen_webm = next(value for value in combined_webm['formats'] if value['hasVideo'] and value['hasAudio'])
    # Generic direct-file inventory cannot attest codecs; explicit WebM must fail closed.
    before = len(origin.requests)
    request(client, parser, muxer, webm_case, origin.url('combined.webm'), '--media-mode', 'muxed',
            '--media-format-id', chosen_webm['id'], '--media-container', 'webm', filename='selected.webm',
            expected=1)
    assert not list(webm_case.glob('selected.*'))
    assert origin.requests[before:] == inventory_requests, 'Unknown-codec choice fetched beyond fresh inventory'
    # DASH declares actual stream codecs, permitting a fresh compatible explicit pair.
    webm_case = root / 'webm-dash'
    webm_url = origin.url('webm-dash/manifest.mpd')
    webm_inventory = json.loads(request(client, parser, muxer, webm_case, webm_url, inspect=True).stdout)
    webm_video = next(value for value in webm_inventory['formats'] if value['hasVideo'] and not value['hasAudio'])
    webm_audio = next(value for value in webm_inventory['formats'] if value['hasAudio'] and not value['hasVideo'])
    assert webm_video['videoCodec'].startswith(('vp9', 'vp09')) and webm_audio['audioCodec'] == 'opus'
    before = len(origin.hits)
    request(client, parser, muxer, webm_case, webm_url, '--media-mode', 'video-audio',
            '--media-format-id', webm_video['id'], '--media-audio-id', webm_audio['id'],
            '--media-container', 'webm', filename='selected.webm')
    assert any(path.startswith('/webm-dash/') and path != '/webm-dash/manifest.mpd'
               for path, _ in origin.hits[before:]), 'Explicit WebM did not fetch real DASH fragments'
    webm = webm_case / 'selected.webm'
    result = json.loads(execute([probe, '-v', 'error', '-show_streams', '-show_format', '-of', 'json', webm],
                               cwd=webm_case, timeout=10).stdout)
    assert webm.is_file() and webm.suffix == '.webm'
    assert 'webm' in result['format']['format_name'].split(',')
    assert any(value['codec_type'] == 'video' and value['codec_name'] == 'vp9' for value in result['streams'])
    assert any(value['codec_type'] == 'audio' and value['codec_name'] == 'opus' for value in result['streams'])
    print('PASS actual MP4/WebM muxed and M4A/MP3/Opus/FLAC audio outputs; vanished ID rejects before payload')


def fallback_guard(client, parser, muxer, root, origin):
    case = root / 'hls-normal'
    origin.manifests = 0
    origin.switch_after = None
    request(client, parser, muxer, case, origin.url('switch.m3u8'))
    normal_requests = origin.manifests
    assert normal_requests >= 2 and (case / 'selected.mkv').is_file()
    proxy = RecordingProxy('http', USERNAME, PASSWORD)
    try:
        origin.manifests = 0
        origin.switch_after = normal_requests - 1
        before = len(origin.hits)
        environment = os.environ.copy()
        environment.update(MASKPILOT_MEDIA_PROXY_USER=USERNAME, MASKPILOT_MEDIA_PROXY_PASS=PASSWORD)
        request(client, parser, muxer, root / 'hls-unsupported', origin.url('switch.m3u8'),
                '--proxy', proxy.url(), '--proxy-auth-required', '--proxy-user-env', 'MASKPILOT_MEDIA_PROXY_USER',
                '--proxy-password-env', 'MASKPILOT_MEDIA_PROXY_PASS', environment=environment, expected=1)
        hits = origin.hits[before:]
        assert origin.manifests >= normal_requests and all(path == '/switch.m3u8' for path, auth in hits)
        assert len(proxy.snapshot()['destinations']) == len(hits), 'Unsupported HLS attempted direct origin access'
        before = len(origin.hits)
        blocked = subprocess.run([str(muxer), '-v', 'error', '-protocol_whitelist', 'pipe', '-i',
                                  origin.url('switch.m3u8'), '-t', '1', '-f', 'null', '-'],
                                 capture_output=True, timeout=10)
        assert blocked.returncode != 0 and b'not on whitelist' in blocked.stderr
        assert len(origin.hits) == before
        print('PASS native HLS/local remux; unsupported HLS has no direct fallback; actual FFmpeg input guard denies HTTP')
    finally:
        origin.switch_after = None
        proxy.close()


def exact_id_component(parser, muxer, root, origin):
    """Qualify the pinned parser's selector semantics, separately from the app path."""
    case = root / 'exact-id-component'
    case.mkdir()
    probe = muxer.with_name('ffprobe.exe' if os.name == 'nt' else 'ffprobe')
    for identifier in ('best', 'all', 'mp4', 'webm', '137'):
        info = case / (identifier + '.info.json')
        formats = []
        for format_id, name, width, height in ((identifier, 'component-low.mp4', 160, 90),
                                                ('other-better', 'combined.mp4', 320, 180)):
            formats.append({'format_id': format_id, 'url': origin.url(name), 'ext': 'mp4',
                            'protocol': 'http', 'vcodec': 'avc1', 'acodec': 'mp4a',
                            'width': width, 'height': height, 'tbr': height * 2})
        info.write_text(json.dumps({'id': 'owned-component', 'title': 'Owned component',
                                    'extractor': 'generic', 'webpage_url': origin.url('component-low.mp4'),
                                    'formats': formats}), encoding='utf-8')
        output = case / (identifier + '.mp4')
        published = json.loads(execute([parser, '--ignore-config', '--no-plugin-dirs', '--no-cache-dir',
                                       '--proxy', '', '--load-info-json', info, '--dump-single-json'],
                                      cwd=case, timeout=25).stdout)
        low = [value for value in published['formats'] if value['url'] == origin.url('component-low.mp4')]
        assert len(low) == 1, (identifier, 'Parser did not publish exactly one low-resolution format')
        published_id = low[0]['format_id']
        assert published_id == ('fwebm' if identifier == 'webm' else identifier), (identifier, published_id)
        before = len(origin.hits)
        try:
            execute([parser, '--ignore-config', '--no-plugin-dirs', '--no-cache-dir', '--proxy', '',
                     '--load-info-json', info, '--format', f"best*[format_id='{published_id}']",
                     '--output', output], cwd=case, timeout=25)
        except AssertionError as error:
            raise AssertionError(f'Exact-ID component raw={identifier}, published={published_id}: {error}') from error
        assert origin.hits[before:] and all(path == '/component-low.mp4'
                                           for path, _ in origin.hits[before:]), 'Exact selector fetched a different ID'
        streams = json.loads(execute([probe, '-v', 'error', '-show_streams', '-of', 'json', output],
                                     cwd=case, timeout=10).stdout)['streams']
        assert any(value['codec_type'] == 'video' and value['width'] == 160 and value['height'] == 90
                   for value in streams), f'Reserved format ID {identifier} selected another stream'
    print('PASS pinned parser component exact reserved/numeric IDs select the real low-resolution source')


def disguised_local_input_guard(client, parser, muxer, root, origin):
    """Whole-app no-publication/no-egress proof; does not infer which helper rejected input."""
    environment = os.environ.copy()
    environment.update(MASKPILOT_MEDIA_PROXY_USER=USERNAME, MASKPILOT_MEDIA_PROXY_PASS=PASSWORD)
    for kind in ('http', 'socks5'):
        proxy = RecordingProxy(kind, USERNAME, PASSWORD)
        try:
            options = ['--proxy', proxy.url(), '--proxy-auth-required', '--proxy-user-env',
                       'MASKPILOT_MEDIA_PROXY_USER', '--proxy-password-env', 'MASKPILOT_MEDIA_PROXY_PASS']
            for playlist in ('hls', 'concat'):
                name = f'disguised-{kind}-{playlist}.mp4'
                nested = f'/nested-{kind}-{playlist}.ts'
                # Keep nested endpoints reachable so the whole app must reject before contacting them.
                (origin.directory / nested.lstrip('/')).write_bytes((origin.directory / 'segment.ts').read_bytes())
                nested_url = origin.url(nested.lstrip('/'))
                body = ('#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXTINF:2,\n' + nested_url + '\n#EXT-X-ENDLIST\n'
                        if playlist == 'hls' else "ffconcat version 1.0\nfile '" + nested_url + "'\n")
                (origin.directory / name).write_text(body, encoding='ascii')
                for mode in ('audio', 'muxed'):
                    case = root / f'local-guard-{kind}-{playlist}-{mode}'
                    before = len(origin.requests)
                    result = request(client, parser, muxer, case, origin.url(name), *options,
                                     *(['--audio-only'] if mode == 'audio' else []),
                                     environment=environment, expected=1)
                    observed = origin.requests[before:]
                    assert ('GET', '/' + name) in observed, 'Deceptive source was never requested'
                    assert not any(path == nested for _, path in observed), 'Local media tool contacted a nested URL'
                    assert not list(case.glob('selected.*')), 'Rejected deceptive media was published'
                    assert PASSWORD.encode() not in result.stderr
                    assert proxy.snapshot()['auth_accepted'] > 0
            print(f'PASS actual {kind} authenticated deceptive local media rejects extraction/remux without nested requests')
        finally:
            proxy.close()


def local_guard_component(client, muxer, root, origin):
    """Directly qualify both probe input spellings and remux with real guarded tools."""
    case = root / 'local-guard-component'
    case.mkdir()
    suffix = '.exe' if os.name == 'nt' else ''
    guard_directory = client.parent / 'media_guard'
    guarded_probe = guard_directory / ('ffprobe' + suffix)
    guarded_muxer = guard_directory / ('ffmpeg' + suffix)
    assert guarded_probe.is_file() and guarded_muxer.is_file(), 'Owned media guards are missing'
    environment = os.environ.copy()
    environment.update(MASKPILOT_MEDIA_REAL_FFMPEG=str(muxer),
                       MASKPILOT_MEDIA_REAL_FFPROBE=str(muxer.with_name('ffprobe' + suffix)),
                       MASKPILOT_MEDIA_WORK_DIR=str(case))
    valid = case / 'valid.mp4'
    shutil.copyfile(origin.directory / 'combined.mp4', valid)
    for spelling in ([], ['-i']):
        response = execute([guarded_probe, '-v', 'error', '-show_streams', '-of', 'json', *spelling, valid],
                           environment=environment, cwd=case, timeout=10)
        assert json.loads(response.stdout)['streams']
    output = case / 'valid.mkv'
    execute([guarded_muxer, '-v', 'error', '-i', valid, '-c', 'copy', output],
            environment=environment, cwd=case, timeout=10)
    assert output.is_file() and output.stat().st_size > 0
    for playlist in ('hls', 'concat'):
        nested = '/guard-component-' + playlist + '.ts'
        (origin.directory / nested.lstrip('/')).write_bytes((origin.directory / 'segment.ts').read_bytes())
        nested_url = origin.url(nested.lstrip('/'))
        deceptive = case / (playlist + '.mp4')
        deceptive.write_text(('#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXTINF:2,\n' + nested_url + '\n#EXT-X-ENDLIST\n'
                              if playlist == 'hls' else "ffconcat version 1.0\nfile '" + nested_url + "'\n"),
                             encoding='ascii')
        rejected_output = case / (playlist + '-rejected.mkv')
        commands = [[guarded_probe, '-v', 'error', '-show_streams', '-of', 'json', deceptive],
                    [guarded_probe, '-v', 'error', '-show_streams', '-of', 'json', '-i', deceptive],
                    [guarded_muxer, '-v', 'error', '-i', deceptive, '-c', 'copy', rejected_output]]
        for command in commands:
            before = len(origin.requests)
            result = subprocess.run(list(map(str, command)), env=environment, cwd=case,
                                    capture_output=True, timeout=10)
            assert result.returncode != 0, 'Guard accepted a local playlist disguised as MP4'
            assert not any(path == nested for _, path in origin.requests[before:]), 'Guard contacted nested origin'
            assert not rejected_output.exists(), 'Guard published rejected local playlist'
    for tool in (guarded_probe, guarded_muxer):
        outside = origin.directory / 'combined.mp4'
        arguments = ([tool, '-v', 'error', outside] if tool == guarded_probe else
                     [tool, '-v', 'error', '-i', outside, '-c', 'copy', case / 'outside.mkv'])
        result = subprocess.run(list(map(str, arguments)), env=environment, cwd=case,
                                capture_output=True, timeout=10)
        assert result.returncode != 0 and b'Local media processing was rejected.' in result.stderr
        assert not (case / 'outside.mkv').exists()
    print('PASS actual guard components accept local containers and deny disguised playlists/out-of-work inputs')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exercise', type=Path, required=True)
    parser.add_argument('--parser', type=Path, required=True)
    parser.add_argument('--muxer', type=Path, required=True)
    parser.add_argument('--ui', type=Path, required=True)
    args = parser.parse_args()
    client, tool, muxer, ui = [path.resolve(strict=True) for path in (args.exercise, args.parser, args.muxer, args.ui)]
    with tempfile.TemporaryDirectory(prefix='maskpilot-media-inventory-') as temporary, contextlib.ExitStack() as stack:
        root = Path(temporary)
        media = root / 'assets'; media.mkdir()
        assets(muxer, media)
        origin = Origin(media); stack.callback(origin.close)
        direct = root / 'direct'; direct.mkdir()
        inventory = inspect_case(client, tool, muxer, direct, origin)
        print('PASS actual DASH inventory has two real video resolutions and selected audio/video output')
        extra_formats(client, tool, muxer, direct, origin, inventory)
        fallback_guard(client, tool, muxer, root, origin)
        exact_id_component(tool, muxer, root, origin)
        disguised_local_input_guard(client, tool, muxer, root, origin)
        local_guard_component(client, muxer, root, origin)
        for kind in ('http', 'socks5'):
            case = root / kind; case.mkdir()
            proxy = RecordingProxy(kind, USERNAME, PASSWORD, mappings={'media-fixture.invalid': '127.0.0.1'})
            try:
                environment = os.environ.copy()
                environment.update(MASKPILOT_MEDIA_PROXY_USER=USERNAME, MASKPILOT_MEDIA_PROXY_PASS=PASSWORD)
                options = ['--proxy', proxy.url(), '--proxy-auth-required', '--proxy-user-env',
                           'MASKPILOT_MEDIA_PROXY_USER', '--proxy-password-env', 'MASKPILOT_MEDIA_PROXY_PASS']
                inspect_case(client, tool, muxer, case, origin, options, environment, 'media-fixture.invalid')
                state = proxy.snapshot()
                assert state['auth_accepted'] > 0 and state['destinations']
                assert all(value['host'] == 'media-fixture.invalid' for value in state['destinations'])
                assert not any(auth for path, auth in origin.hits), 'Proxy credentials reached the origin'
                print(f'PASS actual {kind} authenticated media inventory/download, proxy-side DNS and local merge')
                before = len(origin.hits)
                environment['MASKPILOT_MEDIA_PROXY_PASS'] = 'owned-wrong-password'
                wrong = request(client, tool, muxer, root / (kind + '-wrong'), origin.url('manifest.mpd'),
                                *options, inspect=True, environment=environment, expected=1)
                assert len(origin.hits) == before and PASSWORD.encode() not in wrong.stderr
                print(f'PASS actual {kind} wrong authentication rejects without direct origin fallback')
            finally:
                proxy.close()
        argv_case(client, tool, muxer, root / 'argv', origin)
        environment = os.environ.copy()
        environment.update(MASKPILOT_MEDIA_TEST_PARSER=str(tool), MASKPILOT_MEDIA_TEST_MUXER=str(muxer))
        system_result = execute([ui], environment=environment, timeout=100)
        assert b'PASS actual Qt inventory' in system_result.stdout
        print('PASS actual Qt system selection and final stream verification')


if __name__ == '__main__':
    main()
