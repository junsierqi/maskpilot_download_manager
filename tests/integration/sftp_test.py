"""Actual isolated SFTP/password/key/proxy/publication acceptance; synthetic credentials only."""
import argparse
import base64
import contextlib
import hashlib
import json
import os
from pathlib import Path
import re
import sqlite3
import subprocess
import sys
import tempfile
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'support'))
from sftp_fixture import Server, DATA, SHA, USER, PASSWORD, PASSPHRASE
from proxy_fixture import RecordingProxy


def rows(data, query):
    with contextlib.closing(sqlite3.connect(data / 'tasks.db')) as database:
        return database.execute(query).fetchall()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exercise', required=True, type=Path)
    parser.add_argument('--scanner', type=Path)
    options = parser.parse_args()
    client = options.exercise.resolve()
    with tempfile.TemporaryDirectory(prefix='maskpilot-sftp-') as temporary:
        root = Path(temporary)
        source = Server(root / 'fixture')
        environment = dict(os.environ, MASKPILOT_SFTP_USER=USER, MASKPILOT_SFTP_PASSWORD=PASSWORD,
                           MASKPILOT_SFTP_PASSPHRASE=PASSPHRASE, SSH_AUTH_SOCK=str(root / 'no-agent'))
        password_options = ['--sftp-host-key', 'SHA256:' + source.fingerprint.rstrip('='),
                            '--sftp-user-env', 'MASKPILOT_SFTP_USER', '--sftp-password-env', 'MASKPILOT_SFTP_PASSWORD']
        key_options = ['--sftp-host-key', source.fingerprint, '--sftp-user-env', 'MASKPILOT_SFTP_USER',
                       '--sftp-private-key', str(source.private_key),
                       '--sftp-key-passphrase-env', 'MASKPILOT_SFTP_PASSPHRASE']

        def call(name, request=None, arguments=None, expected=True, prefix=None, identifier=None, env=None):
            case = root / name
            case.mkdir(exist_ok=True)
            data = case / 'data'
            output = case / '\u7ed3\u679c.bin'
            command = ([str(client), request or source.url(), str(output), str(data)] if identifier is None else
                       [str(client), '--resume', identifier, str(data)])
            command[1:1] = prefix or []
            command += password_options if arguments is None else arguments
            result = subprocess.run(command, env=env or environment, capture_output=True, timeout=50)
            assert (result.returncode == 0) == expected, (name, result.returncode, result.stdout[-1500:], result.stderr[-1500:])
            for secret in (USER, PASSWORD, PASSPHRASE):
                assert secret.encode() not in result.stdout + result.stderr
                for file in data.rglob('*'):
                    if file.is_file():
                        assert secret.encode() not in file.read_bytes(), (name, file)
            if expected:
                assert output.read_bytes() == (b'' if request and '/empty.bin' in request else DATA)
            found = re.findall(rb'TASK_ID ([a-f0-9]{32})', result.stdout)
            return (found[0].decode() if found else None), data, output

        try:
            call('password', arguments=password_options + ['--sha256', SHA])
            assert source.snapshot()['passwords'] > 0
            before = source.snapshot()
            call('encrypted-key', arguments=key_options + ['--sha256', SHA])
            after = source.snapshot()
            assert after['keys'] > before['keys'] and after['passwords'] == before['passwords']
            assert any(ord(c) > 127 for c in str(source.private_key))
            call('unicode-source', request=source.url('\u6d4b\u8bd5.bin'), arguments=password_options + ['--sha256', SHA])
            call('empty', request=source.url('empty.bin'))
            print('PASS actual password/encrypted private-key login, Unicode source/output/key paths, exact SHA and empty file', flush=True)

            wrong_pin = base64.b64encode(hashlib.sha256(b'owned-wrong-host').digest()).decode()
            before = source.snapshot()
            call('wrong-pin', arguments=['--sftp-host-key', wrong_pin, *password_options[2:]], expected=False)
            after = source.snapshot()
            assert after['none'] == before['none'] and after['passwords'] == before['passwords']
            assert after['keys'] == before['keys'] and after['opens'] == before['opens']
            before = source.snapshot()
            call('missing-pin', arguments=password_options[2:], expected=False)
            call('missing-user', arguments=password_options[:2], expected=False)
            after = source.snapshot()
            assert after['none'] == before['none'] and after['opens'] == before['opens']
            print('PASS missing/invalid pin and missing credentials fail closed; wrong server key stops before any user authentication', flush=True)

            bad_password = dict(environment, MASKPILOT_SFTP_PASSWORD='wrong')
            identifier, data, _ = call('restart', expected=False, env=bad_password)
            before = source.snapshot()
            call('restart', arguments=[], identifier=identifier, expected=False)
            assert source.snapshot()['none'] == before['none']
            call('restart', identifier=identifier)
            before = source.snapshot()
            call('bad-key-passphrase', arguments=key_options, env=dict(environment, MASKPILOT_SFTP_PASSPHRASE='wrong'), expected=False)
            after = source.snapshot()
            assert after['passwords'] == before['passwords'] and after['accepted'] == before['accepted']
            print('PASS wrong password/key passphrase never falls back; restart requires fresh process-local login then resumes explicitly', flush=True)

            for kind in ('http', 'socks5'):
                proxy = RecordingProxy(kind, 'owned-route-user', 'owned-route-password',
                                       mappings={'sftp.invalid': '127.0.0.1'})
                try:
                    route = ['--proxy', proxy.url(), '--proxy-user', 'owned-route-user',
                             '--proxy-password', 'owned-route-password']
                    call('proxy-' + kind, request=source.url(host='sftp.invalid'), arguments=password_options + route + ['--sha256', SHA])
                    report = proxy.snapshot()
                    assert report['auth_accepted'] > 0 and report['destinations']
                    assert all(item['host'] == 'sftp.invalid' and item['port'] == source.server_address[1]
                               for item in report['destinations'])
                    before = source.snapshot()
                    route[-1] = 'wrong'
                    call('proxy-wrong-' + kind, request=source.url(host='sftp.invalid'), arguments=password_options + route, expected=False)
                    assert source.snapshot()['none'] == before['none']
                finally:
                    proxy.close()
            print('PASS HTTP CONNECT/SOCKS5 authenticated proxy-side DNS; wrong route never falls back to direct SFTP', flush=True)

            case = root / 'interrupted'
            case.mkdir()
            data, output = case / 'data', case / '\u7ed3\u679c.bin'
            process = subprocess.Popen([str(client), source.url('slow.bin'), str(output), str(data),
                                        *password_options, '--sha256', SHA], env=environment,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                deadline = time.monotonic() + 20
                identifier = None
                while time.monotonic() < deadline:
                    try:
                        value = rows(data, 'SELECT id FROM tasks')
                        if value:
                            candidate = value[0][0]
                            marker = data / 'parts' / candidate / 'offset-0'
                            if marker.is_file() and 1024 * 1024 <= int(marker.read_text()) < len(DATA):
                                identifier = candidate
                                break
                    except sqlite3.Error:
                        pass
                    time.sleep(.03)
                assert identifier and process.poll() is None, 'Owned interruption fixture completed too early'
            finally:
                if process.poll() is None:
                    process.kill()
                process.communicate(timeout=10)
            offset = int((data / 'parts' / identifier / 'offset-0').read_text())
            assert 0 < offset < len(DATA)
            before = source.snapshot()
            call('interrupted', identifier=identifier, arguments=[], expected=False)
            assert source.snapshot()['none'] == before['none']
            first_read = len(source.snapshot()['reads'])
            call('interrupted', identifier=identifier)
            assert source.snapshot()['reads'][first_read] == offset
            print('PASS interrupted task persists exact prefix, restart requires login, authorized SFTP resumes at saved offset and verifies final SHA', flush=True)

            case = root / 'quota'
            case.mkdir()
            data = case / 'data'
            subprocess.run([str(client), '--resume', uuid.uuid4().hex, str(data)], capture_output=True, timeout=20)
            cap = 257 * 1024 + 3
            with contextlib.closing(sqlite3.connect(data / 'tasks.db')) as database:
                database.execute('UPDATE download_queues SET daily_payload_budget=? WHERE id=?', (cap, 'default'))
                database.commit()
            identifier, _, output = call('quota', arguments=password_options + ['--sha256', SHA], expected=False)
            assert not output.exists() and rows(data, 'SELECT state FROM tasks') == [(17,)]
            assert rows(data, 'SELECT sum(charged),sum(observed) FROM queue_traffic_usage') == [(cap, cap)]
            before = source.snapshot()
            call('quota', identifier=identifier, expected=False)
            assert source.snapshot()['none'] == before['none']
            with contextlib.closing(sqlite3.connect(data / 'tasks.db')) as database:
                database.execute('UPDATE download_queues SET daily_payload_budget=? WHERE id=?', (len(DATA) * 2, 'default'))
                database.commit()
            call('quota', identifier=identifier)
            print('PASS native payload allowance stops SFTP exactly, survives restart without refund or source contact, and resumes after policy repair', flush=True)

            _, _, output = call('bad-sha', arguments=password_options + ['--sha256', '0' * 64], expected=False)
            assert not output.exists()
            _, _, output = call('changed', request=source.url('changed.bin'), arguments=password_options + ['--sha256', SHA], expected=False)
            assert not output.exists()
            print('PASS wrong digest or source metadata change cannot publish an output', flush=True)

            if options.scanner:
                for mode in ('pass', 'reject'):
                    case = root / ('scan-' + mode)
                    case.mkdir()
                    record = case / 'scan.json'
                    output = case / '\u7ed3\u679c.bin'
                    prefix = ['--scan-program', str(options.scanner.resolve())]
                    for value in ('--mode', mode, '--record', str(record), '--final', str(output), '{file}'):
                        prefix += ['--scan-arg', value]
                    _, data, output = call('scan-' + mode, arguments=password_options + ['--sha256', SHA],
                                           prefix=prefix, expected=mode == 'pass')
                    assert output.exists() == (mode == 'pass')
                    assert not json.loads(record.read_text())['finalExists']
                    assert rows(data, 'SELECT scan_state FROM tasks') == [(3 if mode == 'pass' else 4,)]
                print('PASS actual owned scanner pass/reject gates SFTP publication', flush=True)
        finally:
            source.close()


if __name__ == '__main__':
    main()
