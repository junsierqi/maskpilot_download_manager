"""Actual owned scanner processes and CLI publication gates; no antivirus efficacy claim."""
import argparse
import contextlib
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import re
import select
import signal
import sqlite3
import subprocess
import sys
import tempfile
import time
import uuid

from native_budget_test import Origin, DATA
from ftp_fixture import Server as FtpServer, USER, PASSWORD

FLAGS = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
HASH = hashlib.sha256(DATA).hexdigest()


def call(client, *arguments, expected=None, timeout=35):
    environment = os.environ.copy()
    environment['MASKPILOT_SCAN_PRIVATE_ENV'] = 'owned-secret-must-not-reach-scanner'
    result = subprocess.run([str(client), *map(str, arguments)], capture_output=True, timeout=timeout,
                            env=environment, creationflags=FLAGS)
    if expected is not None:
        assert result.returncode == expected, (result.returncode, result.stdout.decode(errors='replace'),
                                                result.stderr.decode(errors='replace'))
    return result


def scan_arguments(scanner, mode, record, target, *, timeout=3, child=None):
    values = ['--mode', mode, '--record', str(record), '--final', str(target),
              '--literal', 'literal & shell; "quotes" stay literal']
    if child:
        values += ['--child-record', str(child)]
    values += ['{file}']
    return ['--scan-program', str(scanner), '--scan-timeout', str(timeout),
            *[part for value in values for part in ('--scan-arg', value)]]


def rows(data, sql, arguments=()):
    with contextlib.closing(sqlite3.connect((data / 'tasks.db').as_uri() + '?mode=ro', uri=True)) as db:
        db.row_factory = sqlite3.Row
        return [dict(row) for row in db.execute(sql, arguments)]


def task(data, identifier):
    return rows(data, 'SELECT * FROM tasks WHERE id=?', (identifier,))[0]


def identity(result):
    match = re.search(rb'TASK_ID ([a-f0-9]{32})', result.stdout)
    assert match, result.stdout.decode(errors='replace')
    return match[1].decode()


def record_matches(record, target):
    value = json.loads(record.read_text(encoding='utf-8'))
    assert not value['finalExists'] and Path(value['path']) != target
    assert value['files'] and value['files'][0]['sha256'] == HASH
    assert not value['privateEnvironmentPresent']
    assert 'literal & shell; "quotes" stay literal' in value['arguments']
    assert 'private-source-marker' not in json.dumps(value)


