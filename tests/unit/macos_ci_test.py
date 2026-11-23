"""CI-session lifecycle tests using an injected security command, on any host."""
import importlib.util
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location('macos_session', Path(__file__).resolve().parents[2] / 'scripts/macos_test_session.py')
session = importlib.util.module_from_spec(spec)
spec.loader.exec_module(session)


class SecurityFixture:
    def __init__(self, fail=None):
        self.original = ['/owned runner/login.keychain-db', '/owned runner/secondary.keychain-db']
        self.paths = list(self.original)
        self.default = self.original[0]
        self.calls = []
        self.fail = fail
        self.failed = False
        self.created = None

    def __call__(self, command, **options):
        self.calls.append(command)
        name, args = command[1], command[2:]
        assert command[0] == '/usr/bin/security'
        assert options == dict(capture_output=True, text=True, timeout=30)
        output = ''
        if name == 'create-keychain':
            self.created = Path(args[-1])
            self.created.write_bytes(b'owned fake keychain')
        if name == self.fail and not self.failed:
            self.failed = True
            # Simulate an interrupted command after its owned file was created.
            if name == 'create-keychain':
                raise subprocess.TimeoutExpired('security', 30)
            return subprocess.CompletedProcess(command, 1, '', 'fake failure')
        if name == 'list-keychains':
            if '-s' in args:
                self.paths = args[args.index('-s') + 1:]
            output = '\n'.join(shlex.quote(value) for value in self.paths)
        elif name == 'default-keychain':
            if '-s' in args:
                self.default = args[-1]
            output = shlex.quote(self.default)
        elif name == 'delete-keychain':
            assert Path(args[-1]) == self.created, 'Only the owned disposable file may be deleted'
            self.created.unlink(missing_ok=True)
        return subprocess.CompletedProcess(command, 0, output, '')


class KeychainSessionTests(unittest.TestCase):
    def test_parse_quoted_paths_and_empty_list(self):
        self.assertEqual(session.keychain_paths('  "/owned runner/a"\n"/owned/b"'), ['/owned runner/a', '/owned/b'])
        self.assertEqual(session.keychain_paths(''), [])

    def test_reject_ambiguous_or_unsafe_paths(self):
        for value in ['"/unterminated', 'relative', '//remote/a', '"/bad\npath"', '/a /a', '"/bad\x7fpath"', '/a ' * 129, 'x' * 131073]:
            with self.subTest(value=value[:25]), self.assertRaises(RuntimeError):
                session.keychain_paths(value)

    def test_success_restores_exact_original_settings(self):
        fake = SecurityFixture()
        with tempfile.TemporaryDirectory() as directory:
            with session.KeychainSession(directory, fake) as owned:
                self.assertEqual(fake.paths, [str(owned.path)])
                self.assertEqual(fake.default, str(owned.path))
                self.assertTrue(owned.path.exists())
            self.assertEqual(fake.paths, fake.original)
            self.assertEqual(fake.default, fake.original[0])
            self.assertFalse(fake.created.exists())

    def test_failure_during_setup_always_restores(self):
        for operation in ['create-keychain', 'set-keychain-settings', 'unlock-keychain']:
            with self.subTest(operation=operation), tempfile.TemporaryDirectory() as directory:
                fake = SecurityFixture(operation)
                with self.assertRaises((RuntimeError, subprocess.TimeoutExpired)):
                    with session.KeychainSession(directory, fake):
                        self.fail('Failed setup entered the body')
                self.assertEqual(fake.paths, fake.original)
                self.assertEqual(fake.default, fake.original[0])
                self.assertFalse(fake.created.exists())

    def test_test_body_failure_restores_and_propagates(self):
        fake = SecurityFixture()
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, 'owned failure'):
                with session.KeychainSession(directory, fake):
                    raise ValueError('owned failure')
            self.assertEqual(fake.paths, fake.original)
            self.assertFalse(fake.created.exists())

    def test_failure_after_search_or_default_mutation_restores(self):
        for operation in ['list-keychains', 'default-keychain']:
            with self.subTest(operation=operation), tempfile.TemporaryDirectory() as directory:
                fake = SecurityFixture()
                injected = False

                def run(command, **options):
                    nonlocal injected
                    result = fake(command, **options)
                    if command[1] == operation and '-s' in command and not injected:
                        injected = True
                        return subprocess.CompletedProcess(command, 1, '', '')
                    return result

                with self.assertRaises(RuntimeError):
                    with session.KeychainSession(directory, run):
                        self.fail('Partially mutated setup entered the body')
                self.assertEqual(fake.paths, fake.original)
                self.assertEqual(fake.default, fake.original[0])
                self.assertFalse(fake.created.exists())

    def test_restore_failure_cannot_report_success(self):
        fake = SecurityFixture()
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(RuntimeError, 'restoration failed'):
                with session.KeychainSession(directory, fake):
                    fake.fail = 'default-keychain'
            self.assertFalse(fake.created.exists())

    def test_preexisting_file_is_never_deleted(self):
        fake = SecurityFixture()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'qualification.keychain-db'
            path.write_bytes(b'preserve')
            with self.assertRaises(RuntimeError):
                with session.KeychainSession(directory, fake):
                    pass
            self.assertEqual(path.read_bytes(), b'preserve')
            self.assertFalse(any(call[1] == 'delete-keychain' for call in fake.calls))

    def test_only_hosted_darwin_jobs_may_change_keychains(self):
        with tempfile.TemporaryDirectory() as directory:
            environment = dict(GITHUB_ACTIONS='true', RUNNER_OS='macOS', RUNNER_ENVIRONMENT='github-hosted', RUNNER_TEMP=directory)
            self.assertEqual(session.require_hosted_ci(environment, 'darwin'), directory)
            for system, key, value in [('win32', None, None), ('darwin', 'GITHUB_ACTIONS', 'false'),
                                       ('darwin', 'RUNNER_OS', 'Linux'), ('darwin', 'RUNNER_ENVIRONMENT', 'self-hosted'),
                                       ('darwin', 'RUNNER_TEMP', 'relative')]:
                changed = dict(environment)
                if key:
                    changed[key] = value
                with self.assertRaises(RuntimeError):
                    session.require_hosted_ci(changed, system)


