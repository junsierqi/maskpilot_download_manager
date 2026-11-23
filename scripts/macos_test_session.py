"""Isolated hosted-CI Keychain session; never change a developer's local search list."""
import argparse
import os
from pathlib import Path
import secrets
import shlex
import subprocess
import sys
import tempfile
import re
import socket


def keychain_paths(output):
    if len(output) > 131072:
        raise RuntimeError('Keychain search list exceeds the qualification bound')
    try:
        values = shlex.split(output)
    except ValueError:
        raise RuntimeError('Malformed keychain search list') from None
    if len(values) > 128 or len(set(values)) != len(values):
        raise RuntimeError('Ambiguous keychain search list')
    if any(not value.startswith('/') or value.startswith('//') or
           any(ord(c) < 32 or ord(c) == 127 for c in value) for value in values):
        raise RuntimeError('Unsafe keychain path in qualification session')
    return values


class KeychainSession:
    def __init__(self, directory, run=subprocess.run):
        self.directory = Path(directory).resolve(strict=True)
        self.path = self.directory / 'qualification.keychain-db'
        self.password = secrets.token_hex(24)
        self.run = run
        self.created = False
        self.original = None
        self.default = None

    def security(self, *arguments, required=True):
        # Never expose a temporary keychain password through CalledProcessError/command logging.
        result = self.run(['/usr/bin/security', *map(str, arguments)], capture_output=True, text=True, timeout=30)
        if required and result.returncode:
            raise RuntimeError('CI keychain operation failed; no credential output is logged')
        return result

    def __enter__(self):
        self.original = keychain_paths(self.security('list-keychains', '-d', 'user').stdout)
        defaults = keychain_paths(self.security('default-keychain', '-d', 'user').stdout)
        if len(defaults) != 1:
            raise RuntimeError('An existing default keychain is required to restore CI state exactly')
        self.default = defaults[0]
        try:
            if self.path.exists():
                raise RuntimeError('Qualification keychain path must be new')
            # The directory is private and newly allocated. Also clean up a create
            # command that times out after it has already created its owned file.
            self.created = True
            self.security('create-keychain', '-p', self.password, self.path)
            self.security('set-keychain-settings', '-lut', '21600', self.path)
            self.security('unlock-keychain', '-p', self.password, self.path)
            self.security('list-keychains', '-d', 'user', '-s', self.path)
            self.security('default-keychain', '-d', 'user', '-s', self.path)
        except BaseException:
            self.restore()
            raise
        return self

    def restore(self):
        if not self.created:
            return
        errors = []
        for command in [
            ('default-keychain', '-d', 'user', '-s', self.default),
            ('list-keychains', '-d', 'user', '-s', *self.original),
        ]:
            try:
                self.security(*command)
            except Exception:
                errors.append('restore')
        if not errors:
            try:
                if (keychain_paths(self.security('list-keychains', '-d', 'user').stdout) != self.original or
                        keychain_paths(self.security('default-keychain', '-d', 'user').stdout) != [self.default]):
                    errors.append('verify')
            except Exception:
                errors.append('verify')
        # Only delete the exact keychain created in this private directory.
        try:
            result = self.security('delete-keychain', self.path, required=False)
            if result.returncode and self.path.exists():
                errors.append('delete')
        except Exception:
            errors.append('delete')
        self.created = False
        if errors:
            raise RuntimeError('CI keychain cleanup or exact restoration failed')

    def __exit__(self, *exception):
        self.restore()


def require_hosted_ci(environment, system):
    if (system != 'darwin' or environment.get('GITHUB_ACTIONS') != 'true' or
            environment.get('RUNNER_OS') != 'macOS' or environment.get('RUNNER_ENVIRONMENT') != 'github-hosted'):
        raise RuntimeError('Disposable Keychain qualification is restricted to a macOS GitHub-hosted CI job')
    temporary = environment.get('RUNNER_TEMP')
    if not temporary or not Path(temporary).is_absolute() or not Path(temporary).is_dir():
        raise RuntimeError('A valid owned runner temporary directory is required')
    return temporary


def probe_loopback():
    for kind in (socket.SOCK_STREAM, socket.SOCK_DGRAM):
        with socket.socket(socket.AF_INET, kind) as endpoint:
            endpoint.bind(('127.0.0.2', 0))


class LoopbackSession:
    """Own one missing address in a guarded disposable hosted VM, never a whole interface."""
    address = '127.0.0.2'

    def __init__(self, run=subprocess.run, probe=probe_loopback):
        self.run = run
        self.probe = probe
        self.original = None
        self.created = False

    def command(self, arguments):
        result = self.run(arguments, capture_output=True, text=True, timeout=30)
        if result.returncode:
            raise RuntimeError('Hosted loopback fixture command failed')
        return result.stdout

    def addresses(self):
        output = self.command(['/sbin/ifconfig', 'lo0'])
        if len(output) > 131072:
            raise RuntimeError('Loopback fixture inspection exceeded its bound')
        values = re.findall(r'^\s*inet\s+([0-9.]+)\s', output, re.MULTILINE)
        if '127.0.0.1' not in values or len(values) != len(set(values)):
            raise RuntimeError('Ambiguous loopback fixture state')
        return set(values)

    def __enter__(self):
        self.original = self.addresses()
        try:
            if self.address not in self.original:
                # Record the missing address before invoking a command that could
                # time out after mutation. This VM has one owning hosted job.
                self.created = True
                self.command(['/usr/bin/sudo', '-n', '/sbin/ifconfig', 'lo0',
                              'alias', self.address, 'netmask', '255.255.255.255'])
                if self.addresses() != self.original | {self.address}:
                    raise RuntimeError('Unexpected hosted loopback fixture mutation')
            self.probe()
        except BaseException:
            self.restore()
            raise
        return self

    def restore(self):
        if not self.created:
            return
        if self.address in self.addresses():
            self.command(['/usr/bin/sudo', '-n', '/sbin/ifconfig', 'lo0', '-alias', self.address])
        if self.addresses() != self.original:
            raise RuntimeError('Hosted loopback fixture restoration failed')
        self.created = False

    def __exit__(self, exc_type, exc_value, traceback):
        self.restore()
        return False


def execute(command, platform_executable, environment=None, system=None):
    environment = dict(os.environ if environment is None else environment)
    parent = require_hosted_ci(environment, sys.platform if system is None else system)
    with tempfile.TemporaryDirectory(prefix='maskpilot-keychain-', dir=parent) as temporary:
        with LoopbackSession(), KeychainSession(temporary) as session:
            result = subprocess.run(command, env=environment)
            if result.returncode:
                return result.returncode
            record = Path(temporary) / 'locked-entry.json'
            print('macOS qualification: seed owned vault entry', flush=True)
            subprocess.run([platform_executable, '--seed-vault', str(record)], env=environment, check=True, timeout=30)
            print('macOS qualification: lock owned keychain', flush=True)
            session.security('lock-keychain', session.path)
            print('macOS qualification: verify locked vault refuses access', flush=True)
            result = subprocess.run([platform_executable, '--locked-vault', str(record)], env=environment, timeout=30)
            print('macOS qualification: unlock owned keychain for cleanup', flush=True)
            session.security('unlock-keychain', '-p', session.password, session.path)
            return result.returncode


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--platform-executable', required=True)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    arguments = parser.parse_args()
    command = arguments.command
    if command[:1] == ['--']:
        command = command[1:]
    if not command:
        parser.error('A qualification command is required')
    try:
        return execute(command, arguments.platform_executable)
    except (RuntimeError, OSError, subprocess.SubprocessError):
        print('macOS qualification failed while preparing, running or restoring its isolated session', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
