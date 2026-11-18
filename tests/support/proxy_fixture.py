"""Bounded recording HTTP/SOCKS5 proxy for real loopback-only transfer tests."""
import argparse
import base64
import hmac
import ipaddress
import json
import os
from pathlib import Path
import select
import socket
import socketserver
import sys
import threading
import time
import urllib.parse


def exact(connection, count):
    result = bytearray()
    while len(result) < count:
        block = connection.recv(count - len(result))
        if not block:
            raise EOFError('Incomplete proxy protocol message')
        result.extend(block)
    return bytes(result)


class RecordingProxy(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = False
    block_on_close = True

    def __init__(self, kind='http', username='', password='', state=None, mappings=None):
        if kind not in ('http', 'socks5'):
            raise ValueError('Unsupported fixture proxy type')
        self.kind, self.username, self.password = kind, username, password
        self.state_path = Path(state) if state else None
        self.mappings = dict(mappings or {})
        for host, address in self.mappings.items():
            if not host.endswith('.invalid') or not ipaddress.ip_address(address).is_loopback:
                raise ValueError('Mappings must explicitly map .invalid names to loopback addresses')
        self.lock = threading.Lock()
        self.connections = set()
        self.stopping = threading.Event()
        self.last_save = 0.0
        self.state = dict(destinations=[], auth_attempts=0, auth_accepted=0,
                          auth_rejected=0, forwarded_bytes=0)
        super().__init__(('127.0.0.1', 0), ProxyHandler)
        self.host, self.port = self.server_address
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)
        self.thread.start()
        self.save()

    def url(self):
        return f'{self.kind}://{self.host}:{self.port}'

    def save(self):
        with self.lock:
            self._save_locked()

    def _save_locked(self):
        if self.state_path:
            self.state_path.parent.mkdir(parents=True, exist_ok=True)
            staging = self.state_path.with_name(self.state_path.name + '.new')
            staging.write_text(json.dumps(self.state, sort_keys=True), encoding='utf-8')
            os.replace(staging, self.state_path)
        self.last_save = time.monotonic()

    def snapshot(self):
        with self.lock:
            return json.loads(json.dumps(self.state))

    def authenticate(self, username, password):
        accepted = hmac.compare_digest(username, self.username.encode()) and hmac.compare_digest(
            password, self.password.encode())
        with self.lock:
            self.state['auth_attempts'] += 1
            self.state['auth_accepted' if accepted else 'auth_rejected'] += 1
            self._save_locked()
        return accepted

    def connect_target(self, host, port, command):
        if not host or not 1 <= port <= 65535 or len(host) > 253:
            raise ValueError('Invalid fixture destination')
        address = self.mappings.get(host.lower(), host)
        if address.lower() == 'localhost':
            address = '127.0.0.1'
        if not ipaddress.ip_address(address).is_loopback:
            raise ValueError('Fixture refuses non-loopback destinations')
        with self.lock:
            if len(self.state['destinations']) >= 2048:
                raise ValueError('Fixture destination limit exceeded')
            self.state['destinations'].append(dict(host=host, port=port, command=command))
            self._save_locked()
        upstream = socket.create_connection((address, port), timeout=5)
        upstream.settimeout(5)
        with self.lock:
            self.connections.add(upstream)
        return upstream

    def forwarded(self, count):
        with self.lock:
            self.state['forwarded_bytes'] += count
            if time.monotonic() - self.last_save > .1:
                self._save_locked()

    def relay(self, client, upstream):
        sockets = [client, upstream]
        deadline = time.monotonic() + 90
        last_data = time.monotonic()
        while not self.stopping.is_set() and time.monotonic() < deadline:
            readable, _, _ = select.select(sockets, [], [], .1)
            if not readable:
                if time.monotonic() - last_data > 20:
                    return
                continue
            for source in readable:
                data = source.recv(65536)
                if not data:
                    return
                target = upstream if source is client else client
                target.sendall(data)
                self.forwarded(len(data))
                last_data = time.monotonic()

    def close(self):
        self.stopping.set()
        self.shutdown()
        with self.lock:
            connections = list(self.connections)
        for connection in connections:
            try:
                connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            connection.close()
        self.server_close()
        self.thread.join(timeout=5)
        self.save()

    def handle_error(self, request, client_address):
        # Protocol rejections and client disconnects are expected negative test paths.
        pass