class LoopbackFixture:
    def __init__(self, addresses=None, fail=None):
        self.addresses = set(addresses or ['127.0.0.1', '127.0.0.3'])
        self.original = set(self.addresses)
        self.fail = fail
        self.calls = []

    def __call__(self, command, **options):
        self.calls.append(command)
        assert options == dict(capture_output=True, text=True, timeout=30)
        if command == ['/sbin/ifconfig', 'lo0']:
            output = '\n'.join('  inet ' + address + ' netmask 0xffffffff' for address in sorted(self.addresses))
            return subprocess.CompletedProcess(command, 0, output, '')
        assert command[:4] == ['/usr/bin/sudo', '-n', '/sbin/ifconfig', 'lo0']
        assert command[5] == '127.0.0.2', 'Only the owned secondary address may change'
        operation = command[4]
        if operation == 'alias':
            assert command[6:] == ['netmask', '255.255.255.255']
            if self.fail == 'add':
                return subprocess.CompletedProcess(command, 1, '', '')
            self.addresses.add('127.0.0.2')
            if self.fail == 'timeout-after-add':
                raise subprocess.TimeoutExpired('ifconfig', 30)
        elif operation == '-alias':
            if self.fail == 'remove':
                return subprocess.CompletedProcess(command, 1, '', '')
            self.addresses.remove('127.0.0.2')
        else:
            raise AssertionError('Unexpected loopback mutation')
        return subprocess.CompletedProcess(command, 0, '', '')


class LoopbackSessionTests(unittest.TestCase):
    def owned(self, fake, probe=None):
        def bound_probe():
            self.assertIn('127.0.0.2', fake.addresses)
        return session.LoopbackSession(fake, probe or bound_probe)

    def test_success_restores_only_added_address(self):
        fake = LoopbackFixture()
        with self.owned(fake):
            self.assertEqual(fake.addresses, fake.original | {'127.0.0.2'})
        self.assertEqual(fake.addresses, fake.original)

    def test_existing_alias_is_never_modified_even_after_body_failure(self):
        fake = LoopbackFixture(['127.0.0.1', '127.0.0.2'])
        with self.assertRaises(ValueError), self.owned(fake):
            raise ValueError('owned body failure')
        self.assertEqual(fake.addresses, fake.original)
        self.assertFalse(any(command[0] == '/usr/bin/sudo' for command in fake.calls))

    def test_setup_failure_and_partial_timeout_restore(self):
        for fault in ['add', 'timeout-after-add']:
            with self.subTest(fault=fault):
                fake = LoopbackFixture(fail=fault)
                with self.assertRaises((RuntimeError, subprocess.TimeoutExpired)), self.owned(fake):
                    self.fail('Invalid fixture entered its body')
                self.assertEqual(fake.addresses, fake.original)

    def test_probe_failure_restores(self):
        fake = LoopbackFixture()
        def failed_probe():
            raise OSError('owned bind failure')
        with self.assertRaises(OSError), self.owned(fake, failed_probe):
            self.fail('Unavailable fixture entered its body')
        self.assertEqual(fake.addresses, fake.original)

    def test_body_failure_restores(self):
        fake = LoopbackFixture()
        with self.assertRaises(ValueError), self.owned(fake):
            raise ValueError('owned test failure')
        self.assertEqual(fake.addresses, fake.original)

    def test_restore_failure_is_never_success(self):
        fake = LoopbackFixture(fail='remove')
        with self.assertRaises(RuntimeError), self.owned(fake):
            pass
        self.assertIn('127.0.0.2', fake.addresses)

    def test_foreign_address_change_is_preserved_and_reported(self):
        fake = LoopbackFixture()
        with self.assertRaisesRegex(RuntimeError, 'restoration failed'), self.owned(fake):
            fake.addresses.add('127.0.0.4')
        self.assertEqual(fake.addresses, fake.original | {'127.0.0.4'})

    def test_non_hosted_execution_cannot_reach_loopback_changes(self):
        with mock.patch.object(session, 'LoopbackSession', side_effect=AssertionError('Unexpected mutation')):
            with self.assertRaises(RuntimeError):
                session.execute(['owned-command'], 'owned-platform', environment={}, system='win32')


if __name__ == '__main__':
    unittest.main()
