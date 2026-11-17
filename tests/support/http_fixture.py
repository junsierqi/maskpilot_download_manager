"""Deterministic local HTTP behavior and end-to-end output verification."""
import argparse
import os
import sys
import hashlib
import http.server
import pathlib
import re
import socketserver
import subprocess
import tempfile
import threading
import time
import urllib.parse

DATA = bytes(range(256)) * 12289
ETAG = '"fixture-v1"'
LARGE_SIZE = 2**32 + 257
PATTERN = bytes(range(256)) * 4097
REQUESTS = []
FLAKY = 0

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *args):
        pass
    def do_GET(self):
        global FLAKY
        route = urllib.parse.urlsplit(self.path).path
        if route == '/redirect':
            self.send_response(302); self.send_header('Location', '/file'); self.send_header('Content-Length', '0'); self.end_headers(); return
        if route == '/unauthorized':
            self.send_response(401); self.send_header('Content-Length', '0'); self.end_headers(); return
        if route == '/flaky' and FLAKY == 0:
            FLAKY += 1;self.send_response(503);self.send_header('Content-Length','0');self.send_header('Retry-After','1');self.end_headers();return
        if route == '/authenticated' and (self.headers.get('Cookie')!='session=fixture' or self.headers.get('Authorization')!='Bearer fixture'):
            self.send_response(401);self.send_header('Content-Length','0');self.end_headers();return
        if route == '/cross-origin':
            self.send_response(302);self.send_header('Location',f'http://localhost:{self.server.server_port}/no-credentials');self.send_header('Content-Length','0');self.end_headers();return
        if route == '/no-credentials' and (self.headers.get('Cookie') or self.headers.get('Authorization') or self.headers.get('Referer')):
            self.send_response(403);self.send_header('Content-Length','0');self.end_headers();return
        if route == '/empty':
            self.send_response(416); self.send_header('Content-Range', 'bytes */0'); self.send_header('Content-Length', '0'); self.end_headers(); return
        content = DATA if route != '/different' else bytes(reversed(DATA))
        content_length=LARGE_SIZE if route=='/large' else len(content)
        header = self.headers.get('Range', '')
        REQUESTS.append((route,header))
        match = re.fullmatch(r'bytes=(\d+)-(\d*)', header)
        ranged = match is not None and route not in ['/ignore-range','/unknown']
        if route == '/ignore-after-probe' and header != 'bytes=0-0': ranged = False
        begin, end = (int(match[1]), int(match[2] or content_length-1)) if ranged else (0,content_length-1)
        if begin >= content_length or end >= content_length:
            self.send_response(416); self.send_header('Content-Length', '0'); self.end_headers(); return
        self.send_response(206 if ranged else 200)
        if route != '/no-etag': self.send_header('ETag', ETAG)
        if ranged:
            malformed = route == '/bad-range' and end > 0
            self.send_header('Content-Range', f'bytes {begin + int(malformed)}-{end}/{content_length}')
        if route == '/unknown':
            self.send_header('Connection','close'); self.close_connection=True
        else:
            self.send_header('Content-Length',str(end-begin+1))
        self.end_headers()
        try:
            for offset in range(begin,end+1,16384):
                length=min(16384,end+1-offset)
                self.wfile.write(PATTERN[offset%256:offset%256+length] if route=='/large' else content[offset:offset+length])
                if route == '/slow':
                    self.wfile.flush(); time.sleep(.02)
        except (BrokenPipeError,ConnectionResetError,ConnectionAbortedError):
            pass

class Server(socketserver.ThreadingMixIn,http.server.HTTPServer):
    daemon_threads=True
    def handle_error(self, request, client_address):
        if not isinstance(sys.exc_info()[1],(ConnectionResetError,ConnectionAbortedError,BrokenPipeError)):
            super().handle_error(request,client_address)

