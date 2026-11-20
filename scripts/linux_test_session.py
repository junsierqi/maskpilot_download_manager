"""Run actual Linux tests with a disposable session bus, home and encrypted keyring."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time


def run(command):
    if sys.platform != 'linux' or os.geteuid() == 0:
        raise RuntimeError('Linux qualification must run as a non-root user')
    for name in ('dbus-run-session', 'dbus-send', 'gnome-keyring-daemon'):
        if not shutil.which(name):
            raise RuntimeError('Missing test prerequisite: ' + name)
    with tempfile.TemporaryDirectory(prefix='maskpilot-linux-session-') as temporary:
        env = os.environ.copy()
        for key in ('HOME', 'XDG_DATA_HOME', 'XDG_CONFIG_HOME', 'XDG_CACHE_HOME', 'XDG_RUNTIME_DIR'):
            directory = Path(temporary) / key.lower()
            directory.mkdir(mode=0o700)
            env[key] = str(directory)
        for key in ('GTEST_FILTER', 'GTEST_TOTAL_SHARDS', 'GTEST_SHARD_INDEX'):
            env.pop(key, None)
        env.update(LANG='C.UTF-8', QT_QPA_PLATFORM=env.get('QT_QPA_PLATFORM', 'offscreen'))
        daemon = subprocess.Popen(['gnome-keyring-daemon', '--foreground', '--unlock', '--components=secrets'],
                                  env=env, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL)
        try:
            daemon.stdin.write(b'owned-test-keyring-master\n')
            daemon.stdin.close()
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                if daemon.poll() is not None:
                    raise RuntimeError('Private test keyring exited during startup')
                probe = subprocess.run(['dbus-send', '--session', '--type=method_call', '--print-reply',
                    '--dest=org.freedesktop.DBus', '/org/freedesktop/DBus',
                    'org.freedesktop.DBus.NameHasOwner', 'string:org.freedesktop.secrets'],
                    env=env, capture_output=True, timeout=3)
                if b'boolean true' in probe.stdout:
                    break
                time.sleep(.05)
            else:
                raise RuntimeError('Private test keyring startup timed out')
            return subprocess.call(command, env=env)
        finally:
            if daemon.poll() is None:
                daemon.terminate()
                try:
                    daemon.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    daemon.kill()
                    daemon.wait(timeout=5)


if __name__ == '__main__':
    internal = len(sys.argv) > 1 and sys.argv[1] == '--private-session'
    arguments = sys.argv[2:] if internal else sys.argv[1:]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    options = parser.parse_args(arguments)
    if not options.command:
        parser.error('Supply the actual test command to run')
    if internal:
        raise SystemExit(run(options.command))
    raise SystemExit(subprocess.call(['dbus-run-session', '--', sys.executable,
                                     str(Path(__file__).resolve()), '--private-session', *options.command]))