def http_cases(client, scanner, root, origin):
    case = root / 'unavailable-startup'; case.mkdir()
    data, target, record = case / 'data', case / 'final.bin', case / 'probe.json'
    before = len(origin.hits)
    first = call(client, *scan_arguments(case / 'missing.exe', 'pass', record, target),
                 origin.url(), target, data)
    identifier = identity(first)
    saved = task(data, identifier)
    assert first.returncode != 0 and len(origin.hits) == before and not target.exists()
    assert saved['state'] == 13 and saved['scan_required'] == 1 and saved['scan_state'] == 1
    call(client, *scan_arguments(scanner, 'pass', record, target), '--resume', identifier, data, expected=0)
    assert target.read_bytes() == DATA and task(data, identifier)['scan_state'] == 3
    record_matches(record, target)
    print('PASS unavailable startup scanner preserves a repairable task without contacting origin')
    for waiver in (False, True):
        case = root / ('waiver' if waiver else 'retry'); case.mkdir()
        data, target, record = case / 'data', case / 'final.bin', case / 'probe.json'
        source = origin.url() + '?private-source-marker=not-a-scanner-argument'
        first = call(client, *scan_arguments(scanner, 'reject', record, target),
                     source, target, data, '--sha256', HASH)
        identifier = identity(first)
        saved = task(data, identifier)
        assert first.returncode != 0 and saved['state'] == 13 and saved['scan_required'] == 1
        assert saved['scan_state'] == 4 and saved['scan_at'] > 0 and not target.exists()
        record_matches(record, target)
        before = len(origin.hits)
        missing = call(client, '--resume', identifier, data)
        assert missing.returncode != 0 and len(origin.hits) == before and not target.exists()
        assert task(data, identifier)['scan_required'] == 1
        if waiver:
            record.unlink()
            # Even a configured global scanner cannot silently override an explicit per-task waiver.
            call(client, *scan_arguments(scanner, 'reject', record, target), '--waive-scan',
                 '--resume', identifier, data, expected=0)
            assert not record.exists()
            assert task(data, identifier)['scan_required'] == 0 and task(data, identifier)['scan_state'] == 6
        else:
            call(client, *scan_arguments(scanner, 'pass', record, target), '--resume', identifier, data, expected=0)
            record_matches(record, target)
            assert task(data, identifier)['scan_state'] == 3 and task(data, identifier)['scan_required'] == 1
        assert target.read_bytes() == DATA and task(data, identifier)['state'] == 9
        for path in data.glob('tasks.db*'):
            if path.is_file():
                assert b'literal & shell' not in path.read_bytes(), 'Scanner arguments entered durable task data'
    print('PASS actual HTTP reject/restart refusal/verified retry and explicit waiver preserve the publication gate')
    for mode in ('mutate', 'crash'):
        case = root / mode; case.mkdir()
        data, target, record = case / 'data', case / 'final.bin', case / 'probe.json'
        result = call(client, *scan_arguments(scanner, mode, record, target), origin.url(), target, data)
        identifier = identity(result)
        assert result.returncode != 0 and not target.exists()
        assert task(data, identifier)['state'] == 13 and task(data, identifier)['scan_state'] == 5
    print('PASS actual scanner mutation and abnormal exit never publish or report completion')


def child_gone(identity_path, scanner):
    """Only terminate a leaked fixture child after exact image and creation-time checks."""
    assert identity_path.is_file(), 'Owned scanner did not start its actual descendant'
    value = json.loads(identity_path.read_text(encoding='utf-8'))
    if sys.platform == 'darwin':
        # Observe only: no numeric-PID signal can be made atomic with an identity
        # check on Darwin. The disposable CI VM reclaims a failed fixture.
        assert type(value['pid']) is int and value['pid'] > 1
        assert Path(value['program']).resolve() == scanner.resolve()
        deadline = time.monotonic() + 5
        while True:
            result = subprocess.run(['/bin/ps', '-ww', '-p', str(value['pid']), '-o', 'stat=', '-o', 'command='],
                                    capture_output=True, text=True, timeout=5)
            fields = result.stdout.strip().split(None, 1)
            if result.returncode == 1 and not fields:
                return
            assert result.returncode == 0 and len(fields) == 2, 'Cannot inspect owned Darwin descendant'
            state, command = fields
            if state.startswith('Z') or str(identity_path) not in command or str(scanner) not in command:
                return  # Original child exited; never touch a reused PID.
            assert time.monotonic() < deadline, 'Owned scanner descendant survived owner termination'
            time.sleep(.05)
    if sys.platform == 'linux':
        try:
            handle = os.pidfd_open(value['pid'])
        except ProcessLookupError:
            return
        try:
            poller = select.poll()
            poller.register(handle, select.POLLIN)
            if poller.poll(0):
                return
            try:
                proc = Path('/proc') / str(value['pid'])
                stamp = (proc / 'stat').read_text().rsplit(')', 1)[1].split()[19]
                image = (proc / 'exe').resolve(strict=True)
            except FileNotFoundError:
                assert poller.poll(3000), 'Owned descendant changed identity while running'
                return
            if stamp != value['created'] or image != scanner.resolve():
                return  # Never signal a reused PID.
            exited = bool(poller.poll(3000))
            if not exited:
                signal.pidfd_send_signal(handle, signal.SIGKILL)
                assert poller.poll(5000), 'Cannot clean up verified fixture descendant'
            assert exited, 'Owned scanner descendant survived owner termination'
        finally:
            os.close(handle)
        return
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.GetProcessTimes.argtypes = [wintypes.HANDLE, *([ctypes.POINTER(wintypes.FILETIME)] * 4)]
    kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
                                                ctypes.POINTER(wintypes.DWORD)]
    kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    handle = kernel.OpenProcess(0x100000 | 0x1000 | 1, False, value['pid'])
    if not handle:
        assert ctypes.get_last_error() == 87, 'Cannot inspect the owned scanner descendant'
        return
    try:
        state = kernel.WaitForSingleObject(handle, 0)
        assert state in (0, 258), 'Cannot observe the owned scanner descendant'
        if state == 0:
            return  # A terminated process no longer needs an inspectable image.
        created, exited, cpu, user = [wintypes.FILETIME() for _ in range(4)]
        if not kernel.GetProcessTimes(handle, ctypes.byref(created), ctypes.byref(exited), ctypes.byref(cpu), ctypes.byref(user)):
            assert kernel.WaitForSingleObject(handle, 3000) == 0, 'Cannot inspect a live scanner descendant'
            return
        stamp = (created.dwHighDateTime << 32) | created.dwLowDateTime
        path = ctypes.create_unicode_buffer(32768); size = wintypes.DWORD(len(path))
        if not kernel.QueryFullProcessImageNameW(handle, 0, path, ctypes.byref(size)):
            # Exit can race either identity query. Require exit of this held
            # handle; a failed query alone never authorizes success or cleanup.
            assert kernel.WaitForSingleObject(handle, 3000) == 0, 'Cannot inspect a live scanner descendant'
            return
        if stamp != int(value['created']) or os.path.normcase(path.value) != os.path.normcase(str(scanner)):
            return  # The original owned child already exited; never touch a reused PID.
        state = kernel.WaitForSingleObject(handle, 3000)
        if state != 0:
            assert kernel.TerminateProcess(handle, 130), 'Cannot clean up the verified owned fixture child'
            kernel.WaitForSingleObject(handle, 5000)
        assert state == 0, 'Owned scanner descendant survived its job termination'
    finally:
        kernel.CloseHandle(handle)


