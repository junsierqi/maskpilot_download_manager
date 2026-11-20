"""Actual framed native-host/broker isolation through one owned desktop and real proxy routes."""
import argparse
import contextlib
import http.server
import json
import os
from pathlib import Path
import re
import shutil
import sqlite3
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'support'))
from proxy_fixture import RecordingProxy

ORIGIN = 'chrome-extension://epdfgbhpbpkpephcfmemmadlblpodnfo/'
DATA = bytes(range(256)) * 8193
USERNAME = 'context-proxy-user-private-471'
PASSWORD = 'context-proxy-password-private-965'
COOKIES = {'a': 'context_cookie=origin-private-A-158', 'b': 'context_cookie=origin-private-B-396'}


def opaque(value):
    return isinstance(value, str) and re.fullmatch('[a-f0-9]{32}', value) is not None


def frame(value):
    body = json.dumps(value, ensure_ascii=False).encode('utf-8')
    return struct.pack('<I', len(body)) + body


class OriginServer(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self):
        self.hits = []
        self.lock = threading.Lock()
        super().__init__(('127.0.0.1', 0), OriginHandler)
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)
        self.thread.start()

    def url(self, owner, path='/file'):
        return f'http://127.0.0.1:{self.server_port}{path}?owner={owner}'

    def snapshot(self):
        with self.lock:
            return list(self.hits)

    def close(self):
        self.shutdown()
        self.server_close()
        self.thread.join(timeout=5)


class OriginHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def do_GET(self):
        parsed = urllib.parse.urlsplit(self.path)
        owner = urllib.parse.parse_qs(parsed.query).get('owner', [''])[0]
        correct = self.headers.get('Cookie') == COOKIES.get(owner)
        with self.server.lock:
            self.server.hits.append((owner, parsed.path, correct,
                                     bool(self.headers.get('Proxy-Authorization'))))
        if not correct and parsed.path != '/legacy':
            self.send_response(403)
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        if parsed.path == '/redirect':
            self.send_response(302)
            self.send_header('Location', self.server.url(owner, '/redirect-target'))
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
        self.send_header('ETag', '"context-native-v1"')
        self.send_header('Content-Length', str(end - begin + 1))
        if match:
            self.send_header('Content-Range', f'bytes {begin}-{end}/{len(DATA)}')
        self.end_headers()
        try:
            for offset in range(begin, end + 1, 16384):
                self.wfile.write(DATA[offset:min(offset + 16384, end + 1)])
                if parsed.path == '/slow':
                    self.wfile.flush()
                    time.sleep(.12)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass


