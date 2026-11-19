"""Deterministic PE/signature-seam fixtures; these do NOT qualify real vendor binaries."""
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import zipfile


SCRIPT = Path(__file__).resolve().parents[2] / 'scripts/audit_release_paths.py'
SPEC = importlib.util.spec_from_file_location('maskpilot_release_paths', SCRIPT)
audit = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audit)


def synthetic_pe(pdb='fixture.pdb', kind=b'RSDS'):
    """Tiny structurally bounded x64 PE/debug fixture, never an executable proof."""
    data = bytearray(0xa00)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 60, 0x80)
    data[0x80:0x84] = b'PE\0\0'
    struct.pack_into('<HH', data, 0x84, 0x8664, 1)
    struct.pack_into('<H', data, 0x94, 240)
    optional = 0x98
    struct.pack_into('<H', data, optional, 0x20b)
    struct.pack_into('<I', data, optional + 60, 0x200)
    struct.pack_into('<I', data, optional + 108, 16)
    struct.pack_into('<II', data, optional + 160, 0x1000, 28)
    section = optional + 240
    data[section:section + 8] = b'.rdata\0\0'
    struct.pack_into('<IIII', data, section + 8, 0x800, 0x1000, 0x800, 0x200)
    payload = kind + bytes(16) + struct.pack('<I', 1) if kind == b'RSDS' else kind + bytes(8) + struct.pack('<I', 1)
    payload += pdb.encode('utf-8') + b'\0'
    struct.pack_into('<IIII', data, 0x200 + 12, 2, len(payload), 0x1100, 0x300)
    data[0x300:0x300 + len(payload)] = payload
    diagnostic = b'src/fixture/relative.cpp\0'
    data[0x600:0x600 + len(diagnostic)] = diagnostic
    return bytes(data)


class PeParserTests(unittest.TestCase):
    def test_actual_debug_directory_rsds_and_nb10(self):
        for kind in (b'RSDS', b'NB10'):
            self.assertEqual(audit.pe_codeview(synthetic_pe(kind=kind)),
                             [{'format': kind.decode(), 'pdb': 'fixture.pdb', 'age': 1}])

    def test_blind_rsds_bytes_are_not_a_debug_directory(self):
        image = bytearray(synthetic_pe())
        struct.pack_into('<II', image, 0x98 + 160, 0, 0)
        self.assertIn(b'RSDS', image)
        self.assertEqual(audit.pe_codeview(bytes(image)), [])

    def test_malformed_offsets_rvas_sections_and_payloads(self):
        changes = [(60, '<I', 0xfffffff0), (0x86, '<H', 0), (0x98, '<H', 0x10b),
                   (0x98 + 160, '<I', 0x500000), (0x98 + 164, '<I', 27),
                   (0x200 + 24, '<I', 0xffffff00), (0x200 + 20, '<I', 0x1200),
                   (0x200 + 16, '<I', 65537)]
        for offset, fmt, value in changes:
            with self.subTest(offset=offset):
                image = bytearray(synthetic_pe())
                struct.pack_into(fmt, image, offset, value)
                with self.assertRaises(ValueError):
                    audit.pe_codeview(bytes(image))
        with self.assertRaises(ValueError):
            audit.pe_codeview(synthetic_pe()[:600])
        image = bytearray(synthetic_pe())
        image[0x300:0x304] = b'FAKE'
        with self.assertRaises(ValueError):
            audit.pe_codeview(bytes(image))

    def test_private_prefix_encodings_slashes_and_stream_boundaries(self):
        prefix = r'D:\private-work\owner'
        patterns = audit._patterns((prefix,))
        values = (prefix, prefix.replace('\\', '/'), prefix.upper(),
                  json.dumps(prefix)[1:-1], prefix.replace('\\', '\\\\\\\\'))
        for value in values:
            for encoding in ('utf-8', 'utf-16-le', 'utf-16-be'):
                with self.subTest(value=value, encoding=encoding), self.assertRaises(ValueError):
                    audit._scan(io.BytesIO(b'x' * (1024 * 1024 - 3) + value.encode(encoding)), patterns, 'fixture.dll')
        audit._scan(io.BytesIO(b'C:\\Windows\\System32 and src/relative.cpp'), patterns, 'relative.dll')


class ArtifactTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='maskpilot-release-audit-fixtures-')
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.repo, self.runtime = self.base / 'repo', self.base / 'runtime'
        self.qt, self.crt = self.base / 'sdk', self.base / 'redist'
        self.repo.mkdir()
        self.runtime.mkdir()
        self.crt.mkdir()
        self.catalog = {'qtVersion': '6.11.3', 'archives': []}
        self.write(self.repo / 'third_party/sources.json', json.dumps(self.catalog).encode())
        for name in ('LICENSE.vcpkg.txt', 'libxml2/vcpkg.json', 'libxml2/portable-default-catalog.patch',
                     'openssl/vcpkg.json', 'openssl/portable-buildinfo.patch'):
            self.write(self.repo / 'cmake/ports' / name, ('fixture portable source ' + name).encode())
        for name in audit.LOCAL_PE:
            self.write(self.runtime / name, synthetic_pe(Path(name).stem + '.pdb'))
        for name in audit.QT_DLLS:
            self.write(self.qt / 'bin' / name, synthetic_pe('C:/public-vendor/qt/' + name + '.pdb'))
        for name in audit.QT_PLUGINS:
            self.write(self.qt / 'plugins' / name, synthetic_pe('C:/public-vendor/qt/' + Path(name).name + '.pdb'))
        self.write(self.qt / 'translations/qtbase_zh_CN.qm', b'fixture translation bytes')
        for name in ('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll'):
            self.write(self.crt / name, synthetic_pe('F:/public-buildbot/crt/' + name + '.pdb'))
        for name, (source, _, _) in audit._supplier_files(self.qt, self.crt).items():
            self.write(self.runtime / name, source.read_bytes())
        self.write(self.runtime / 'qt.conf', b'[Paths]\nPrefix=.\nPlugins=plugins\nTranslations=translations\n')
        self.signatures = patch.object(audit, '_signatures', side_effect=self.fixture_signatures)
        self.signatures.start()
        self.addCleanup(self.signatures.stop)

    @staticmethod
    def write(path, data):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    @staticmethod
    def fixture_signatures(paths):
        # Explicit fixture seam: no trust claim for the synthetic PE bytes.
        return [{'valid': True,
                 'subject': 'CN=The Qt Company Oy, O=The Qt Company Oy' if path.name not in audit.CRT_DLLS else 'CN=Microsoft Corporation, O=Microsoft Corporation',
                 'version': '6.11.3.0' if path.name not in audit.CRT_DLLS else '14.44.35211.0'} for path in paths]

    def check_runtime(self):
        return audit.audit_runtime(self.repo, self.runtime, self.qt, self.crt)

    def inventory(self):
        records = []
        for path in sorted(self.runtime.rglob('*')):
            if path.is_file() and path.name != 'SHA256SUMS.txt':
                records.append(hashlib.sha256(path.read_bytes()).hexdigest() + '  ' + path.relative_to(self.runtime).as_posix())
        self.write(self.runtime / 'SHA256SUMS.txt', ('\n'.join(records) + '\n').encode())

    def package(self):
        blob = b'pinned synthetic source archive (not actual Qt source qualification)'
        self.catalog['archives'] = [{'file': 'qtbase-fixture.tar.xz', 'size': len(blob), 'sha256': hashlib.sha256(blob).hexdigest()}]
        self.write(self.repo / 'third_party/sources.json', json.dumps(self.catalog).encode())
        self.write(self.runtime / 'dependency-sources/qtbase-fixture.tar.xz', blob)
        self.write(self.runtime / 'dependency-sources/sources.json', (self.repo / 'third_party/sources.json').read_bytes())
        for source in (self.repo / 'cmake/ports').rglob('*'):
            if source.is_file():
                self.write(self.runtime / 'dependency-sources/portable-ports' / source.relative_to(self.repo / 'cmake/ports'), source.read_bytes())
        self.write(self.runtime / 'README.md', b'Example C:\\example\\application, relative runtime configuration.\n')
        self.inventory()

    def test_runtime_includes_only_fixed_modules_not_private_build_evidence(self):
        self.write(self.runtime / 'CMakeCache.txt', str(self.repo).encode())
        report = self.check_runtime()
        self.assertEqual(report['privatePrefixFindings'], 0)
        self.assertEqual(report['fileCount'], len(audit.LOCAL_PE) + len(audit._supplier_files(self.qt, self.crt)) + 1)
        self.assertTrue(report['supplierExceptions'])

    def test_vs_toolchain_prefix_is_derived_without_bootstrap_environment(self):
        redist = self.base / 'VisualStudio/VC/Redist/MSVC/14.44.1/x64/Microsoft.VC143.CRT'
        optional = ('VCToolsInstallDir', 'VSINSTALLDIR', 'WindowsSdkDir', 'QT_ROOT_DIR',
                    'VCPKG_ROOT', 'VCPKG_INSTALLED_DIR')
        with patch.dict(audit.os.environ, {key: '' for key in optional}):
            prefixes = audit._private_prefixes(self.repo, self.qt, redist)
        self.assertIn(str(self.base / 'VisualStudio'), prefixes)

    def test_absolute_local_pdb_is_not_excused_by_a_valid_fixture_signature(self):
        self.write(self.runtime / 'maskpilot.exe', synthetic_pe(r'Z:\foreign-host\maskpilot.pdb'))
        with self.assertRaisesRegex(ValueError, 'basename-only'):
            self.check_runtime()

    def test_supplier_copy_tamper_or_wrong_identity_is_rejected(self):
        original = (self.runtime / 'Qt6Core.dll').read_bytes()
        self.write(self.runtime / 'Qt6Core.dll', original + b'tampered')
        with self.assertRaisesRegex(ValueError, 'supplier bytes'):
            self.check_runtime()
        self.write(self.runtime / 'Qt6Core.dll', original)
        with patch.object(audit, '_signatures', return_value=[{'valid': True, 'subject': 'CN=Wrong Publisher', 'version': '6.11.3.0'}] * 16):
            with self.assertRaises(ValueError):
                self.check_runtime()

    def test_our_host_path_is_never_bypassed_for_a_supplier(self):
        image = synthetic_pe() + str(self.repo).encode('utf-16-le')
        self.write(self.qt / 'bin/Qt6Core.dll', image)
        self.write(self.runtime / 'Qt6Core.dll', image)
        with self.assertRaisesRegex(ValueError, 'private host prefix'):
            self.check_runtime()

    def test_additional_pe_or_absolute_runtime_configuration_is_rejected(self):
        self.write(self.runtime / 'extra.dll', synthetic_pe())
        with self.assertRaisesRegex(ValueError, 'inventory differs'):
            self.check_runtime()
        (self.runtime / 'extra.dll').unlink()
        self.write(self.runtime / 'qt.conf', b'[Paths]\nPrefix=C:/somewhere\n')
        with self.assertRaisesRegex(ValueError, 'relative deployment'):
            self.check_runtime()

    def test_complete_package_and_archive_match_source_pins(self):
        self.package()
        report = audit.audit_directory(self.repo, self.runtime, self.qt, self.crt)
        self.assertEqual(report['verifiedSourceArchives'], 1)
        self.assertEqual(report['verifiedPortablePortFiles'], 5)
        zipped = self.base / 'fixture.zip'
        with zipfile.ZipFile(zipped, 'w') as archive:
            for path in self.runtime.rglob('*'):
                if path.is_file():
                    archive.write(path, 'MaskPilot-fixture/' + path.relative_to(self.runtime).as_posix())
        result = audit.audit_archive(self.repo, zipped, self.qt, self.crt)
        self.assertEqual(result['files'], report['files'])

    def test_packaged_catalog_and_port_sources_match_exact_repository_set(self):
        self.package()
        catalog_copy = self.runtime / 'dependency-sources/sources.json'
        self.write(catalog_copy, b'{"changed":true}')
        self.inventory()
        with self.assertRaisesRegex(ValueError, 'source catalog differs'):
            audit.audit_directory(self.repo, self.runtime, self.qt, self.crt)
        self.write(catalog_copy, (self.repo / 'third_party/sources.json').read_bytes())
        port = self.runtime / 'dependency-sources/portable-ports/openssl/portable-buildinfo.patch'
        self.write(port, b'modified portable patch')
        self.inventory()
        with self.assertRaisesRegex(ValueError, 'portable-port source'):
            audit.audit_directory(self.repo, self.runtime, self.qt, self.crt)
        self.write(port, (self.repo / 'cmake/ports/openssl/portable-buildinfo.patch').read_bytes())
        unknown = self.runtime / 'dependency-sources/portable-ports/unknown.patch'
        self.write(unknown, b'unknown source')
        self.inventory()
        with self.assertRaisesRegex(ValueError, 'portable-port source'):
            audit.audit_directory(self.repo, self.runtime, self.qt, self.crt)
        unknown.unlink()
        port.unlink()
        self.inventory()
        with self.assertRaisesRegex(ValueError, 'source set is incomplete'):
            audit.audit_directory(self.repo, self.runtime, self.qt, self.crt)

    def test_package_private_artifacts_inventory_and_source_substitution_fail(self):
        self.package()
        for name in ('debug.pdb', 'CMakeCache.txt', 'data/tasks.db', 'qualification-private.json', '.git/config', 'downloads/test.bin'):
            with self.subTest(name=name):
                self.write(self.runtime / name, b'fixture private payload')
                self.inventory()
                with self.assertRaises(ValueError):
                    audit.audit_directory(self.repo, self.runtime, self.qt, self.crt)
                (self.runtime / name).unlink()
        self.inventory()
        self.write(self.runtime / 'README.md', b'changed after inventory')
        with self.assertRaisesRegex(ValueError, 'inventory hash'):
            audit.audit_directory(self.repo, self.runtime, self.qt, self.crt)
        self.write(self.runtime / 'dependency-sources/qtbase-fixture.tar.xz', b'substituted source archive')
        self.inventory()
        with self.assertRaisesRegex(ValueError, 'unpinned'):
            audit.audit_directory(self.repo, self.runtime, self.qt, self.crt)

    def test_private_prefix_is_rejected_in_package_text(self):
        self.package()
        self.write(self.runtime / 'README.md', json.dumps(str(self.repo)).encode())
        self.inventory()
        with self.assertRaisesRegex(ValueError, 'private host prefix'):
            audit.audit_directory(self.repo, self.runtime, self.qt, self.crt)

    def test_zip_traversal_case_collision_prefix_links_and_expansion_bounds(self):
        cases = [('../escaped', 'x'), ('pkg/a', 'PKG/A'), ('pkg/a', 'pkg/a/child')]
        for number, names in enumerate(cases):
            archive = self.base / ('malformed-' + str(number) + '.zip')
            with zipfile.ZipFile(archive, 'w') as zipped:
                for name in names:
                    zipped.writestr(name, b'fixture')
            with self.assertRaises(ValueError):
                audit.audit_archive(self.repo, archive, self.qt, self.crt)
        archive = self.base / 'link.zip'
        with zipfile.ZipFile(archive, 'w') as zipped:
            entry = zipfile.ZipInfo('pkg/link')
            entry.create_system = 3
            entry.external_attr = 0o120777 << 16
            zipped.writestr(entry, b'../elsewhere')
        with self.assertRaises(ValueError):
            audit.audit_archive(self.repo, archive, self.qt, self.crt)
        archive = self.base / 'too-large.zip'
        with zipfile.ZipFile(archive, 'w') as zipped:
            zipped.writestr('pkg/large', b'x' * 128)
        with patch.object(audit, 'MAX_TOTAL', 64), self.assertRaises(ValueError):
            audit.audit_archive(self.repo, archive, self.qt, self.crt)


if __name__ == '__main__':
    unittest.main()
