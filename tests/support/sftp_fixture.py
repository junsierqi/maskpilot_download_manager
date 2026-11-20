"""Owned read-only SSH/SFTP server; virtual files only, no OS users/services or external hosts."""
import argparse
import base64
import contextlib
import hashlib
import hmac
import json
import logging
import os
from pathlib import Path
import posixpath
import socketserver
import stat
import sys
import threading
import time
from urllib.parse import quote

import paramiko

DATA = bytes(range(251)) * 32769
SHA = hashlib.sha256(DATA).hexdigest()
USER = 'owned-sftp-user'
PASSWORD = 'owned-sftp-password-private'
PASSPHRASE = 'owned-sftp-key-passphrase-private'
logging.getLogger('paramiko').setLevel(logging.CRITICAL)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, root):
        self.root = Path(root)
        self.root.mkdir(parents=True, exist_ok=True)
        self.host_key = paramiko.RSAKey.generate(2048)
        self.client_key = paramiko.RSAKey.generate(2048)
        self.private_key = self.root / '\u79c1\u94a5.pem'
        # Unicode is deliberate, including the encrypted private-key path.
        self.client_key.write_private_key_file(str(self.private_key), password=PASSPHRASE)
        self.fingerprint = base64.b64encode(hashlib.sha256(self.host_key.asbytes()).digest()).decode()
        self.lock = threading.RLock()
        self.transports = set()
        self.auth_none = self.passwords = self.keys = self.accepted = self.opens = 0
        self.reads = []
        self.modified = 1700000000
        self.thread = None
        super().__init__(('127.0.0.1', 0), Connection)
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)
        self.thread.start()

    def url(self, name='payload.bin', host='127.0.0.1'):
        return f'sftp://{host}:{self.server_address[1]}/' + quote(name)

    def snapshot(self):
        with self.lock:
            return {'none': self.auth_none, 'passwords': self.passwords, 'keys': self.keys,
                    'accepted': self.accepted, 'opens': self.opens, 'reads': list(self.reads)}

    def close(self):
        self.shutdown()
        with self.lock:
            connections = list(self.transports)
        for transport in connections:
            transport.close()
        self.server_close()
        self.thread.join(5)


class Login(paramiko.ServerInterface):
    def __init__(self, owner):
        self.owner = owner

    def get_allowed_auths(self, username):
        return 'password,publickey'

    def check_auth_none(self, username):
        with self.owner.lock:
            self.owner.auth_none += 1
        return paramiko.AUTH_FAILED

    def check_auth_password(self, username, password):
        accepted = hmac.compare_digest(username, USER) and hmac.compare_digest(password, PASSWORD)
        with self.owner.lock:
            self.owner.passwords += 1
            self.owner.accepted += int(accepted)
        return paramiko.AUTH_SUCCESSFUL if accepted else paramiko.AUTH_FAILED

    def check_auth_publickey(self, username, key):
        accepted = hmac.compare_digest(username, USER) and hmac.compare_digest(key.asbytes(), self.owner.client_key.asbytes())
        with self.owner.lock:
            self.owner.keys += 1
            self.owner.accepted += int(accepted)
        return paramiko.AUTH_SUCCESSFUL if accepted else paramiko.AUTH_FAILED

    def check_channel_request(self, kind, channel_id):
        return paramiko.OPEN_SUCCEEDED if kind == 'session' else paramiko.OPEN_FAILED_ADMINISTRATIVELY_PROHIBITED


class Handle(paramiko.SFTPHandle):
    def __init__(self, owner, name, data):
        super().__init__(os.O_RDONLY)
        self.owner, self.name, self.data = owner, name, data

    def stat(self):
        return attributes(self.owner, self.data)

    def read(self, offset, length):
        with self.owner.lock:
            if len(self.owner.reads) < 20000:
                self.owner.reads.append(offset)
            if self.name == '/changed.bin':
                self.owner.modified = 1700000001
        time.sleep(.01 if self.name == '/slow.bin' else .002)
        return self.data[offset:offset + min(length, 128 * 1024)]


def attributes(owner, data):
    result = paramiko.SFTPAttributes()
    result.st_size = len(data)
    result.st_mode = stat.S_IFREG | 0o444
    result.st_mtime = result.st_atime = owner.modified
    result.st_uid = result.st_gid = 1000
    return result


class Files(paramiko.SFTPServerInterface):
    def __init__(self, server, owner):
        super().__init__(server)
        self.owner = owner

    def canonicalize(self, path):
        return posixpath.normpath('/' + path.lstrip('/'))

    def data(self, path):
        if path in ('/payload.bin', '/slow.bin', '/changed.bin', '/\u6d4b\u8bd5.bin'):
            return DATA
        if path == '/empty.bin':
            return b''
        return None

    def stat(self, path):
        data = self.data(path)
        return attributes(self.owner, data) if data is not None else paramiko.SFTP_NO_SUCH_FILE

    lstat = stat

    def open(self, path, flags, attr):
        data = self.data(path)
        if data is None:
            return paramiko.SFTP_NO_SUCH_FILE
        if flags & (os.O_WRONLY | os.O_RDWR | os.O_CREAT | os.O_TRUNC | os.O_APPEND):
            return paramiko.SFTP_PERMISSION_DENIED
        with self.owner.lock:
            self.owner.opens += 1
        return Handle(self.owner, path, data)


class Connection(socketserver.BaseRequestHandler):
    def handle(self):
        transport = paramiko.Transport(self.request)
        with self.server.lock:
            self.server.transports.add(transport)
        try:
            transport.add_server_key(self.server.host_key)
            transport.set_subsystem_handler('sftp', paramiko.SFTPServer, Files, owner=self.server)
            transport.start_server(server=Login(self.server))
            while transport.is_active():
                time.sleep(.02)
        except (OSError, EOFError, paramiko.SSHException):
            pass
        finally:
            transport.close()
            with self.server.lock:
                self.server.transports.discard(transport)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--metadata', required=True, type=Path)
    args = parser.parse_args()
    server = Server(args.metadata.parent / 'keys')
    try:
        args.metadata.write_text(json.dumps({'url': server.url(), 'fingerprint': server.fingerprint,
            'privateKey': str(server.private_key), 'sha256': SHA}), encoding='utf-8')
        sys.stdin.readline()
    finally:
        server.close()


if __name__ == '__main__':
    main()
