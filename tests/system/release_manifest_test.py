"""Deterministic local-baseline tests; synthetic binaries do not qualify actual PE runtime."""
import copy
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[2] / 'scripts/release_manifest.py'
spec = importlib.util.spec_from_file_location('maskpilot_release_manifest', SCRIPT)
manifest = importlib.util.module_from_spec(spec)
spec.loader.exec_module(manifest)


class BaselineTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='maskpilot-release-manifest-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.repo = self.root / 'repo'
        self.build = self.root / 'build'
        self.repo.mkdir()
        self.build.mkdir()
        self.write(self.repo / 'CMakeLists.txt', 'project(MaskPilot VERSION 0.3.0 LANGUAGES CXX)\n')
        self.write(self.repo / 'vcpkg.json', json.dumps({'version-string': '0.3.0'}))
        self.write(self.repo / 'vcpkg-configuration.json', '{}')
        self.write(self.repo / 'src/main.cpp', 'int owned_source;\n')
        self.write(self.repo / 'apps/cli/main.cpp', 'int owned_cli;\n')
        self.write(self.repo / 'database/migrations/0001.sql', 'SELECT 1;\n')
        self.write(self.repo / 'resources/windows/app.manifest',
                   '<assembly xmlns="urn:schemas-microsoft-com:asm.v1"><assemblyIdentity version="0.3.0.0"/></assembly>')
        for name in ('package.json', 'manifests/manifest.json', 'manifests/firefox.json'):
            self.write(self.repo / 'browser_extension' / name, json.dumps({'version': '0.3.0'}))
        self.write(self.repo / 'browser_extension/package-lock.json',
                   json.dumps({'version': '0.3.0', 'packages': {'': {'version': '0.3.0'}}}))
        self.write(self.build / 'CMakeCache.txt',
                   'CMAKE_BUILD_TYPE:STRING=Release\nMASKPILOT_ENABLE_ASAN:BOOL=OFF\n'
                   'CMAKE_PROJECT_VERSION:STATIC=0.3.0\nCMAKE_GENERATOR:INTERNAL=Ninja\n'
                   'CMAKE_CXX_COMPILER:FILEPATH=C:/owned/compiler.exe\n'
                   'CMAKE_HOME_DIRECTORY:INTERNAL=' + self.repo.as_posix() + '\n')
        for name in manifest.REQUIRED_RUNTIME:
            self.write(self.build / name, 'synthetic executable identity ' + name)
        self.write(self.build / 'native-runtime.dll', 'synthetic DLL')
        self.write(self.build / 'plugins/platforms/qwindows.dll', 'synthetic platform plugin')
        self.write(self.build / 'translations/qt_en.qm', 'synthetic translation')
        self.write(self.build / 'qt.conf', '[Paths]\nPlugins=plugins\n')
        self.tool = self.root / 'owned-parser.exe'
        self.write(self.tool, 'synthetic external parser')
        self.git('init', '-q')
        self.git('add', '.')
        self.git('-c', 'user.name=Owned Fixture', '-c', 'user.email=owned@fixture.invalid',
                 '-c', 'core.hooksPath=' + str(self.root / 'no-hooks'),
                 '-c', 'commit.gpgsign=false', 'commit', '-qm', 'Create owned fixture')
        # File-version API is a unit seam only. Actual baseline creation checks the five real Windows PE resources.
        self.version_patch = patch.object(manifest, '_binary_version', return_value='0.3.0.0')
        self.version_patch.start()
        self.addCleanup(self.version_patch.stop)
        self.baseline = manifest.capture(self.repo, self.build, {'parser': self.tool})

    @staticmethod
    def write(path, text):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding='utf-8')

    def git(self, *arguments):
        subprocess.run(['git', '-C', str(self.repo), *arguments], check=True, capture_output=True, timeout=15,
                       creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))

    def verify(self, value=None):
        manifest.verify(self.baseline if value is None else value, self.repo, self.build)

    def test_roundtrip_inventory_and_tooling_only_commit_remain_compatible(self):
        self.verify(json.loads(json.dumps(self.baseline)))
        self.assertEqual(set(self.baseline['runtime']['maskpilot.exe']), {'sha256', 'size'})
        self.assertEqual(set(self.baseline['external']['parser']), {'path', 'sha256', 'size'})
        self.assertIsInstance(self.baseline['sources']['src/main.cpp'], str)
        self.write(self.repo / 'scripts/new-tool.py', 'print("owned tooling")\n')
        self.write(self.repo / 'tests/new-test.py', '# owned fixture\n')
        self.git('add', '.')
        self.git('-c', 'user.name=Owned Fixture', '-c', 'user.email=owned@fixture.invalid',
                 '-c', 'core.hooksPath=' + str(self.root / 'no-hooks'),
                 '-c', 'commit.gpgsign=false', 'commit', '-qm', 'Change release tooling only')
        self.verify()

    def test_native_source_hash_missing_and_new_set_are_strict(self):
        source = self.repo / 'src/main.cpp'
        original = source.read_bytes()
        source.write_bytes(b'changed native source')
        with self.assertRaises(ValueError): self.verify()
        source.write_bytes(original)
        source.unlink()
        with self.assertRaises((ValueError, OSError)): self.verify()
        source.write_bytes(original)
        extra = self.repo / 'src/new.cpp'
        self.write(extra, 'new native material')
        with self.assertRaises(ValueError): self.verify()
        self.git('add', 'src/new.cpp')
        with self.assertRaises(ValueError): self.verify()

    def test_runtime_added_removed_changed_and_plugin_translation_drift_reject(self):
        for relative in ('maskpilot.exe', 'plugins/platforms/qwindows.dll', 'translations/qt_en.qm', 'qt.conf'):
            with self.subTest(relative=relative):
                path = self.build / relative
                original = path.read_bytes()
                path.write_bytes(b'changed runtime')
                with self.assertRaises(ValueError): self.verify()
                path.write_bytes(original)
        extra = self.build / 'extra.dll'
        self.write(extra, 'new runtime')
        with self.assertRaises(ValueError): self.verify()
        extra.unlink()
        (self.build / 'media_guard/ffprobe.exe').unlink()
        with self.assertRaises(ValueError): self.verify()

    def test_test_executables_and_build_intermediates_are_not_runtime(self):
        self.write(self.build / 'tests/owned-test.exe', 'not a shipped process')
        self.write(self.build / 'CMakeFiles/intermediate.dll', 'not runtime')
        self.verify()

    def test_external_tool_drift_is_independent_of_compiled_runtime(self):
        self.tool.write_bytes(b'different parser')
        with self.assertRaises(ValueError): self.verify()

    def test_release_cache_version_instrumentation_and_source_root_reject(self):
        path = self.build / 'CMakeCache.txt'
        original = path.read_text()
        for changed in (original.replace('=Release', '=Debug'), original.replace('ASAN:BOOL=OFF', 'ASAN:BOOL=ON'),
                        original.replace('VERSION:STATIC=0.3.0', 'VERSION:STATIC=0.2.0'),
                        original + 'CMAKE_CXX_FLAGS:STRING=/fsanitize=address\n',
                        original.replace(self.repo.as_posix(), self.build.as_posix())):
            with self.subTest(cache=changed):
                path.write_text(changed)
                with self.assertRaises(ValueError): self.verify()
        path.write_text(original)
        with patch.object(manifest, '_binary_version', return_value='0.2.0.0'):
            with self.assertRaises(ValueError): self.verify()

    def test_synchronized_extension_assembly_and_dependency_versions_reject_drift(self):
        for relative in ('browser_extension/manifests/firefox.json', 'browser_extension/package-lock.json',
                         'vcpkg.json', 'resources/windows/app.manifest'):
            path = self.repo / relative
            original = path.read_text()
            path.write_text(original.replace('0.3.0', '0.2.0'))
            with self.assertRaises(ValueError): self.verify()
            path.write_text(original)

    def test_manifest_paths_shapes_unsafe_numbers_and_environment_reject(self):
        for name in ('../escape.dll', '/absolute.dll', 'media_guard/../escape.exe', 'bad\\file.dll'):
            value = copy.deepcopy(self.baseline)
            value['runtime'][name] = value['runtime']['maskpilot.exe']
            with self.assertRaises(ValueError): self.verify(value)
        value = copy.deepcopy(self.baseline)
        value['runtime']['maskpilot.exe']['size'] = True
        with self.assertRaises(ValueError): self.verify(value)
        value = copy.deepcopy(self.baseline)
        value['external']['parser']['path'] = '../relative-tool'
        with self.assertRaises(ValueError): self.verify(value)
        value = copy.deepcopy(self.baseline)
        value['environment']['python'] = 'unexpected interpreter'
        with self.assertRaises(ValueError): self.verify(value)
        value = copy.deepcopy(self.baseline)
        value['extra'] = 'unknown authority'
        with self.assertRaises(ValueError): self.verify(value)

    def test_atomic_create_never_overwrites_prior_baseline_or_leaves_temporary_file(self):
        output = self.root / 'baseline.json'
        manifest._write_new(output, self.baseline)
        original = output.read_bytes()
        with self.assertRaises(FileExistsError): manifest._write_new(output, {'changed': True})
        self.assertEqual(output.read_bytes(), original)
        self.assertFalse(list(self.root.glob('.qualification-*.tmp')))


@unittest.skipUnless(os.name == 'nt', 'The real Windows harness imports Windows process APIs')
class LoadedModulePolicyTest(unittest.TestCase):
    def test_system_exemption_excludes_windows_temporary_and_other_writable_directories(self):
        source = SCRIPT.parents[1] / 'benchmarks/run.py'
        module_spec = importlib.util.spec_from_file_location('maskpilot_module_policy_fixture', source)
        harness = importlib.util.module_from_spec(module_spec)
        module_spec.loader.exec_module(harness)
        system = Path('C:/Windows')
        for directory in ('System32', 'SysWOW64', 'WinSxS'):
            self.assertTrue(harness.is_system_module(system / directory / 'owned.dll', system))
        for path in ('Temp/unqualified.dll', 'Tasks/unqualified.dll', 'unqualified.dll',
                     'System32-neighbor/unqualified.dll'):
            self.assertFalse(harness.is_system_module(system / path, system))


if __name__ == '__main__':
    unittest.main()