def process_cases(client, scanner, root, origin):
    case = root / 'timeout'; case.mkdir()
    data, target, record, child = case / 'data', case / 'final.bin', case / 'probe.json', case / 'child.json'
    result = call(client, *scan_arguments(scanner, 'spawn', record, target, timeout=1, child=child),
                  origin.url(), target, data)
    identifier = identity(result)
    assert result.returncode != 0 and task(data, identifier)['scan_state'] == 5 and not target.exists()
    child_gone(child, scanner)
    case = root / 'forced-exit'; case.mkdir()
    data, target, record, child = case / 'data', case / 'final.bin', case / 'probe.json', case / 'child.json'
    process = subprocess.Popen([str(client), *scan_arguments(scanner, 'spawn', record, target, timeout=30, child=child),
                                origin.url(), str(target), str(data)], stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, creationflags=FLAGS)
    try:
        deadline = time.monotonic() + 15
        saved = None
        while time.monotonic() < deadline and process.poll() is None:
            if child.is_file() and (data / 'tasks.db').is_file():
                saved = next((row for row in rows(data, 'SELECT * FROM tasks') if row['scan_state'] == 2), None)
                if saved:
                    break
            time.sleep(.02)
        assert saved and saved['scan_required'] == 1, 'Did not observe a durable active scan'
        process.kill(); process.communicate(timeout=10)
        child_gone(child, scanner)
        before = len(origin.hits)
        answer = call(client, '--resume', saved['id'], data)
        assert answer.returncode != 0 and len(origin.hits) == before and not target.exists()
        assert task(data, saved['id'])['scan_required'] == 1
    finally:
        if process.poll() is None:
            process.kill(); process.communicate(timeout=10)
        if child.is_file():
            child_gone(child, scanner)
    print('PASS actual scanner timeout and forced application exit stop owned descendants and preserve required retry')


