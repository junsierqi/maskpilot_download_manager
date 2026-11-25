"""Local FTP and genuine explicit/implicit TLS fixture for the native product path.

The bounded server implements only the commands used by the download adapter; it
never accesses arbitrary filesystem paths. Certificates are generated per run.
"""
import argparse
import datetime
import hashlib
import os
from pathlib import Path
import shutil
import socket
import socketserver
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse

DATA = bytes(range(256)) * 32769
USER = 'ftp-fixture-user'
PASSWORD = 'ftp-fixture-password-private'
SHA = hashlib.sha256(DATA).hexdigest()


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, mode='plain', certificate=None, key=None, require_tls_session_reuse=False):
        self.mode = mode
        self.require_tls_session_reuse = require_tls_session_reuse
        self.tls_session_reuse = []
        self.tls_reuse_rejections = 0
        self.tls_data_events = []
        self.context = None
        if certificate:
            self.context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            self.context.minimum_version = ssl.TLSVersion.TLSv1_2
            self.context.load_cert_chain(certificate, key)
        self.commands = []
        self.transfers = []
        self.rest_rejections = 0
        self.modified = '20260101000000'
        self.files = {name: DATA for name in ['file.bin', 'slow.bin', 'norest.bin', 'auth.bin',
                                             'changed.bin', 'unknown.bin', 'short.bin', '中文.bin']}
        self.files['empty.bin'] = b''
        super().__init__(('127.0.0.1', 0), Handler)
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.shutdown()
        self.server_close()
        self.thread.join(timeout=5)

    def url(self, name, hostname=None):
        scheme = 'ftps' if self.mode == 'implicit' else 'ftp'
        host = hostname or ('localhost' if self.mode != 'plain' else '127.0.0.1')
        return f'{scheme}://{host}:{self.server_address[1]}/{urllib.parse.quote(name)}'


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        control = self.request
        passive = None
        reader = None
        try:
            if self.server.mode == 'implicit':
                control = self.server.context.wrap_socket(control, server_side=True)
            control.settimeout(30)
            reader = control.makefile('rb')
            protected = False
            active = None
            offset = 0
            user = ''
            logged_in = False
            last_name = ''

            def reply(value):
                control.sendall((value + '\r\n').encode('utf-8'))

            reply('220 Local transfer fixture ready')
            while True:
                line = reader.readline(65537)
                if not line or len(line) > 65536:
                    break
                verb, _, argument = line.decode('utf-8').rstrip('\r\n').partition(' ')
                verb = verb.upper()
                # Never retain the password, including in fixture diagnostics.
                self.server.commands.append((verb, '' if verb == 'PASS' else argument))
                name = argument.lstrip('/')
                if verb == 'AUTH':
                    if self.server.mode != 'explicit' or isinstance(control, ssl.SSLSocket):
                        reply('502 TLS not available')
                    else:
                        reply('234 Begin TLS')
                        reader.close()
                        control = self.server.context.wrap_socket(control, server_side=True)
                        reader = control.makefile('rb')
                elif verb == 'USER':
                    user = argument
                    reply('331 Password required')
                elif verb == 'PASS':
                    logged_in = user == 'anonymous' or (user == USER and argument == PASSWORD)
                    reply('230 Logged in' if logged_in else '530 Login rejected')
                elif verb == 'QUIT':
                    reply('221 Goodbye')
                    break
                elif verb == 'SYST':
                    reply('215 UNIX Type: L8')
                elif verb == 'FEAT':
                    reply('211-Features\r\n SIZE\r\n MDTM\r\n REST STREAM\r\n UTF8\r\n211 End')
                elif verb == 'PWD':
                    reply('257 "/" is the current directory')
                elif verb in ['TYPE', 'OPTS', 'CWD', 'PBSZ']:
                    reply('200 Accepted')
                elif verb == 'PROT':
                    protected = argument == 'P'
                    reply('200 Protection accepted')
                elif not logged_in:
                    reply('530 Login required')
                elif verb in ['SIZE', 'MDTM']:
                    last_name = name
                    if name not in self.server.files or (name == 'auth.bin' and user != USER):
                        reply('550 File unavailable')
                    elif name == 'unknown.bin':
                        reply('502 Metadata unavailable')
                    elif verb == 'SIZE':
                        reply('213 ' + str(len(self.server.files[name])))
                    else:
                        reply('213 ' + self.server.modified)
                elif verb in ['EPSV', 'PASV']:
                    if passive:
                        passive.close()
                    passive = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                    passive.bind(('127.0.0.1', 0))
                    passive.listen(1)
                    passive.settimeout(10)
                    port = passive.getsockname()[1]
                    active = None
                    reply(f'229 Entering Extended Passive Mode (|||{port}|)' if verb == 'EPSV'
                          else f'227 Entering Passive Mode (127,0,0,1,{port // 256},{port % 256})')
                elif verb in ['EPRT', 'PORT']:
                    if verb == 'EPRT':
                        fields = argument.split(argument[0])
                        host, port = fields[2], int(fields[3])
                    else:
                        fields = [int(value) for value in argument.split(',')]
                        host, port = '.'.join(map(str, fields[:4])), fields[4] * 256 + fields[5]
                    if host not in ['127.0.0.1', '::1']:
                        reply('501 Only loopback is allowed')
                    else:
                        active = (host, port)
                        reply('200 Active connection accepted')
                elif verb == 'REST':
                    if last_name == 'norest.bin':
                        self.server.rest_rejections += 1
                        offset = 0
                        reply('502 Restart not supported')
                    else:
                        offset = int(argument)
                        reply('350 Restart accepted')
                elif verb == 'RETR':
                    if name not in self.server.files or (name == 'auth.bin' and user != USER):
                        reply('550 File unavailable')
                        continue
                    if self.server.mode != 'plain' and not protected:
                        reply('534 Data protection required')
                        continue
                    if name == 'norest.bin' and offset:
                        reply('550 Restart refused')
                        offset = 0
                        continue
                    self.server.transfers.append((name, offset, isinstance(control, ssl.SSLSocket), protected))
                    reply('150 Opening data connection')
                    data = None
                    receipt = None
                    try:
                        if active:
                            data = socket.create_connection(active, timeout=10)
                        elif passive:
                            data, _ = passive.accept()
                        else:
                            raise RuntimeError('No data connection')
                        data.settimeout(10)
                        if protected:
                            receipt = dict(phase='handshake', sentBytes=0)
                            self.server.tls_data_events.append(receipt)
                            data = self.server.context.wrap_socket(data, server_side=True)
                            reused = data.session_reused
                            receipt.update(phase='payload', version=data.version(), reused=reused)
                            self.server.tls_session_reuse.append(reused)
                            if self.server.require_tls_session_reuse and not reused:
                                self.server.tls_reuse_rejections += 1
                                receipt['phase'] = 'rejected'
                                try:
                                    data.unwrap().close()
                                    receipt['tlsShutdownComplete'] = True
                                except (OSError, ssl.SSLError) as error:
                                    receipt['tlsShutdownError'] = type(error).__name__
                                reply('425 TLS session resumption required')
                                continue
                        body = self.server.files[name]
                        if name == 'short.bin':
                            body = body[:len(body) // 2]
                        for start in range(offset, len(body), 32768):
                            data.sendall(body[start:start + 32768])
                            if receipt is not None:
                                receipt['sentBytes'] += min(32768, len(body) - start)
                            if name in ['slow.bin', 'norest.bin']:
                                time.sleep(.004)
                        if name == 'changed.bin':
                            self.server.modified = '20260102000000'
                        if isinstance(data, ssl.SSLSocket):
                            receipt['phase'] = 'shutdown'
                            try:
                                data.unwrap().close()
                                receipt['tlsShutdownComplete'] = True
                            except (OSError, ssl.SSLError) as error:
                                receipt['tlsShutdownError'] = type(error).__name__
                            receipt['phase'] = 'complete'
                        reply('226 Transfer complete')
                    except (OSError, RuntimeError) as error:
                        if receipt is not None:
                            receipt.update(phase='interrupted', errorType=type(error).__name__)
                        reply('426 Transfer interrupted')
                    finally:
                        if data:
                            data.close()
                        if passive:
                            passive.close()
                            passive = None
                        offset = 0
                else:
                    reply('502 Command not supported')
        except (OSError, ValueError, UnicodeError):
            pass
        finally:
            if passive:
                passive.close()
            if reader:
                reader.close()
            control.close()


def certificates(root):
    certificate, key = root / 'fixture-ca.pem', root / 'fixture-key.pem'
    try:
        from cryptography import x509
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import rsa
        from cryptography.x509.oid import NameOID
        private = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'localhost')])
        now = datetime.datetime.now(datetime.timezone.utc)
        cert = (x509.CertificateBuilder().subject_name(subject).issuer_name(subject)
                .public_key(private.public_key()).serial_number(x509.random_serial_number())
                .not_valid_before(now - datetime.timedelta(minutes=5))
                .not_valid_after(now + datetime.timedelta(days=1))
                .add_extension(x509.SubjectAlternativeName([x509.DNSName('localhost')]), critical=False)
                .add_extension(x509.BasicConstraints(ca=True, path_length=0), critical=True)
                .sign(private, hashes.SHA256()))
        certificate.write_bytes(cert.public_bytes(serialization.Encoding.PEM))
        key.write_bytes(private.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                             serialization.NoEncryption()))
    except ImportError:
        openssl = shutil.which('openssl')
        if not openssl:
            candidate = Path(os.environ.get('ProgramFiles', 'C:/Program Files')) / 'Git/usr/bin/openssl.exe'
            openssl = str(candidate) if candidate.is_file() else None
        if not openssl:
            raise RuntimeError('FTP TLS tests require cryptography or an openssl executable; TLS tests are not skipped.')
        result = subprocess.run([openssl, 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                                 '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost',
                                 '-keyout', str(key), '-out', str(certificate)], capture_output=True, timeout=30)
        if result.returncode:
            raise RuntimeError('Cannot generate the temporary FTP TLS certificate.')
    return certificate, key


