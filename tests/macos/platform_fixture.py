"""Exercise actual Darwin vault, exclusive publication and owner-death behavior."""
import argparse
import json
import os
from pathlib import Path
import secrets
import signal
import subprocess
import sys
import tempfile
import time


def process_field(pid, field):
    result = subprocess.run(['/bin/ps', '-ww', '-p', str(pid), '-o', field + '='],
                            capture_output=True, text=True, timeout=5)
    assert result.returncode in (0, 1), 'Cannot inspect owned native process'
    assert result.returncode == 0 or not result.stdout.strip(), 'Ambiguous native process inspection'
    return result.stdout.strip()


def alive(pid):
    state = process_field(pid, 'stat')
    return bool(state) and not state.startswith('Z')


def exercise(platform, helper, mode, directory):
    nonce = secrets.token_hex(16)
    record = Path(directory) / (mode + '.json')
    owner = subprocess.Popen([platform, mode, helper, str(record), nonce])
    pids = []
    try:
        deadline = time.monotonic() + 25
        while time.monotonic() < deadline:
            if record.exists():
                try:
                    data = json.loads(record.read_text())
                    break
                except json.JSONDecodeError:
                    pass
            if owner.poll() is not None:
                raise AssertionError('Helper owner exited before proving private-input EOF')
            time.sleep(0.05)
        else:
            raise AssertionError('Private helper startup/input EOF timed out')
        assert data['nonce'] == nonce and data['inputClosed'] is True
        pids = [data['parent'], data['child']]
        assert all(type(pid) is int and pid > 1 for pid in pids) and len(set(pids)) == 2
        if mode == '--guardian-owner':
            assert all(alive(pid) for pid in pids), 'Helper tree must exist before forced owner death'
            owner.kill()
            owner.wait(timeout=10)
        else:
            assert owner.wait(timeout=15) == 0
        deadline = time.monotonic() + 15
        while any(alive(pid) for pid in pids) and time.monotonic() < deadline:
            time.sleep(0.05)
        assert not any(alive(pid) for pid in pids), 'Owned helper descendants survived owner death/stop'
    finally:
        if owner.poll() is None:
            owner.kill()
            owner.wait(timeout=10)
        for pid in pids:
            # Restrict fallback cleanup to the exact helper command and unique nonce.
            command = process_field(pid, 'command')
            if helper in command and nonce in command:
                try:
                    os.kill(pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--platform', required=True)
    parser.add_argument('--helper', required=True)
    arguments = parser.parse_args()
    if sys.platform != 'darwin':
        raise RuntimeError('Native macOS qualification requires Darwin')
    platform = str(Path(arguments.platform).resolve(strict=True))
    helper = str(Path(arguments.helper).resolve(strict=True))
    subprocess.run([platform], check=True, timeout=45)
    with tempfile.TemporaryDirectory(prefix='maskpilot-macos-platform-') as directory:
        for mode in ['--guardian-stop', '--guardian-owner']:
            exercise(platform, helper, mode, directory)


if __name__ == '__main__':
    main()
