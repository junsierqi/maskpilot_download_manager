"""Actual Linux providers in a private session bus and encrypted test keyring."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import uuid


def run(args, env, timeout=60):
    result = subprocess.run(list(map(str, args)), env=env, capture_output=True, timeout=timeout)
    sys.stdout.buffer.write(result.stdout)
    sys.stderr.buffer.write(result.stderr)
    assert result.returncode == 0, ('Owned Linux product process failed', result.returncode)


def exercise(binary):
    for name in ('dbus-run-session', 'dbus-send', 'gnome-keyring-daemon'):
        assert shutil.which(name), f'Required Linux test component missing: {name}'
    if os.environ.get('MASKPILOT_PRIVATE_BUS') != '1':
        env = os.environ.copy()
        env['MASKPILOT_PRIVATE_BUS'] = '1'
        run(['dbus-run-session', '--', sys.executable, __file__, '--exercise', binary], env, 85)
        return
    with tempfile.TemporaryDirectory(prefix='maskpilot-linux-platform-') as temporary:
        root = Path(temporary)
        env = os.environ.copy()
        for key, sub in [('HOME', 'home'), ('XDG_DATA_HOME', 'data'), ('XDG_CONFIG_HOME', 'config'),
                         ('XDG_CACHE_HOME', 'cache'), ('XDG_RUNTIME_DIR', 'runtime')]:
            target = root / sub
            target.mkdir(mode=0o700)
            env[key] = str(target)
        env['LANG'] = 'C.UTF-8'
        missing = env.copy()
        missing['DBUS_SESSION_BUS_ADDRESS'] = 'unix:path=' + str(root / 'missing-bus')
        run([binary, '--no-service'], missing, 8)
        print('PASS actual missing secret service fails closed without plaintext fallback', flush=True)

        def start():
            stream = (root / ('daemon-' + uuid.uuid4().hex + '.log')).open('wb')
            process = subprocess.Popen(['gnome-keyring-daemon', '--foreground', '--unlock', '--components=secrets'],
                                       env=env, stdin=subprocess.PIPE, stdout=stream, stderr=subprocess.STDOUT)
            stream.close()
            process.stdin.write(b'owned-keyring-master\n')
            process.stdin.close()
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                assert process.poll() is None, 'Owned keyring daemon exited before service startup'
                check = subprocess.run(['dbus-send', '--session', '--type=method_call', '--print-reply',
                                        '--dest=org.freedesktop.DBus', '/org/freedesktop/DBus',
                                        'org.freedesktop.DBus.NameHasOwner', 'string:org.freedesktop.secrets'],
                                       env=env, capture_output=True, timeout=3)
                if b'boolean true' in check.stdout:
                    return process
                time.sleep(.05)
            raise AssertionError('Owned secret service startup exceeded deadline')

        def stop(process):
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)

        daemon = start()
        try:
            run([binary], env)
            identifiers = [uuid.uuid4().hex for _ in range(3)]
            run([binary, '--vault-store', *identifiers], env, 10)
            stop(daemon)
            files = list((root / 'data').rglob('*.keyring'))
            assert files, 'Native provider did not persist an encrypted keyring'
            for file in files:
                data = file.read_bytes()
                assert b'owned-linux-vault-secret-' not in data and b'owned-keyring-master' not in data
            daemon = start()
            run([binary, '--vault-check', *identifiers], env, 10)
            print('PASS actual secret-service restart, persisted bound credential and encrypted-at-rest bytes', flush=True)
        finally:
            stop(daemon)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--exercise', type=Path, required=True)
    exercise(parser.parse_args().exercise.resolve(strict=True))
