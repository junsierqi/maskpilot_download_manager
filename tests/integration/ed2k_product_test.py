"""Public CLI and unified-service ED2K acceptance on owned loopback sources."""
import argparse
from contextlib import closing
import json
from pathlib import Path
import sqlite3
import subprocess
import tempfile
import threading
import time
from ed2k_peer_test import ABC, ZERO_FILE, ZERO_PART, ZERO_BYTE, PART, Peer, Index, running

class ExclusivePeer(Peer):
    def handle(self):
        with self.server.active_lock:
            self.server.active += 1
            self.server.maximum_active = max(self.server.maximum_active, self.server.active)
        try:
            super().handle()
        finally:
            with self.server.active_lock:
                self.server.active -= 1


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exercise', required=True, type=Path)
    parser.add_argument('--probe', required=True, type=Path)
    args = parser.parse_args()
    cli, probe = str(args.exercise.resolve()), str(args.probe.resolve())
    with tempfile.TemporaryDirectory(prefix='maskpilot-ed2k-product-') as temporary:
        root = Path(temporary).resolve()
        link = f'ed2k://|file|input.bin|3|{ABC}|/'
        def invoke(command, expected=0):
            result = subprocess.run(command, text=True, capture_output=True, timeout=35)
            assert result.returncode == expected, (result.returncode, result.stdout, result.stderr)
            return result
        with running(strict_identity=True) as peer:
            endpoint = f'127.0.0.1:{peer.server_address[1]}'
            data, target = root/'cli', root/'output.bin'
            invoke([cli, '--bt-no-discovery', link, str(target), str(data), '--ed2k-source', endpoint])
            assert target.read_bytes() == b'abc'
            with closing(sqlite3.connect(data/'tasks.db')) as db:
                assert db.execute('PRAGMA user_version').fetchone()[0] == 20
                row = db.execute('SELECT kind,state,ed2k_sources FROM tasks').fetchone()
                assert row == (6, 9, endpoint), row
            assert not list((data/'parts').glob('*/ed2k/identity.json'))
            assert not list(root.glob('*.maskpilot-*.tmp'))
        print('PASS public CLI, schema20, native routing, final bytes and owned cleanup', flush=True)
        with running() as peer, running(Index, peer_port=peer.server_address[1]) as index:
            endpoint = f'127.0.0.1:{index.server_address[1]}'
            invoke([cli, '--bt-no-discovery', link, str(root/'index.bin'), str(root/'index'),
                    '--ed2k-server', f'ed2k://|server|127.0.0.1|{index.server_address[1]}|/'])
            assert (root/'index.bin').read_bytes() == b'abc' and index.connections
        print('PASS public CLI server metadata option and actual source discovery', flush=True)
        with running(ExclusivePeer, size=PART+1, file_hash=ZERO_FILE, hashes=[ZERO_PART, ZERO_BYTE],
                     strict_identity=True, active=0, maximum_active=0, active_lock=threading.Lock()) as peer:
            endpoint = f'127.0.0.1:{peer.server_address[1]}'
            target, work = root/'parallel.bin', root/'parallel'
            result = invoke([probe, '--service', '--url', f'ed2k://|file|zero.bin|{PART+1}|{ZERO_FILE}|/',
                             '--destination', str(target), '--work', str(work), '--source', endpoint])
            answer = json.loads(result.stdout.strip().splitlines()[-1])
            assert answer['complete'] and answer['count'] == 2, answer
            assert target.read_bytes() == b'\0'*(PART+1)
            assert Path(str(target)+'.second').read_bytes() == target.read_bytes()
            assert peer.maximum_active == 1 and peer.rejected_identities == 0, vars(peer)
            with closing(sqlite3.connect(work/'tasks.db')) as db:
                assert db.execute('SELECT COUNT(*) FROM tasks WHERE kind=6 AND state=9').fetchone()[0] == 2
        print('PASS two concurrent actual service tasks, shared identity/endpoint arbitration and exact bytes', flush=True)
        with running() as peer:
            target = root/'budget.bin'
            result = invoke([probe, '--service', '--url', link, '--destination', str(target),
                             '--work', str(root/'budget'), '--source', f'127.0.0.1:{peer.server_address[1]}',
                             '--quota', '2'], expected=3)
            answer = json.loads(result.stdout.strip().splitlines()[-1])
            assert answer['waiting'] and answer['payload'] == 2 and not target.exists(), answer
        print('PASS configured service quota charges actual payload and returns policy waiting', flush=True)
        with running() as peer:
            before = peer.connections
            invoke([cli, '--bt-no-discovery', link, str(root/'bad.bin'), str(root/'bad'),
                    '--ed2k-source', 'user:password@example.test:4'], expected=1)
            assert peer.connections == before and not (root/'bad.bin').exists()
        print('PASS malformed endpoints rejected before contact/publication', flush=True)

if __name__ == '__main__':
    main()
