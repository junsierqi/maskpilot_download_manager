"""Strict native body credits through actual CLI, SQLite, HTTP and FTP adapters."""
import argparse
import contextlib
import http.server
from pathlib import Path
import re
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'support'))
from ftp_fixture import Server as FtpServer, USER, PASSWORD

DATA = bytes(range(256)) * 8193


class Origin(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, slow=False):
        self.hits = []
        self.slow = slow
        super().__init__(('127.0.0.1', 0), Handler)
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)
        self.thread.start()

    def url(self):
        return f'http://127.0.0.1:{self.server_port}/payload'

    def close(self):
        self.shutdown()
        self.server_close()
        self.thread.join(timeout=5)


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def do_GET(self):
        match = re.fullmatch(r'bytes=(\d+)-(\d*)', self.headers.get('Range', ''))
        start, end = (int(match[1]), int(match[2] or len(DATA) - 1)) if match else (0, len(DATA) - 1)
        self.server.hits.append((start, end))
        if not 0 <= start <= end < len(DATA):
            self.send_error(416)
            return
        try:
            self.send_response(206 if match else 200)
            self.send_header('Content-Length', str(end - start + 1))
            self.send_header('ETag', '"native-budget-fixture-v1"')
            self.send_header('Accept-Ranges', 'bytes')
            if match:
                self.send_header('Content-Range', f'bytes {start}-{end}/{len(DATA)}')
            self.end_headers()
            for pos in range(start, end + 1, 4096):
                self.wfile.write(DATA[pos:min(pos + 4096, end + 1)])
                self.wfile.flush()
                if self.server.slow and end > start:
                    time.sleep(.015)
        except (OSError, ConnectionError):
            pass


def call(client, *args, expected=None, timeout=35):
    result = subprocess.run([str(client), *map(str, args)], capture_output=True, timeout=timeout)
    if expected is not None:
        assert result.returncode == expected, (result.returncode, result.stdout.decode(errors='replace'),
                                                result.stderr.decode(errors='replace'))
    return result


def seed(client, data, budget):
    call(client, '--resume', uuid.uuid4().hex, data)
    assert (data / 'tasks.db').is_file(), 'CLI did not create its owned database'
    with contextlib.closing(sqlite3.connect(data / 'tasks.db')) as db:
        db.execute('UPDATE download_queues SET daily_payload_budget=? WHERE id=?', (budget, 'default'))
        db.commit()


def rows(data, sql, args=()):
    with contextlib.closing(sqlite3.connect((data / 'tasks.db').as_uri() + '?mode=ro', uri=True)) as db:
        return db.execute(sql, args).fetchall()


def policy(data, amount):
    # Called only after the owned CLI exited and released instance.lock.
    with contextlib.closing(sqlite3.connect(data / 'tasks.db')) as db:
        db.execute('UPDATE download_queues SET daily_payload_budget=? WHERE id=?', (amount, 'default'))
        db.commit()


def identity(result):
    match = re.search(rb'TASK_ID ([a-f0-9]{32})', result.stdout)
    assert match, 'CLI did not return a durable task ID'
    return match[1].decode()


def usage(data):
    values = rows(data, 'SELECT sum(charged),sum(observed) FROM queue_traffic_usage')
    return tuple(value or 0 for value in values[0])


def http_case(client, root, connections):
    data, target = root / f'http-{connections}', root / f'http-{connections}.bin'
    cap = 257 * 1024 + 3
    seed(client, data, cap)
    origin = Origin()
    try:
        first = call(client, origin.url(), target, data, '--connections', connections, expected=4)
        task = identity(first)
        state, downloaded, intent = rows(data, 'SELECT state,downloaded,run_intent FROM tasks WHERE id=?',
                                         (task,))[0]
        assert state == 17 and intent == 0 and 0 < downloaded < len(DATA)
        assert not target.exists() and usage(data) == (cap, cap), 'Native quota did not settle exactly'
        before = len(origin.hits)
        call(client, '--resume', task, data, expected=4)
        assert len(origin.hits) == before, 'An exhausted queue contacted its origin again'
        assert usage(data) == (cap, cap), 'A restart refunded spent allowance'
        policy(data, len(DATA) * 3)
        call(client, '--resume', task, data, '--connections', 1, expected=0)
        assert target.read_bytes() == DATA
        charged, observed = usage(data)
        probes = sum(1 for start, end in origin.hits if start == end == 0)
        assert charged == observed == len(DATA) + probes, (charged, observed, probes)
        assert any(start > 0 for start, end in origin.hits if end > start), 'Recovery did not retain prefixes'
        with contextlib.closing(sqlite3.connect(data / 'tasks.db')) as db:
            db.execute('DELETE FROM tasks WHERE id=?', (task,))
            db.commit()
        assert usage(data) == (charged, observed), 'Removing history refunded received bytes'
        print(f'PASS actual HTTP{connections}: exact quota, retained prefix, shared credits, digest and history retention')
    finally:
        origin.close()