def exercise(executable,large=False):
    os.environ['NO_PROXY']='127.0.0.1,localhost,::1';os.environ['no_proxy']=os.environ['NO_PROXY']
    server=Server(('127.0.0.1',0),Handler)
    thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
    base=f'http://127.0.0.1:{server.server_port}'
    try:
        with tempfile.TemporaryDirectory(prefix='maskpilot-product-') as temporary:
            root=pathlib.Path(temporary)
            if large:
                expected=hashlib.sha256();remaining=LARGE_SIZE
                while remaining:
                    length=min(1024*1024,remaining);expected.update(PATTERN[:length]);remaining-=length
                target=root/'large.bin';started=time.monotonic()
                result=subprocess.run([str(executable),f'{base}/large',str(target),str(root/'large'),'8',expected.hexdigest()],capture_output=True,text=True,timeout=300)
                assert result.returncode==0,(result.stdout,result.stderr)
                assert target.stat().st_size==LARGE_SIZE
                actual=hashlib.sha256()
                with target.open('rb') as output:
                    for block in iter(lambda:output.read(1024*1024),b''):actual.update(block)
                assert actual.hexdigest()==expected.hexdigest()
                print('PASS 64-bit file',LARGE_SIZE,'bytes',actual.hexdigest(),f'{time.monotonic()-started:.2f}s',flush=True)
                return
            for route in ['file','redirect','ignore-range','unknown','empty','no-etag','flaky']:
                target=root/f'{route}.bin'
                result=subprocess.run([str(executable),f'{base}/{route}',str(target),str(root/route),'4'],capture_output=True,text=True,timeout=30)
                assert result.returncode==0,(route,result.stdout,result.stderr)
                expected=b'' if route=='empty' else DATA
                assert target.read_bytes()==expected,route
                print('PASS',route,hashlib.sha256(target.read_bytes()).hexdigest())
            target=root/'\u4e0b\u8f7d \u6587\u4ef6'/'\u6d4b\u8bd5\u6587\u4ef6.bin'
            result=subprocess.run([str(executable),f'{base}/file',str(target),str(root/'\u4efb\u52a1\u6570\u636e'),'8'],capture_output=True,text=True,timeout=30)
            assert result.returncode==0,(result.stdout,result.stderr)
            assert target.read_bytes()==DATA
            print('PASS Unicode destination and database paths')
            blocked=root/'blocked-parent';blocked.write_bytes(b'keep this file')
            target=blocked/'cannot-create.bin'
            result=subprocess.run([str(executable),f'{base}/file',str(target),str(root/'blocked-state'),'4'],capture_output=True,text=True,timeout=30)
            assert result.returncode!=0 and blocked.read_bytes()==b'keep this file'
            assert not target.exists()
            print('PASS destination filesystem failure preserved existing data')
            for route in ['bad-range','unauthorized','ignore-after-probe']:
                target=root/f'{route}.bin'
                result=subprocess.run([str(executable),f'{base}/{route}',str(target),str(root/route),'4'],capture_output=True,text=True,timeout=30)
                assert result.returncode!=0,(route,result.stdout,result.stderr)
                assert not target.exists(),route
                print('PASS rejected',route)
            target=root/'checksum.bin'
            result=subprocess.run([str(executable),f'{base}/file',str(target),str(root/'checksum'),'4','0'*64],capture_output=True,text=True,timeout=30)
            assert result.returncode!=0 and not target.exists()
            print('PASS checksum mismatch not published')
            for route in ['authenticated','cross-origin']:
                target=root/f'{route}.bin'
                result=subprocess.run([str(executable),f'{base}/{route}',str(target),str(root/route),'4','','','session=fixture','Bearer fixture'],capture_output=True,text=True,timeout=30)
                assert result.returncode==0,(route,result.stdout,result.stderr)
                assert target.read_bytes()==DATA
                print('PASS',route)
            target=root/'proxy.bin'
            result=subprocess.run([str(executable),'http://fixture.invalid/file',str(target),str(root/'proxy'),'4','',base],capture_output=True,text=True,timeout=30)
            assert result.returncode==0,(result.stdout,result.stderr)
            assert target.read_bytes()==DATA
            print('PASS explicit HTTP proxy')
            target=root/'conflict.bin';target.write_bytes(b'keep existing')
            result=subprocess.run([str(executable),f'{base}/file',str(target),str(root/'conflict'),'4'],capture_output=True,text=True,timeout=30)
            assert result.returncode==0 and target.read_bytes()==b'keep existing'
            assert (root/'conflict (1).bin').read_bytes()==DATA
            print('PASS existing destination preserved')
            target=root/'recovered.bin';state=root/'recovery'
            process=subprocess.Popen([str(executable),f'{base}/slow',str(target),str(state),'1'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            line=process.stdout.readline().strip();assert line.startswith('TASK_ID '),line
            task_id=line.split()[1]
            checkpoint=state/'parts'/task_id/'offset-0'
            deadline=time.monotonic()+15
            while not checkpoint.exists() or checkpoint.stat().st_size<=1:
                assert process.poll() is None,'Slow fixture finished before a partial durable checkpoint'
                assert time.monotonic()<deadline,'No durable checkpoint before termination'
                time.sleep(.05)
            process.kill();process.communicate(timeout=10)
            assert not target.exists()
            checkpoint=state/'parts'/task_id/'offset-0'
            assert checkpoint.exists() and int(checkpoint.read_text())>0,'No durable checkpoint before termination'
            requests_before=len(REQUESTS)
            result=subprocess.run([str(executable),'--resume',task_id,str(state)],capture_output=True,text=True,timeout=30)
            assert result.returncode==0,(result.stdout,result.stderr)
            assert target.read_bytes()==DATA
            assert any(route=='/slow' and header.startswith('bytes=') and int(header[6:].split('-')[0])>0 for route,header in REQUESTS[requests_before:]),'Did not resume from durable bytes'
            print('PASS forced termination and persisted Range recovery')
            target=root/'changed.bin';state=root/'changed'
            process=subprocess.Popen([str(executable),f'{base}/slow',str(target),str(state),'1'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            task_id=process.stdout.readline().strip().split()[1]
            checkpoint=state/'parts'/task_id/'offset-0'
            deadline=time.monotonic()+15
            while not checkpoint.exists() or checkpoint.stat().st_size<=1:
                assert process.poll() is None,'Changed-resource fixture finished before checkpoint'
                assert time.monotonic()<deadline,'No durable checkpoint before resource change'
                time.sleep(.05)
            process.kill();process.communicate(timeout=10)
            result=subprocess.run([str(executable),'--change-url',task_id,str(state),f'{base}/different'],capture_output=True,text=True,timeout=30)
            assert result.returncode==0,(result.stdout,result.stderr)
            assert target.read_bytes()==bytes(reversed(DATA)),'Cross-resource resume mixed file versions'
            print('PASS different URL with same ETag/size restarts safely')
    finally:
        server.shutdown();server.server_close()

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--exercise',type=pathlib.Path);parser.add_argument('--port',type=int,default=18765);parser.add_argument('--large',action='store_true');args=parser.parse_args()
    if args.exercise: exercise(args.exercise.resolve(),args.large)
    else: Server(('127.0.0.1',args.port),Handler).serve_forever()