def ftp_case(client, scanner, root):
    server = FtpServer(); server.files['scan.bin'] = DATA
    case = root / 'ftp'; case.mkdir()
    data, target, record = case / 'data', case / 'final.bin', case / 'probe.json'
    try:
        result = call(client, *scan_arguments(scanner, 'reject', record, target), server.url('scan.bin'), target, data,
                      '--ftp-user', USER, '--ftp-password', PASSWORD, '--sha256', HASH)
        identifier = identity(result)
        assert result.returncode != 0 and task(data, identifier)['scan_state'] == 4 and not target.exists()
        record_matches(record, target)
        call(client, *scan_arguments(scanner, 'pass', record, target), '--resume', identifier, data,
             '--ftp-user', USER, '--ftp-password', PASSWORD, expected=0)
        assert target.read_bytes() == DATA and task(data, identifier)['scan_state'] == 3
        record_matches(record, target)
    finally:
        server.close()
    print('PASS actual FTP verified private-stage scanning rejects then resumes with the exact payload')


def migration_case(client, root, origin):
    case = root / 'migration-v14'; case.mkdir()
    data = case / 'data'; data.mkdir()
    destination = case / 'legacy.bin'; identifier = uuid.uuid4().hex
    migrations = Path(__file__).resolve().parents[2] / 'database' / 'migrations'
    with contextlib.closing(sqlite3.connect(data / 'tasks.db')) as db:
        for migration in sorted(migrations.glob('*.sql')):
            if int(migration.name.split('_')[0]) <= 14:
                db.executescript(migration.read_text(encoding='utf-8'))
        columns = ['id', 'url', 'destination', 'category', 'referrer', 'user_agent', 'proxy', 'expected_sha256',
                   'connections', 'priority', 'max_retries', 'speed_limit', 'scheduled_at', 'stop_at', 'state',
                   'downloaded', 'total', 'created_at', 'updated_at', 'retries', 'resumable', 'error']
        values = [identifier, 'https://example.test/file', str(destination), 'General', '', 'MaskPilot', '', '',
                  1, 0, 3, 0, 0, 0, 5, 0, -1, 1, 1, 0, 0, '']
        db.execute('INSERT INTO tasks(' + ','.join(columns) + ') VALUES(' + ','.join('?' for _ in values) + ')', values)
        db.commit()
    call(client, '--resume', uuid.uuid4().hex, data)
    assert rows(data, 'PRAGMA user_version')[0]['user_version'] == 19
    saved = task(data, identifier)
    assert saved['destination'] == str(destination) and saved['scan_required'] == saved['scan_state'] == saved['scan_at'] == 0
    backups = list(data.glob('tasks.db.before-v15-*.db')); assert len(backups) == 1
    with contextlib.closing(sqlite3.connect(backups[0])) as db:
        assert db.execute('PRAGMA user_version').fetchone()[0] == 14
        assert db.execute('SELECT destination FROM tasks WHERE id=?', (identifier,)).fetchone()[0] == str(destination)
    with contextlib.closing(sqlite3.connect(data / 'tasks.db')) as db:
        db.execute('UPDATE tasks SET scan_state=1.25 WHERE id=?', (identifier,)); db.commit()
    before = len(origin.hits)
    result = call(client, origin.url(), case / 'never.bin', data)
    assert result.returncode != 0 and len(origin.hits) == before and not (case / 'never.bin').exists()
    print('PASS actual schema14 backup/default migration and corrupted scan state rejection before origin contact')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exercise', type=Path, required=True)
    parser.add_argument('--scanner', type=Path, required=True)
    args = parser.parse_args()
    client, scanner = args.exercise.resolve(strict=True), args.scanner.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix='maskpilot-scan-') as temporary:
        root = Path(temporary) / '扫描 Unicode spaces'; root.mkdir()
        origin = Origin()
        try:
            http_cases(client, scanner, root, origin)
            process_cases(client, scanner, root, origin)
            ftp_case(client, scanner, root)
            migration_case(client, root, origin)
        finally:
            origin.close()


if __name__ == '__main__':
    main()