def ftp_case(client, root):
    server = FtpServer()
    server.files['norest.bin'] = DATA
    data, target = root / 'ftp', root / 'ftp.bin'
    cap = 257 * 1024 + 7
    seed(client, data, cap)
    try:
        first = call(client, server.url('norest.bin'), target, data,
                     '--ftp-user', USER, '--ftp-password', PASSWORD, expected=4)
        task = identity(first)
        assert usage(data) == (cap, cap) and not target.exists()
        policy(data, len(DATA) * 3)
        call(client, '--resume', task, data, '--ftp-user', USER, '--ftp-password', PASSWORD, expected=0)
        assert target.read_bytes() == DATA
        assert server.rest_rejections > 0, 'Actual FTP REST-denied recovery was not exercised'
        charged, observed = usage(data)
        assert charged == observed == cap + len(DATA), (charged, observed, cap)
        print('PASS actual FTP REST denial: received retry bytes count again; correct digest')
    finally:
        server.close()


def crash_case(client, root):
    data, target = root / 'crash', root / 'crash.bin'
    seed(client, data, 1024 * 1024)
    origin = Origin(slow=True)
    process = subprocess.Popen([str(client), origin.url(), str(target), str(data), '--connections', '1'],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic() + 15
        captured = None
        while time.monotonic() < deadline and process.poll() is None:
            current = rows(data, 'SELECT id,reserved,observed FROM queue_traffic_runs WHERE status=0 '
                                  'AND reserved>observed')
            if current:
                captured = current[0]
                break
            time.sleep(.005)
        assert captured, 'Did not observe a real precharged native run'
        process.kill()
        process.communicate(timeout=10)
        before = usage(data)
        calls = len(origin.hits)
        # Opens/recovers the store without resuming the interrupted task.
        call(client, '--resume', uuid.uuid4().hex, data)
        assert usage(data) == before, 'Crash recovery refunded uncertain outstanding credits'
        status, reserved, observed = rows(data, 'SELECT status,reserved,observed FROM queue_traffic_runs '
                                              'WHERE id=?', (captured[0],))[0]
        assert status == 2 and reserved > observed
        assert len(origin.hits) == calls and not target.exists()
        print('PASS actual forced termination: abandoned outstanding native credits stay charged')
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate(timeout=10)
        origin.close()


def failures(client, root):
    origin = Origin()
    try:
        data = root / 'storage-failure'
        seed(client, data, 1024 * 1024)
        with contextlib.closing(sqlite3.connect(data / 'tasks.db')) as db:
            db.execute("CREATE TRIGGER deny_traffic_run BEFORE INSERT ON queue_traffic_runs BEGIN "
                       "SELECT RAISE(ABORT,'owned ledger fault'); END")
            db.commit()
        result = call(client, origin.url(), root / 'fault.bin', data, timeout=15)
        assert result.returncode != 0 and not origin.hits
        assert not (root / 'fault.bin').exists() and usage(data) == (0, 0)
        print('PASS actual ledger write failure: no origin contact or accepted payload')
        data = root / 'strict-torrent'
        seed(client, data, 1024 * 1024)
        result = call(client, '--bt-no-discovery',
                      'magnet:?xt=urn:btih:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
                      root / 'torrent', data, timeout=15)
        assert result.returncode != 0 and b'HTTP' in result.stderr + result.stdout
        assert not (root / 'torrent').exists() and usage(data) == (0, 0)
        print('PASS actual unified CLI rejects strict-budget torrent before launch')
    finally:
        origin.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exercise', type=Path, required=True)
    options = parser.parse_args()
    client = options.exercise.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix='maskpilot-native-budget-') as temporary:
        root = Path(temporary).resolve()
        for connections in (1, 8):
            http_case(client, root, connections)
        ftp_case(client, root)
        crash_case(client, root)
        failures(client, root)


if __name__ == '__main__':
    main()