class Harness:
    def __init__(self, host, desktop, root):
        self.desktop, self.root = desktop, root
        self.data, self.downloads = root / 'data', root / 'downloads'
        self.downloads.mkdir()
        (self.data / 'MaskPilot').mkdir(parents=True)
        (self.data / 'MaskPilot' / 'MaskPilot.ini').write_text(
            '[General]\ndownloadDirectory=' + self.downloads.as_posix() + '\nlanguage=en\n', encoding='utf-8')
        isolated = root / 'host'
        isolated.mkdir()
        self.host = isolated / host.name
        shutil.copy2(host, self.host)
        (isolated / 'native-bridge.json').write_text(json.dumps({
            'allowedOrigins': [ORIGIN], 'desktopPath': str(desktop), 'dataDirectory': str(self.data)}), encoding='utf-8')
        self.environment = os.environ.copy()
        self.environment.update(QT_QPA_PLATFORM='offscreen', NO_PROXY='*', no_proxy='*')
        self.environment['PATH'] = str(host.parent) + os.pathsep + self.environment.get('PATH', '')
        self.flags = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
        self.process = None
        self.outputs = []

    def start(self):
        assert self.process is None
        self.process = subprocess.Popen([str(self.desktop), '--background', '--data-dir', str(self.data)],
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                        env=self.environment, creationflags=self.flags)
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            assert self.process.poll() is None, 'Owned desktop exited before readiness'
            if (self.data / 'ipc.token').exists() and (self.data / 'ipc-admin.token').exists():
                # Files may be leftovers after restart; ping proves the new actor is accepting frames.
                result = self.legacy('ping')
                if result.get('ok'):
                    break
            time.sleep(.05)
        else:
            raise AssertionError('Owned desktop did not become ready')
        legacy = (self.data / 'ipc.token').read_bytes().strip()
        admin = (self.data / 'ipc-admin.token').read_bytes().strip()
        assert len(legacy) == len(admin) == 64 and legacy != admin

    def stop(self):
        if self.process is None:
            return
        self.process.terminate()
        try:
            output, error = self.process.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.kill()
            output, error = self.process.communicate(timeout=10)
        self.outputs.append(output + error)
        self.process = None

    def query(self, request, broker=False, origin=ORIGIN):
        command = [str(self.host), '--broker', '--data-dir', str(self.data)] if broker else [str(self.host), origin]
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, env=self.environment, creationflags=self.flags)
        try:
            output, error = process.communicate(frame(request), timeout=25 if broker else 15)
        except BaseException:
            process.kill()
            process.communicate(timeout=5)
            raise
        self.outputs.append(error)
        assert len(output) >= 4, 'Native host omitted its framed response'
        length = struct.unpack('<I', output[:4])[0]
        assert 0 < length <= 1024 * 1024 and len(output) == length + 4, 'Invalid native response framing'
        answer = json.loads(output[4:])
        assert isinstance(answer, dict) and answer.get('requestId') == request.get('requestId', '')
        if request.get('version') in (1, 2):
            assert answer.get('version') == request['version'], 'Native response changed protocol version'
        else:
            assert answer.get('version') in (1, 2), 'Malformed-version response has no supported protocol envelope'
        assert isinstance(answer.get('ok'), bool)
        assert (process.returncode == 0) == answer['ok'], 'Native exit status disagrees with acknowledgement'
        for secret in (USERNAME, PASSWORD, *COOKIES.values()):
            assert secret not in json.dumps(answer), 'Public native response leaked a credential'
        return answer

    def admin(self, command, params=None, expected=True):
        response = self.query(dict(version=2, requestId=uuid.uuid4().hex, command=command, params=params or {}), True)
        assert response['ok'] == expected, 'Administrative command expectation failed: ' + command
        return response

    def scoped(self, token, command, params=None, expected=True, request_id=None, **extra):
        response = self.query(dict(version=2, requestId=request_id or uuid.uuid4().hex, command=command,
                                   params=params or {}, sessionToken=token, **extra))
        assert response['ok'] == expected, 'Scoped command expectation failed: ' + command
        return response

    def legacy(self, command, expected=True, **fields):
        response = self.query(dict(version=1, requestId=uuid.uuid4().hex, command=command, **fields))
        assert response['ok'] == expected, 'Legacy command expectation failed: ' + command
        return response

    def row(self, task_id):
        with contextlib.closing(sqlite3.connect((self.data / 'tasks.db').as_uri() + '?mode=ro', uri=True)) as database:
            return database.execute('SELECT state,downloaded,destination,owner_id,request_id,context_revision '
                                    'FROM tasks WHERE id=?', (task_id,)).fetchone()

    def wait(self, task_id, states, timeout=15):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            row = self.row(task_id)
            if row and row[0] in states:
                return row
            time.sleep(.05)
        raise AssertionError('Task failed to reach expected durable state')