class ProxyHandler(socketserver.BaseRequestHandler):
    def handle(self):
        upstream = None
        self.request.settimeout(5)
        with self.server.lock:
            self.server.connections.add(self.request)
        try:
            if self.server.kind == 'socks5':
                upstream = self.socks()
            else:
                upstream = self.http()
            if upstream:
                self.server.relay(self.request, upstream)
        except (OSError, EOFError, ValueError, UnicodeError):
            pass
        finally:
            if upstream:
                upstream.close()
            with self.server.lock:
                self.server.connections.discard(self.request)
                self.server.connections.discard(upstream)
                self.server._save_locked()

    def socks(self):
        version, count = exact(self.request, 2)
        if version != 5 or count == 0:
            return None
        methods = exact(self.request, count)
        method = 2 if self.server.username or self.server.password else 0
        if method not in methods:
            self.request.sendall(b'\x05\xff')
            return None
        self.request.sendall(bytes([5, method]))
        if method == 2:
            version, length = exact(self.request, 2)
            username = exact(self.request, length)
            password = exact(self.request, exact(self.request, 1)[0])
            accepted = version == 1 and self.server.authenticate(username, password)
            self.request.sendall(bytes([1, 0 if accepted else 1]))
            if not accepted:
                return None
        version, command, reserved, address_type = exact(self.request, 4)
        if (version, command, reserved) != (5, 1, 0):
            return None
        if address_type == 1:
            host = socket.inet_ntop(socket.AF_INET, exact(self.request, 4))
        elif address_type == 4:
            host = socket.inet_ntop(socket.AF_INET6, exact(self.request, 16))
        elif address_type == 3:
            host = exact(self.request, exact(self.request, 1)[0]).decode('ascii')
        else:
            return None
        port = int.from_bytes(exact(self.request, 2), 'big')
        try:
            upstream = self.server.connect_target(host, port, 'SOCKS5')
        except (OSError, ValueError):
            self.request.sendall(b'\x05\x04\x00\x01' + b'\x00' * 6)
            return None
        self.request.sendall(b'\x05\x00\x00\x01' + b'\x00' * 6)
        return upstream

    def http(self):
        header = bytearray()
        while not header.endswith(b'\r\n\r\n'):
            if len(header) >= 65536:
                raise ValueError('Proxy headers exceed bound')
            header.extend(exact(self.request, 1))
        lines = bytes(header).decode('iso-8859-1').split('\r\n')
        method, target, version = lines[0].split(' ')
        if version not in ('HTTP/1.0', 'HTTP/1.1'):
            raise ValueError('Invalid HTTP version')
        fields = [line.partition(':') for line in lines[1:] if line]
        credentials = [value.strip() for name, separator, value in fields
                       if name.lower() == 'proxy-authorization']
        if self.server.username or self.server.password:
            username = password = b''
            if len(credentials) == 1 and credentials[0].startswith('Basic '):
                try:
                    username, password = base64.b64decode(credentials[0][6:], validate=True).split(b':', 1)
                except (ValueError, base64.binascii.Error):
                    pass
            if not self.server.authenticate(username, password):
                self.request.sendall(b'HTTP/1.1 407 Proxy Authentication Required\r\n'
                                     b'Proxy-Authenticate: Basic realm="local-fixture"\r\n'
                                     b'Content-Length: 0\r\nConnection: close\r\n\r\n')
                return None
        if method == 'CONNECT':
            parsed = urllib.parse.urlsplit('//' + target)
            host, port = parsed.hostname, parsed.port
            if not host or not port or parsed.path:
                raise ValueError('Invalid CONNECT authority')
        else:
            parsed = urllib.parse.urlsplit(target)
            if method not in ('GET', 'HEAD') or parsed.scheme != 'http' or parsed.username or parsed.password:
                raise ValueError('Fixture requires HTTP absolute-form GET/HEAD')
            host, port = parsed.hostname, parsed.port or 80
        try:
            upstream = self.server.connect_target(host, port, method)
        except (OSError, ValueError):
            self.request.sendall(b'HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n')
            return None
        if method == 'CONNECT':
            self.request.sendall(b'HTTP/1.1 200 Connection Established\r\n\r\n')
        else:
            path = urllib.parse.urlunsplit(('', '', parsed.path or '/', parsed.query, ''))
            forwarded = [f'{method} {path} HTTP/1.1']
            forwarded.extend(f'{name}:{value}' for name, separator, value in fields
                             if separator and name.lower() not in ('proxy-authorization', 'proxy-connection',
                                                                   'connection'))
            forwarded.extend(['Connection: close', '', ''])
            data = '\r\n'.join(forwarded).encode('iso-8859-1')
            upstream.sendall(data)
            self.server.forwarded(len(data))
        return upstream


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serve', action='store_true', required=True)
    parser.add_argument('--kind', choices=['http', 'socks5'], required=True)
    parser.add_argument('--state', type=Path, required=True)
    parser.add_argument('--username', default='')
    parser.add_argument('--password', default='')
    parser.add_argument('--map', action='append', default=[])
    arguments = parser.parse_args()
    mappings = dict(value.split('=', 1) for value in arguments.map)
    server = RecordingProxy(arguments.kind, arguments.username, arguments.password, arguments.state, mappings)
    try:
        print(json.dumps(dict(host=server.host, port=server.port)), flush=True)
        for line in sys.stdin:
            if line.strip() == 'quit':
                break
    finally:
        server.close()


if __name__ == '__main__':
    main()
