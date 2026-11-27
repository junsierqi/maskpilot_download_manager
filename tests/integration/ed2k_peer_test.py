"""Bounded loopback protocol fixtures for the actual native transfer probe."""
import argparse
import contextlib
import hashlib
import json
from pathlib import Path
import select
import socket
import socketserver
import struct
import subprocess
import tempfile
import threading
import time
import uuid
import zlib

PART = 9728000
ABC = 'a448017aaf21d8525fc10ae87aa6729d'
ZERO_PART = 'd7def262a127cd79096a108e7a9fc138'
ZERO_BYTE = '47c61a0fa8738ba77308a8a600f88e4b'
ZERO_FILE = '06329e9dba1373512c06386fe29e3c65'


def exact(connection, size):
    data = bytearray()
    while len(data) < size:
        block = connection.recv(size-len(data))
        if not block:
            raise EOFError('fixture peer closed')
        data.extend(block)
    return bytes(data)


def frame(protocol, opcode, payload=b''):
    return bytes([protocol])+struct.pack('<I', len(payload)+1)+bytes([opcode])+payload


def packet(connection):
    header = exact(connection, 5)
    length = struct.unpack('<I', header[1:])[0]
    assert header[0] in (0xe3, 0xc5, 0xd4) and 0 < length <= 2*1024*1024
    body = exact(connection, length)
    return header[0], body[0], body[1:]


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    block_on_close = False

    def __init__(self, handler, **fields):
        self.__dict__.update(fields)
        self.connections = 0
        self.failures = []
        super().__init__(('127.0.0.1', 0), handler)

    def handle_error(self, request, address):
        import sys
        error = sys.exception()
        if not isinstance(error, (EOFError, ConnectionError, OSError)):
            self.failures.append(repr(error))


class Peer(socketserver.BaseRequestHandler):
    def handle(self):
        server, connection = self.server, self.request
        connection.settimeout(20)
        server.connections += 1
        file_hash = bytes.fromhex(server.file_hash)
        send_lock = threading.Lock()

        def send(opcode, payload=b'', protocol=0xe3):
            message = frame(protocol, opcode, payload)
            with send_lock:
                if opcode == 0x4c and server.fragment:
                    connection.sendall(message[:2])
                    time.sleep(.01)
                    connection.sendall(message[2:])
                else:
                    connection.sendall(message)

        while True:
            protocol, opcode, payload = packet(connection)
            if opcode == 0x01 and protocol == 0xe3:
                if server.malformed:
                    connection.sendall(b'\xe3\xff\xff\xff\xff')
                    return
                assert payload[0] == 16 and len(payload) >= 33
                with server.identity_lock:
                    port = struct.unpack_from('<H', payload, 21)[0]
                    identity = payload[1:17]
                    previous = server.identities.get(port)
                    if server.strict_identity and previous is not None and previous != identity:
                        server.rejected_identities += 1
                        return
                    server.identities[port] = identity
                # Minimal classic hello answer with explicit large-file capability.
                tags = b'\x03\x01\x00\xfe'+struct.pack('<I', 16)
                send(0x4c, bytes.fromhex(ABC)+struct.pack('<IHI', 0, 0, 1)+tags+b'\0'*6)
            elif opcode == 0x58:
                assert payload == file_hash
                name = b'../untrusted-peer-name.bin'
                send(0x59, file_hash+struct.pack('<H', len(name))+name)
            elif opcode == 0x4f:
                assert payload == file_hash
                send(0x50, file_hash+b'\0\0')
            elif opcode == 0x51:
                assert payload == file_hash
                hashes = server.hashes
                proof = file_hash+struct.pack('<H', len(hashes))+b''.join(bytes.fromhex(x) for x in hashes)
                if server.packed:
                    send(0x52, zlib.compress(proof), 0xd4)
                else:
                    send(0x52, proof)
            elif opcode == 0x54:
                assert payload == file_hash
                if server.queue:
                    send(0x60, struct.pack('<H', 7)+b'\0'*10, 0xc5)
                    timer = threading.Timer(.15, lambda: send(0x55))
                    timer.daemon = True
                    timer.start()
                else:
                    send(0x55)
            elif opcode in (0x47, 0xa1):
                assert payload[:16] == file_hash
                wide = opcode == 0xa1
                fields = struct.unpack('<6Q' if wide else '<6I', payload[16:])
                server.wide_requests += int(wide)
                for begin, end in reversed(list(zip(fields[:3], fields[3:]))):
                    if begin == end == 0:
                        continue
                    assert 0 <= begin < end <= server.size and end-begin <= 180*1024
                    data = b'abc'[begin:end] if server.size == 3 else b'\0'*(end-begin)
                    if server.corrupt:
                        data = bytes([data[0] ^ 1])+data[1:]
                    if server.compress:
                        encoded = zlib.compress(data)
                        prefix = file_hash+struct.pack('<QI' if wide else '<II', begin, len(encoded))
                        half = len(encoded)//2
                        for block in (encoded[:half], encoded[half:]):
                            send(0xa3 if wide else 0x40, prefix+block, 0xc5)
                    else:
                        for offset in range(0, len(data), 13777):
                            block = data[offset:offset+13777]
                            prefix = file_hash+struct.pack('<QQ' if wide else '<II', begin+offset, begin+offset+len(block))
                            send(0xa2 if wide else 0x46, prefix+block, 0xc5 if wide else 0xe3)
            else:
                raise AssertionError(f'unexpected fixture request {protocol:x}/{opcode:x}')