def run(host, desktop):
    with tempfile.TemporaryDirectory(prefix='maskpilot-context-bridge-') as temporary, contextlib.ExitStack() as stack:
        root = Path(temporary).resolve()
        origin = OriginServer()
        stack.callback(origin.close)
        routes = {key: RecordingProxy(kind, USERNAME, PASSWORD, root / f'proxy-{key}.json')
                  for key, kind in [('a', 'http'), ('b', 'socks5')]}
        for route in routes.values():
            stack.callback(route.close)
        harness = Harness(host, desktop, root)
        stack.callback(harness.stop)
        harness.start()

        # A second coordinator must not open the desktop's database or shared part files.
        cli = Path(desktop).with_name('maskpilot_cli.exe' if os.name == 'nt' else 'maskpilot_cli')
        destination = root / 'second-coordinator.bin'
        rejected = subprocess.run([str(cli), origin.url('cli', '/legacy'), str(destination),
                                   str(harness.data)], capture_output=True, timeout=10)
        assert rejected.returncode != 0 and b'already in use' in rejected.stderr
        assert b'TASK_ID' not in rejected.stdout and not destination.exists()
        assert not origin.snapshot(), 'Rejected coordinator contacted the origin'
        with contextlib.closing(sqlite3.connect(harness.data / 'tasks.db')) as database:
            assert database.execute('SELECT count(*) FROM tasks').fetchone()[0] == 0

        def configure(key, current=None, **changes):
            params = dict(externalKey='trusted-fixture/' + key, label='Context ' + key,
                          downloadRoot=str(root / ('context-' + key)), proxyEndpoint=routes[key].url(),
                          proxyAuthenticationRequired=True, originAuthenticationRequired=True,
                          rememberProxy=False, proxyCredentials=dict(username=USERNAME, password=PASSWORD))
            if current:
                params.update(id=current['id'], expectedRevision=current['revision'])
            params.update(changes)
            context = harness.admin('context.configure', params)['context']
            assert opaque(context['id']) and context['enabled'] and not context['hasStoredCredentials']
            assert 'proxyCredentialRef' not in context and 'ownershipNonce' not in context
            return context

        contexts = {key: configure(key) for key in routes}

        def issue(key):
            token = harness.admin('context.issue', {'contextId': contexts[key]['id']})['sessionToken']
            assert opaque(token)
            return token

        def grant(key, url, expires=None):
            response = harness.admin('origin.grant', dict(contextId=contexts[key]['id'], url=url,
                                                          cookie=COOKIES[key], expiresAt=expires or int(time.time()) + 120))
            assert opaque(response['originGrantId'])
            return response['originGrantId']

        tickets = {key: issue(key) for key in contexts}
        assert tickets['a'] != tickets['b']
        for key in tickets:
            harness.scoped(tickets[key], 'ping')
        common_request = uuid.uuid4().hex
        tasks = {}
        for key in tickets:
            url = origin.url(key)
            draft = dict(url=url, filename='same-name.bin', originGrantId=grant(key, url))
            answer = harness.scoped(tickets[key], 'prepare', draft, request_id=common_request)
            task = answer['taskId']
            assert opaque(task)
            tasks[key] = task
            row = harness.row(task)
            assert row and row[0] == 5 and row[3] == contexts[key]['id'] and row[4] == common_request
            assert Path(row[2]).parent == root / ('context-' + key) and not Path(row[2]).exists()
            assert harness.scoped(tickets[key], 'prepare', draft, request_id=common_request)['taskId'] == task
            changed = dict(draft, filename='different-name.bin')
            harness.scoped(tickets[key], 'prepare', changed, expected=False, request_id=common_request)
        assert tasks['a'] != tasks['b'] and origin.snapshot() == []
        print('PASS broker split, two scoped clients, identical request IDs and durable paused replay', flush=True)

        safe_task_keys = {'id', 'name', 'state', 'downloaded', 'total'}
        for key, other in [('a', 'b'), ('b', 'a')]:
            listing = harness.scoped(tickets[key], 'list')
            assert {item['id'] for item in listing['tasks']} == {tasks[key]}
            assert all(set(item) == safe_task_keys for item in listing['tasks'])
            for command in ('pause', 'resume', 'cancel', 'remove'):
                foreign = harness.scoped(tickets[key], command, {'taskId': tasks[other]}, expected=False)
                absent = harness.scoped(tickets[key], command, {'taskId': uuid.uuid4().hex}, expected=False)
                assert foreign.get('error') == absent.get('error'), 'Ownership errors disclose foreign task presence'
            harness.scoped(tickets[key], 'list', {'cursor': tasks[other]}, expected=False)
        assert not harness.legacy('list').get('tasks')
        for command in ('resume', 'pause', 'cancel', 'remove'):
            harness.legacy(command, taskId=tasks['a'], expected=False)

        base = dict(url=origin.url('a'), filename='hostile.bin')
        for field, value in [('ownerId', contexts['b']['id']), ('contextId', contexts['b']['id']),
                             ('queueId', uuid.uuid4().hex), ('runIntent', 'Run'),
                             ('profileId', contexts['b']['id']), ('proxy', routes['b'].url()),
                             ('proxyEndpoint', routes['b'].url()), ('cookie', COOKIES['b']),
                             ('authorization', 'secret-extra'), ('credentialRef', uuid.uuid4().hex),
                             ('proxyCredentialRef', uuid.uuid4().hex),
                             ('destination', str(root / 'escape.bin')), ('downloadRoot', str(root)),
                             ('tool', 'forbidden.exe'),
                             ('executable', 'forbidden.exe'), ('broker', True), ('unexpected', True)]:
            harness.scoped(tickets['a'], 'prepare', dict(base, **{field: value}), expected=False)
        harness.scoped(tickets['a'], 'list', expected=False, ownerId=contexts['b']['id'])
        harness.scoped(tickets['a'], 'context.list', expected=False)
        harness.scoped(tickets['a'], 'context.configure', {}, expected=False, broker=True)
        harness.scoped(uuid.uuid4().hex, 'list', expected=False)
        harness.scoped('invalid-ticket', 'list', expected=False)
        missing = harness.query(dict(version=2, requestId=uuid.uuid4().hex, command='list', params={}))
        assert not missing['ok']
        invalid_version = harness.query(dict(version=3, requestId=uuid.uuid4().hex, command='list',
                                            params={}, sessionToken=tickets['a']))
        assert not invalid_version['ok']
        admin_token = (harness.data / 'ipc-admin.token').read_text().strip()
        spoofed = harness.query(dict(version=2, requestId=uuid.uuid4().hex, command='context.list',
                                    params={}, token=admin_token, sessionToken=tickets['a']))
        assert not spoofed['ok'], 'Browser invocation accepted raw administrative authority'
        spoofed = harness.query(dict(version=2, requestId=uuid.uuid4().hex, command='list', params={},
                                    token=admin_token, sessionToken=uuid.uuid4().hex))
        assert not spoofed['ok'], 'Invalid bearer fell back to caller-supplied administrative token'
        downgraded = harness.query(dict(version=1, requestId=uuid.uuid4().hex, command='context.list',
                                       params={}, token=admin_token, broker=True))
        assert not downgraded['ok'], 'Legacy browser invocation switched to administrative mode'
        assert origin.snapshot() == []
        print('PASS cross-owner list/control/cursor denial and hostile authority/route/path fields without fallback', flush=True)

        for key in tickets:
            harness.scoped(tickets[key], 'resume', {'taskId': tasks[key]})
        for key in tickets:
            row = harness.wait(tasks[key], {9})
            assert Path(row[2]).read_bytes() == DATA
            state = routes[key].snapshot()
            assert state['auth_accepted'] > 0 and state['forwarded_bytes'] > len(DATA)
        hits = origin.snapshot()
        assert {hit[0] for hit in hits} == {'a', 'b'}
        assert all(correct and not leaked for _, _, correct, leaked in hits)
        print('PASS actual HTTP/SOCKS routes, correct per-context cookie grants and exact bytes', flush=True)

        redirect_url = origin.url('b', '/redirect')
        redirected = harness.scoped(tickets['b'], 'add', dict(url=redirect_url, filename='redirect.bin',
                                                            originGrantId=grant('b', redirect_url)))['taskId']
        harness.wait(redirected, {13})
        assert not any(path == '/redirect-target' for _, path, _, _ in origin.snapshot())
        assert not Path(harness.row(redirected)[2]).exists()

        # Real expiry is short and bounded; unlike the one-hour caller lifetime it is practical here.
        expires = int(time.time()) + 2
        expired_grant = grant('b', origin.url('b', '/expired'), expires)
        deadline = time.monotonic() + 3
        while int(time.time()) < expires and time.monotonic() < deadline:
            time.sleep(.02)
        before = len(origin.snapshot())
        harness.scoped(tickets['b'], 'prepare', dict(url=origin.url('b', '/expired'), filename='expired.bin',
                                                    originGrantId=expired_grant), expected=False)
        assert len(origin.snapshot()) == before

        stale = tickets['a']
        contexts['a'] = configure('a', contexts['a'])
        harness.scoped(stale, 'list', expected=False)
        tickets['a'] = issue('a')
        assert tickets['a'] != stale
        harness.scoped(tickets['a'], 'list')
        slow_url = origin.url('a', '/slow')
        slow = harness.scoped(tickets['a'], 'add', dict(url=slow_url, filename='revoked-partial.bin',
                                                      originGrantId=grant('a', slow_url)))['taskId']
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            progress = harness.scoped(tickets['a'], 'list')['tasks']
            current = next((item for item in progress if item['id'] == slow), None)
            if current and current['state'] == 'Downloading' and current['downloaded'] > 0:
                break
            time.sleep(.05)
        else:
            raise AssertionError('Revoke fixture did not start a real partial transfer')
        harness.admin('context.revoke', {'contextId': contexts['a']['id']})
        row = harness.row(slow)
        assert row and row[0] == 5 and row[1] > 0 and not Path(row[2]).exists(), 'Revoke ACK preceded worker pause'
        before = len(origin.snapshot())
        harness.scoped(tickets['a'], 'resume', {'taskId': slow}, expected=False)
        harness.admin('context.issue', {'contextId': contexts['a']['id']}, expected=False)
        time.sleep(.15)
        assert len(origin.snapshot()) == before
        assert Path(harness.row(tasks['a'])[2]).read_bytes() == DATA
        harness.scoped(tickets['b'], 'list')
        print('PASS exact-URL redirect/expiry denial, ticket renewal and revoke waits for paused work', flush=True)

        # Legacy Cookie handoff is standalone but must retain the same no-anonymous source policy.
        legacy_cookie_url = origin.url('a', '/legacy-cookie')
        legacy_cookie = harness.legacy('prepare', url=legacy_cookie_url, filename='legacy-cookie.bin',
                                       cookie=COOKIES['a'])['taskId']
        with contextlib.closing(sqlite3.connect((harness.data / 'tasks.db').as_uri() + '?mode=ro', uri=True)) as database:
            assert database.execute('SELECT origin_auth_required FROM tasks WHERE id=?',
                                    (legacy_cookie,)).fetchone() == (1,)
        harness.legacy('resume', taskId=legacy_cookie)
        assert Path(harness.wait(legacy_cookie, {9})[2]).read_bytes() == DATA
        legacy_redirect = harness.legacy('add', url=origin.url('a', '/redirect'), filename='legacy-redirect.bin',
                                         cookie=COOKIES['a'])['taskId']
        harness.wait(legacy_redirect, {13})
        assert not any(path == '/redirect-target' for _, path, _, _ in origin.snapshot())
        legacy_private_pending = harness.legacy('prepare', url=origin.url('a', '/legacy-restart'),
                                                filename='legacy-private-restart.bin', cookie=COOKIES['a'])['taskId']

        # Supplying an optional URL grant must not be downgraded by rebinding to a public policy.
        contexts['b'] = configure('b', contexts['b'], originAuthenticationRequired=False)
        tickets['b'] = issue('b')
        optional_url = origin.url('b', '/optional-grant')
        optional_grant = grant('b', optional_url)
        optional_task = harness.scoped(tickets['b'], 'prepare', dict(url=optional_url, filename='optional-grant.bin',
                                                                    originGrantId=optional_grant))['taskId']
        before = len(origin.snapshot())
        harness.admin('task.rebind', {'taskId': optional_task}, expected=False)
        assert len(origin.snapshot()) == before
        harness.admin('task.rebind', {'taskId': optional_task, 'originGrantId': grant('b', optional_url)})
        with contextlib.closing(sqlite3.connect((harness.data / 'tasks.db').as_uri() + '?mode=ro', uri=True)) as database:
            assert database.execute('SELECT origin_auth_required FROM tasks WHERE id=?',
                                    (optional_task,)).fetchone() == (1,)
        print('PASS legacy Cookie durable requirement/changed-path denial and optional-grant rebind cannot drop authentication', flush=True)

        # Preserve a live-owner paused task to prove restart does not invent vanished cookie/proxy secrets.
        pending_url = origin.url('b', '/after-restart')
        pending = harness.scoped(tickets['b'], 'prepare', dict(url=pending_url, filename='after-restart.bin',
                                                             originGrantId=grant('b', pending_url)))['taskId']
        legacy = harness.legacy('prepare', url=origin.url('legacy', '/legacy'), filename='legacy-paused.bin')['taskId']
        assert harness.row(legacy)[3] == 'standalone'
        harness.stop()
        # Seed the global history ONLY with the owned manager fully stopped; never a concurrent DB writer.
        with contextlib.closing(sqlite3.connect(harness.data / 'tasks.db')) as database:
            columns = [row[1] for row in database.execute('PRAGMA table_info(tasks)')]
            template = dict(zip(columns, database.execute('SELECT * FROM tasks WHERE id=?', (legacy,)).fetchone()))
            rows = []
            for index in range(1001):
                row = dict(template, id=uuid.uuid4().hex, request_id=uuid.uuid4().hex,
                           destination=str(harness.downloads / f'history-{index}.bin'))
                rows.append([row[column] for column in columns])
            database.executemany('INSERT INTO tasks(' + ','.join(columns) + ') VALUES(' +
                                 ','.join('?' for _ in columns) + ')', rows)
            database.commit()
        harness.start()
        harness.scoped(tickets['a'], 'list', expected=False)
        harness.scoped(tickets['b'], 'list', expected=False)
        tickets['b'] = issue('b')
        listing = harness.scoped(tickets['b'], 'list', {'limit': 1000})
        expected_b = {tasks['b'], redirected, pending, optional_task}
        assert {item['id'] for item in listing['tasks']} == expected_b
        page = harness.scoped(tickets['b'], 'list', {'limit': 1})
        collected = []
        while True:
            assert len(page['tasks']) == 1
            collected.append(page['tasks'][0]['id'])
            cursor = page.get('nextCursor', '')
            if not cursor:
                break
            assert opaque(cursor)
            page = harness.scoped(tickets['b'], 'list', {'limit': 1, 'cursor': cursor})
            assert len(collected) <= len(expected_b)
        assert set(collected) == expected_b and len(collected) == len(expected_b)
        legacy_listing = harness.legacy('list')
        assert len(legacy_listing['tasks']) == 1000
        assert not expected_b.intersection(item['id'] for item in legacy_listing['tasks'])
        before = len(origin.snapshot())
        harness.scoped(tickets['b'], 'resume', {'taskId': pending})
        harness.wait(pending, {13})
        assert len(origin.snapshot()) == before and not Path(harness.row(pending)[2]).exists()
        harness.legacy('resume', taskId=legacy_private_pending)
        harness.wait(legacy_private_pending, {13})
        assert len(origin.snapshot()) == before
        assert not Path(harness.row(legacy_private_pending)[2]).exists()
        harness.legacy('resume', taskId=pending, expected=False)
        contexts_after = harness.admin('context.list')['contexts']
        assert len(contexts_after) == 2 and not next(item for item in contexts_after if item['id'] == contexts['a']['id'])['enabled']
        harness.stop()
        for content in harness.outputs:
            for secret in (USERNAME, PASSWORD, *COOKIES.values()):
                assert secret.encode() not in content, 'Process diagnostics leaked a credential'
        for path in root.rglob('*'):
            if path.is_file():
                content = path.read_bytes()
                for secret in (USERNAME, PASSWORD, *COOKIES.values()):
                    assert secret.encode() not in content, 'Persistent fixture tree contains credentials'
        print('PASS restart invalidates tickets/secrets, scoped pagination filters1000+ legacy history, no persisted secrets', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('host', type=Path)
    parser.add_argument('desktop', type=Path)
    arguments = parser.parse_args()
    run(arguments.host.resolve(strict=True), arguments.desktop.resolve(strict=True))