def cli_child_environment():
    environment = os.environ.copy()
    if os.environ.get('MASKPILOT_TEST_CLEAN_CLI_ENV') == '1':
        # The Windows os.environ mapping is case insensitive; its plain dict copy is not.
        system_root = os.environ['SystemRoot']
        environment['PATH'] = os.pathsep.join([os.path.join(system_root, 'System32'), system_root])
        for name in ['QT_ROOT_DIR', 'QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH',
                     'QML2_IMPORT_PATH', 'QT_QPA_PLATFORM']:
            environment.pop(name, None)
    return environment


def exercise(cli):
    cli = cli.resolve(strict=True)
    flags = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
    cli_environment = cli_child_environment()
    with tempfile.TemporaryDirectory(prefix='maskpilot-ftp-') as temporary:
        root = Path(temporary)
        certificate, key = certificates(root)
        sequence = 0

        def run(server, name='file.bin', options=(), expected=True, destination=None, data=None):
            nonlocal sequence
            sequence += 1
            target = destination or root / f'file-{sequence}.bin'
            state = data or root / f'data-{sequence}'
            result = subprocess.run([str(cli), server.url(name), str(target), str(state), *options],
                                    capture_output=True, text=True, timeout=35, creationflags=flags,
                                    env=cli_environment)
            assert (result.returncode == 0) == expected, (
                result.returncode, result.stdout, result.stderr,
                dict(mode=server.mode, ssl=ssl.OPENSSL_VERSION,
                     destinationPublished=target.exists(),
                     cacheBytes=sum(path.stat().st_size for path in (state / 'parts').rglob('*') if path.is_file()),
                     commandVerbs=[verb for verb, _ in server.commands],
                     transfers=server.transfers, tlsData=server.tls_data_events,
                     sessionReuse=server.tls_session_reuse, reuseRejections=server.tls_reuse_rejections))
            if expected:
                assert target.read_bytes() == server.files[name]
            else:
                assert not target.exists(), 'Failed transfer published a final file'
            return target, state, result

        def recover(server, name, digest=True, mutate=False):
            nonlocal sequence
            sequence += 1
            target, state = root / f'recovery-{sequence}.bin', root / f'recovery-{sequence}'
            arguments = ['--sha256', SHA] if digest else []
            process = subprocess.Popen([str(cli), server.url(name), str(target), str(state), *arguments],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                       creationflags=flags, env=cli_environment)
            try:
                deadline = time.monotonic() + 15
                offset = None
                while time.monotonic() < deadline:
                    candidates = list((state / 'parts').glob('*/offset-0')) if state.exists() else []
                    if candidates:
                        offset = candidates[0]
                        try:
                            if int(offset.read_text()) >= 1024 * 1024:
                                break
                        except (OSError, ValueError):
                            pass
                    assert process.poll() is None, process.communicate()
                    time.sleep(.01)
                assert offset and int(offset.read_text()) >= 1024 * 1024, 'No durable partial progress'
                process.kill()
                process.communicate(timeout=10)
                task_id = offset.parent.name
                if mutate:
                    server.files[name] = bytes(reversed(DATA))
                    server.modified = '20260201000000'
                before = len(server.transfers)
                rejections = server.rest_rejections
                result = subprocess.run([str(cli), '--resume', task_id, str(state)], capture_output=True,
                                        text=True, timeout=35, creationflags=flags, env=cli_environment)
                assert result.returncode == 0, (result.stdout, result.stderr)
                assert target.read_bytes() == server.files[name]
                resumed = server.transfers[before:]
                if not digest or mutate:
                    assert all(offset == 0 for _, offset, _, _ in resumed), resumed
                elif name != 'norest.bin':
                    assert any(offset > 0 for _, offset, _, _ in resumed), resumed
                else:
                    assert server.rest_rejections > rejections, 'REST-denied branch was not exercised'
                    assert resumed and all(offset == 0 for _, offset, _, _ in resumed), resumed
                return resumed
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate(timeout=10)

        plain = Server()
        try:
            run(plain)
            run(plain, 'empty.bin')
            run(plain, 'unknown.bin')
            run(plain, '中文.bin')
            run(plain, options=['--ftp-active'])
            print('PASS FTP passive/active, empty file and Unicode URL', flush=True)
            _, state, _ = run(plain, 'auth.bin', ['--ftp-user', USER, '--ftp-password', PASSWORD])
            for path in state.rglob('*'):
                if path.is_file():
                    assert PASSWORD.encode() not in path.read_bytes(), path
                    assert USER.encode() not in path.read_bytes(), path
            run(plain, 'auth.bin', ['4', '--ftp-user', USER, '--ftp-password', PASSWORD])
            run(plain, 'auth.bin', ['--ftp-user', USER, '--ftp-password', 'incorrect'], expected=False)
            print('PASS FTP authentication and credentials excluded from persistent bytes', flush=True)
            run(plain, options=['--sha256', '0' * 64], expected=False)
            run(plain, 'short.bin', expected=False)
            run(plain, 'missing.bin', expected=False)
            run(plain, 'changed.bin', expected=False)
            run(plain, options=['--ftp-explicit-tls'], expected=False)
            print('PASS FTP checksum/remote-change rejection and no TLS downgrade', flush=True)
            destination = root / 'existing.bin'
            destination.write_bytes(b'preserve existing')
            state = root / 'conflict-data'
            result = subprocess.run([str(cli), plain.url('file.bin'), str(destination), str(state)],
                                    capture_output=True, text=True, timeout=35, creationflags=flags,
                                    env=cli_environment)
            assert result.returncode == 0, (result.stdout, result.stderr)
            assert destination.read_bytes() == b'preserve existing'
            assert (root / 'existing (1).bin').read_bytes() == DATA
            print('PASS FTP filename conflict preserves existing bytes', flush=True)
            recover(plain, 'slow.bin')
            recover(plain, 'slow.bin', digest=False)
            recover(plain, 'norest.bin')
            # A no-digest recovery must fully restart after a remote version change.
            recover(plain, 'slow.bin', digest=False, mutate=True)
            print('PASS FTP force-exit recovery, verified REST, safe restart and REST-denied fallback', flush=True)
        finally:
            plain.close()
        for mode in ['explicit', 'implicit']:
            secure = Server(mode, certificate, key, require_tls_session_reuse=True)
            try:
                options = ['--ftp-ca', str(certificate)]
                if mode == 'explicit':
                    options.append('--ftp-explicit-tls')
                run(secure, options=options)
                assert secure.transfers[-1][2:] == (True, True)
                assert secure.tls_session_reuse and secure.tls_session_reuse[-1]
                untrusted = ['--ftp-explicit-tls'] if mode == 'explicit' else []
                run(secure, options=untrusted, expected=False)
                target, state = root / f'hostname-{mode}.bin', root / f'hostname-{mode}'
                result = subprocess.run([str(cli), secure.url('file.bin', '127.0.0.1'), str(target),
                                         str(state), *options], capture_output=True, text=True, timeout=35,
                                        creationflags=flags, env=cli_environment)
                assert result.returncode != 0 and not target.exists()
                print(f'PASS FTPS {mode}: encrypted control/data, required TLS session reuse, CA trust and hostname rejection', flush=True)
            finally:
                secure.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument('--exercise', type=Path)
    action.add_argument('--serve', action='store_true', help='Print one local URL; stop when stdin closes.')
    options = parser.parse_args()
    if options.serve:
        server = Server()
        try:
            print(server.url('file.bin'), flush=True)
            sys.stdin.buffer.read()
        finally:
            server.close()
    else:
        exercise(options.exercise)