class Index(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(20)
        self.server.connections += 1
        protocol, opcode, payload = packet(self.request)
        assert protocol == 0xe3 and opcode == 1 and len(payload) >= 26
        self.request.sendall(frame(0xe3, 0x40, struct.pack('<I', 0x0100007f)))
        while True:
            protocol, opcode, payload = packet(self.request)
            assert opcode == 0x19 and payload[:16] == bytes.fromhex(self.server.file_hash)
            port = self.server.peer_port
            self.request.sendall(frame(0xe3, 0x42, payload[:16]+b'\x01'+socket.inet_aton('127.0.0.1')+struct.pack('<H', port)))


class Proxy(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(10)
        version, count = exact(self.request, 2)
        assert version == 5
        exact(self.request, count)
        self.request.sendall(b'\x05\xff' if self.server.reject else b'\x05\x00')
        if self.server.reject:
            return
        version, command, reserved, kind = exact(self.request, 4)
        assert (version, command, reserved, kind) == (5, 1, 0, 3), 'source DNS must resolve at the proxy'
        host = exact(self.request, exact(self.request, 1)[0]).decode('ascii')
        port = struct.unpack('!H', exact(self.request, 2))[0]
        assert host == 'peer.invalid' and port == self.server.peer_port
        self.server.hosts.append(host)
        with socket.create_connection(('127.0.0.1', port), timeout=10) as upstream:
            self.request.sendall(b'\x05\x00\x00\x01\x7f\x00\x00\x01\x00\x00')
            while True:
                ready, _, _ = select.select([self.request, upstream], [], [], 10)
                if not ready:
                    return
                for source in ready:
                    data = source.recv(65536)
                    if not data:
                        return
                    (upstream if source is self.request else self.request).sendall(data)


@contextlib.contextmanager
def running(handler=Peer, **settings):
    values = dict(file_hash=ABC, size=3, hashes=[], fragment=True, malformed=False,
                  corrupt=False, compress=False, packed=False, queue=False, wide_requests=0,
                  strict_identity=False, identities={}, identity_lock=threading.Lock(), rejected_identities=0)
    values.update(settings)
    with Server(handler, **values) as server:
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            yield server
        finally:
            server.shutdown()
            thread.join(timeout=5)
            assert not server.failures, server.failures


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='maskpilot-ed2k-') as temporary:
        root = Path(temporary).resolve()

        def run(name, server=None, extra=(), size=3, file_hash=ABC, task_id=None):
            folder = root/name
            folder.mkdir(exist_ok=True)
            target = folder/'output.bin'
            command = [str(args.probe.resolve()), '--url', f'ed2k://|file|input.bin|{size}|{file_hash}|/',
                       '--destination', str(target), '--work', str(folder/'work'), '--timeout-ms', '20000',
                       '--task-id', task_id or uuid.uuid4().hex]
            if server:
                command += ['--source', f'127.0.0.1:{server.server_address[1]}']
            result = subprocess.run(command+list(extra), capture_output=True, text=True, timeout=25)
            assert result.stdout.strip(), (result.returncode, result.stderr)
            answer = json.loads(result.stdout.strip().splitlines()[-1])
            return result.returncode, answer, target

        with running(queue=True) as server:
            code, answer, target = run('queued', server)
            assert code == 0 and target.read_bytes() == b'abc' and not answer['cleanupError'], answer
            assert not (target.parent/'work/ed2k').exists()
        print('PASS native handshake, remote queue, fragmented framing and untrusted peer filename', flush=True)

        with running(corrupt=True) as bad, running() as good:
            code, answer, target = run('corrupt', bad, ['--connections', '1', '--source', f'127.0.0.1:{good.server_address[1]}'])
            assert code == 0 and target.read_bytes() == b'abc' and answer['payload'] >= 6, answer
            assert bad.connections and good.connections
        with running(malformed=True) as bad, running() as good:
            code, answer, target = run('malformed', bad, ['--connections', '1', '--source', f'127.0.0.1:{good.server_address[1]}'])
            assert code == 0 and target.read_bytes() == b'abc', answer
        print('PASS corrupt source and oversized frame rejected; alternate source succeeds', flush=True)

        with running(Index, peer_port=0) as empty:
            code, answer, target = run('cancel', extra=['--server', f'127.0.0.1:{empty.server_address[1]}', '--timeout-ms', '500'])
            assert code == 4 and not target.exists(), answer
        with running() as peer, running(Index, peer_port=peer.server_address[1]) as index:
            code, answer, target = run('discovered', extra=['--server', f'127.0.0.1:{index.server_address[1]}'])
            assert code == 0 and target.read_bytes() == b'abc' and index.connections, answer
        print('PASS configured index discovers a direct source; source-wait cancellation works', flush=True)

        with running() as server:
            code, answer, target = run('quota', server, ['--quota', '2'])
            assert code == 3 and answer['payload'] == 2 and not target.exists(), answer
            code, answer, target = run('scan', server, ['--reject-scan'])
            assert code == 2 and not target.exists() and 'scan' in answer['error'].lower(), answer
            code, answer, target = run('rate-cancel', server, ['--rate', '1', '--timeout-ms', '500'])
            assert code == 4 and not target.exists(), answer
            protected = root/'existing'
            protected.mkdir()
            (protected/'output.bin').write_bytes(b'user file')
            code, answer, target = run('existing', server)
            assert code == 2 and target.read_bytes() == b'user file', answer
        print('PASS quota, scan rejection, rate-limited cancellation and no-clobber publication', flush=True)
        with running() as server:
            code, answer, target = run('required-encryption', extra=[
                '--source', f'127.0.0.1:{server.server_address[1]}:4'])
            assert code == 2 and not server.connections and not target.exists(), answer
            assert 'encryption' in answer['error'].lower()
        print('PASS required encryption is rejected before plaintext contact', flush=True)
        shared = root/'shared-service-work'
        shared.mkdir()
        with running(strict_identity=True) as server:
            for i in range(2):
                code, answer, target = run(f'stable-client-{i}', server,
                    ['--work', str(shared/uuid.uuid4().hex)])
                assert code == 0 and target.read_bytes() == b'abc', answer
            assert len(server.identities) == 1 and not server.rejected_identities
        print('PASS separate task processes reuse one owner/route identity after cache removal', flush=True)

        with running(file_hash=ZERO_FILE, size=PART+1, hashes=[ZERO_PART, ZERO_BYTE], compress=True, packed=True) as server:
            task_id = uuid.uuid4().hex
            code, answer, target = run('resume', server, ['--pause-at', str(PART), '--connections', '1'],
                                       size=PART+1, file_hash=ZERO_FILE, task_id=task_id)
            assert code == 3 and not target.exists(), answer
            code, answer, target = run('resume', server, size=PART+1, file_hash=ZERO_FILE, task_id=task_id)
            assert code == 0 and target.stat().st_size == PART+1 and not answer['cleanupError'], answer
            assert hashlib.sha256(target.read_bytes()).digest() == hashlib.sha256(b'\0'*(PART+1)).digest()
        print('PASS compressed blocks, packed hashset, independent boundary identity and cross-process verified resume', flush=True)
        # Independent OS MD4 vector: 441 full zero parts followed by one zero byte.
        large_hash = '11b2937f9061e5c4570668784cef0eb0'
        large_size = 441*PART+1
        with running(file_hash=large_hash, size=large_size, hashes=[ZERO_PART]*441+[ZERO_BYTE]) as server:
            code, answer, target = run('wide-requests', server, ['--quota', '2'], size=large_size, file_hash=large_hash)
            assert code == 3 and answer['payload'] == 2 and server.wide_requests and not target.exists(), answer
        print('PASS actual native 64-bit block requests above legacy boundary; bounded quota stops payload', flush=True)

        with running() as peer, running(Proxy, peer_port=peer.server_address[1], reject=False, hosts=[]) as proxy:
            code, answer, target = run('proxy', extra=['--source', f'peer.invalid:{peer.server_address[1]}',
                '--proxy', f'socks5://127.0.0.1:{proxy.server_address[1]}'])
            assert code == 0 and target.read_bytes() == b'abc' and proxy.hosts == ['peer.invalid'], answer
        with running() as peer, running(Proxy, peer_port=peer.server_address[1], reject=True, hosts=[]) as proxy:
            code, answer, target = run('proxy-failed', peer, ['--proxy', f'socks5://127.0.0.1:{proxy.server_address[1]}',
                                                           '--timeout-ms', '500'])
            assert code == 4 and not peer.connections and not target.exists(), answer
        print('PASS proxy-side DNS, SOCKS routing and failed-route no-direct-fallback', flush=True)


if __name__ == '__main__':
    main()
