"""Exercise native framing, durable browser handoff and the actual desktop service."""
import argparse
import hashlib
import http.server
import json
import os
import select
import signal
from pathlib import Path
import shutil
import socketserver
import sqlite3
import struct
import subprocess
import sys
import tempfile
import threading
import time
import uuid
import importlib.util

ORIGIN='chrome-extension://epdfgbhpbpkpephcfmemmadlblpodnfo/'
DATA=bytes(range(256))*8193

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version='HTTP/1.1'
    def log_message(self,*args):pass
    def do_GET(self):
        begin,end=0,len(DATA)-1
        value=self.headers.get('Range')
        if value:
            left,right=value.removeprefix('bytes=').split('-');begin=int(left);end=int(right or end)
        self.send_response(206 if value else 200);self.send_header('Content-Length',str(end-begin+1));self.send_header('ETag','"native-fixture-v1"')
        if value:self.send_header('Content-Range',f'bytes {begin}-{end}/{len(DATA)}')
        self.end_headers()
        try:self.wfile.write(DATA[begin:end+1])
        except (BrokenPipeError,ConnectionError):pass

class Server(socketserver.ThreadingMixIn,http.server.HTTPServer):
    daemon_threads=True
    def handle_error(self,request,address):pass

def frame(value):
    payload=json.dumps(value,ensure_ascii=False).encode();return struct.pack('<I',len(payload))+payload

def desktop_autostart(host,desktop,root,environment):
    directory=root/'auto-start';directory.mkdir()
    copied_host=directory/host.name;shutil.copy2(host,copied_host)
    copied_desktop=directory/('maskpilot.exe' if os.name=='nt' else 'maskpilot');shutil.copy2(desktop,copied_desktop)
    if (desktop.parent/'lib').is_dir():
        shutil.copytree(desktop.parent/'lib',directory/'lib')
    data=directory/'data'
    (directory/'native-bridge.json').write_text(json.dumps(dict(allowedOrigins=[ORIGIN],dataDirectory=str(data))))
    try:
        request=dict(version=1,requestId=uuid.uuid4().hex,command='ping')
        result=subprocess.run([str(copied_host),ORIGIN],input=frame(request),capture_output=True,
                              env=environment,timeout=15,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        assert result.returncode==0 and json.loads(result.stdout[4:])['ok'],result.stdout
        print('PASS native host starts the adjacent desktop and closes its reply pipe without desktopPath',flush=True)
    finally:
        lock=data/'instance.lock'
        if lock.exists():
            pid=int(lock.read_text().splitlines()[0])
            if os.name=='nt':
                import ctypes as ct
                from ctypes import wintypes as wt
                kernel=ct.WinDLL('kernel32',use_last_error=True)
                kernel.OpenProcess.argtypes=[wt.DWORD,wt.BOOL,wt.DWORD];kernel.OpenProcess.restype=wt.HANDLE
                kernel.QueryFullProcessImageNameW.argtypes=[wt.HANDLE,wt.DWORD,wt.LPWSTR,ct.POINTER(wt.DWORD)]
                kernel.WaitForSingleObject.argtypes=[wt.HANDLE,wt.DWORD]
                kernel.TerminateProcess.argtypes=[wt.HANDLE,wt.UINT]
                kernel.CloseHandle.argtypes=[wt.HANDLE]
                handle=kernel.OpenProcess(0x100000|0x1000|1,False,pid)
                if not handle:
                    assert ct.get_last_error()==87,'Cannot inspect owned auto-start desktop'
                else:
                    try:
                        if kernel.WaitForSingleObject(handle,0)!=0:
                            image=ct.create_unicode_buffer(32768);length=wt.DWORD(len(image))
                            assert kernel.QueryFullProcessImageNameW(handle,0,image,ct.byref(length))
                            assert os.path.normcase(str(Path(image.value).resolve()))==os.path.normcase(str(copied_desktop.resolve()))
                            assert kernel.TerminateProcess(handle,130)
                            assert kernel.WaitForSingleObject(handle,5000)==0
                    finally: kernel.CloseHandle(handle)
            elif sys.platform == 'darwin':
                # This detached desktop is not a Popen child. Restrict fixture
                # cleanup to its exact executable and unique private directory.
                def owned_desktop():
                    result=subprocess.run(['/bin/ps','-ww','-p',str(pid),'-o','stat=','-o','command='],
                                          capture_output=True,text=True,timeout=5)
                    fields=result.stdout.strip().split(None,1)
                    if result.returncode==1 and not fields:return False
                    assert result.returncode==0 and len(fields)==2,'Cannot inspect owned macOS desktop'
                    state,command=fields
                    return not state.startswith('Z') and str(copied_desktop) in command and str(data) in command
                for requested_signal in (signal.SIGTERM,signal.SIGKILL):
                    if not owned_desktop():break
                    try:os.kill(pid,requested_signal)
                    except ProcessLookupError:break
                    deadline=time.monotonic()+5
                    while owned_desktop() and time.monotonic()<deadline:time.sleep(.05)
                assert not owned_desktop(),'Owned macOS auto-start desktop cleanup failed'
            else:
                try: descriptor=os.pidfd_open(pid)
                except ProcessLookupError: descriptor=None
                if descriptor is not None:
                    try:
                        poller=select.poll();poller.register(descriptor,select.POLLIN)
                        if not poller.poll(0):
                            proc=Path('/proc')/str(pid)
                            assert (proc/'exe').resolve(strict=True)==copied_desktop.resolve()
                            assert str(data).encode() in (proc/'cmdline').read_bytes().split(b'\0')
                            signal.pidfd_send_signal(descriptor,signal.SIGTERM)
                            if not poller.poll(5000):
                                signal.pidfd_send_signal(descriptor,signal.SIGKILL)
                                assert poller.poll(5000),'Owned auto-start desktop cleanup failed'
                    finally: os.close(descriptor)

def run(host,desktop,parser=None,muxer=None):
    environment=os.environ.copy();environment.update({'NO_PROXY':'127.0.0.1,localhost,::1','no_proxy':'127.0.0.1,localhost,::1','QT_QPA_PLATFORM':'offscreen'})
    environment['PATH']=str(host.parent)+os.pathsep+environment.get('PATH','')
    flags=getattr(subprocess,'CREATE_NO_WINDOW',0)
    with tempfile.TemporaryDirectory(prefix='maskpilot-native-') as temporary:
        root=Path(temporary).resolve();data=root/'data';downloads=root/'downloads';downloads.mkdir();(data/'MaskPilot').mkdir(parents=True)
        settings='[General]\ndownloadDirectory='+downloads.as_posix()+'\nlanguage=en\n'
        if parser:settings+='mediaDownloader='+parser.as_posix()+'\n'
        if muxer:settings+='mediaMuxer='+muxer.as_posix()+'\n'
        (data/'MaskPilot/MaskPilot.ini').write_text(settings,encoding='utf-8')
        isolated=root/'host';isolated.mkdir();test_host=isolated/host.name;shutil.copy2(host,test_host)
        (isolated/'native-bridge.json').write_text(json.dumps({'allowedOrigins':[ORIGIN],'desktopPath':str(desktop),'dataDirectory':str(data)}))
        process=subprocess.Popen([str(desktop),'--background','--data-dir',str(data)],env=environment,stdout=subprocess.PIPE,stderr=subprocess.PIPE,creationflags=flags)
        server=Server(('127.0.0.1',0),Handler);threading.Thread(target=server.serve_forever,daemon=True).start()
        def query(command,**fields):
            request={'version':1,'requestId':uuid.uuid4().hex,'command':command,**fields}
            result=subprocess.run([str(test_host),ORIGIN],input=frame(request),capture_output=True,env=environment,timeout=25,creationflags=flags)
            assert len(result.stdout)>=4,(result.returncode,result.stderr.decode(errors='replace'))
            length=struct.unpack('<I',result.stdout[:4])[0];assert length==len(result.stdout)-4
            answer=json.loads(result.stdout[4:]);assert answer['requestId']==request['requestId'];return answer
        def record(task_id):
            database=sqlite3.connect(data/'tasks.db')
            try:return database.execute('SELECT state,downloaded,destination,error FROM tasks WHERE id=?',(task_id,)).fetchone()
            finally:database.close()
        def wait_complete(task_id,timeout=40):
            deadline=time.monotonic()+timeout
            while time.monotonic()<deadline:
                entry=record(task_id)
                if entry and entry[0]==9:return Path(entry[2])
                if entry and entry[0] in (10,13):raise AssertionError(entry[3])
                time.sleep(.1)
            raise AssertionError('Native task completion timeout')
        media_server=None
        try:
            deadline=time.monotonic()+15
            while not (data/'ipc.token').exists() and time.monotonic()<deadline:
                assert process.poll() is None,process.communicate();time.sleep(.05)
            ping=query('ping');assert ping['ok'],ping;print('PASS native host to desktop authenticated connection',flush=True)
            desktop_autostart(host,desktop,root,environment)
            if os.name != 'nt':
                config_path=isolated/'native-bridge.json'
                config=json.loads(config_path.read_text())
                manifest=root/'registered-manifest.json'
                gecko='downloads@maskpilot.invalid'
                manifest.write_text(json.dumps(dict(name='com.maskpilot.downloads',type='stdio',
                    path=str(test_host),allowed_extensions=[gecko])))
                config.update(firefoxManifestPath=str(manifest),allowedFirefoxExtensions=[gecko])
                config_path.write_text(json.dumps(config))
                def firefox(path,extension):
                    request=dict(version=1,requestId=uuid.uuid4().hex,command='ping')
                    return subprocess.run([str(test_host),str(path),extension],input=frame(request),
                        capture_output=True,env=environment,timeout=10)
                good=firefox(manifest,gecko)
                assert good.returncode==0 and json.loads(good.stdout[4:])['ok'],good.stderr
                assert firefox(root/'REGISTERED-manifest.json',gecko).returncode!=0
                assert firefox(manifest,'unregistered@example.invalid').returncode!=0
                saved=manifest.read_text();manifest.unlink()
                actual=root/'actual-manifest.json';actual.write_text(saved)
                manifest.symlink_to(actual)
                assert firefox(manifest,gecko).returncode!=0
                manifest.unlink();manifest.write_text(saved)
                print('PASS registered Firefox manifest binding, case, extension and symlink rejection',flush=True)
            bridge_spec=importlib.util.spec_from_file_location('maskpilot_stress_bridge',Path(__file__).resolve().parents[2]/'benchmarks/run.py')
            bridge_module=importlib.util.module_from_spec(bridge_spec);bridge_spec.loader.exec_module(bridge_module)
            bridge=bridge_module.Bridge(data)
            for delay in (.05,.1,.25):assert bridge.call('ping',read_delay=delay)['ok']
            print('PASS framed replies survive delayed client reads',flush=True)
            request=query('prepare',url=f'http://127.0.0.1:{server.server_port}/file',filename='native.bin',cookie='fixture=private')
            assert request['ok'],request;task_id=request['taskId'];entry=record(task_id);assert entry and entry[0]==5,'ACK arrived before durable paused record'
            assert not (downloads/'native.bin').exists();assert query('resume',taskId=task_id)['ok'];target=wait_complete(task_id);assert target.read_bytes()==DATA
            print('PASS prepare persisted before ACK, then resume and correct output',flush=True)
            listing=query('list');assert listing['ok'];assert 'cookie' not in json.dumps(listing).lower();assert any(task['id']==task_id for task in listing['tasks'])
            assert not query('add',url='file:///C:/Windows/win.ini')['ok']
            assert not query('add',url='https://example.test/file',filename='../escape.bin')['ok']
            bad=subprocess.run([str(test_host),'chrome-extension://unregistered/'],input=frame({'version':1}),capture_output=True,env=environment,timeout=5,creationflags=flags);assert bad.returncode!=0
            oversized=subprocess.run([str(test_host),ORIGIN],input=struct.pack('<I',1024*1024+1),capture_output=True,env=environment,timeout=5,creationflags=flags);assert oversized.returncode!=0
            print('PASS origin, framing, path and URL rejection',flush=True)
            assert query('remove',taskId=task_id)['ok']
            for _ in range(30):
                if record(task_id) is None:break
                time.sleep(.05)
            assert record(task_id) is None and target.read_bytes()==DATA
            print('PASS removing history preserves the final file',flush=True)
            if parser and muxer:
                fixtures=root/'media';fixtures.mkdir();source=fixtures/'source.mp4'
                commands=[
                    ['-f','lavfi','-i','testsrc=size=128x96:rate=10','-f','lavfi','-i','sine=frequency=440:sample_rate=44100','-t','2','-c:v','libx264','-pix_fmt','yuv420p','-c:a','aac',source.as_posix()],
                    ['-i',source.as_posix(),'-c','copy','-hls_time','1','-f','hls',(fixtures/'playlist.m3u8').as_posix()],
                    ['-i',source.as_posix(),'-map','0','-c','copy','-f','dash',(fixtures/'manifest.mpd').as_posix()]
                ]
                for arguments in commands:
                    result=subprocess.run([str(muxer),'-hide_banner','-loglevel','error','-y',*arguments],capture_output=True,timeout=30,creationflags=flags);assert result.returncode==0,result.stderr.decode(errors='replace')
                class MediaHandler(http.server.SimpleHTTPRequestHandler):
                    def __init__(self,*args,**kwargs):super().__init__(*args,directory=str(fixtures),**kwargs)
                    def log_message(self,*args):pass
                media_server=Server(('127.0.0.1',0),MediaHandler);threading.Thread(target=media_server.serve_forever,daemon=True).start()
                denied=query('add',url=f'http://127.0.0.1:{media_server.server_port}/source.mp4',filename='authenticated-media.mkv',media=True,cookie='fixture-only')
                assert not denied['ok'],'Media must not forward origin-bound credentials through global helper headers'
                print('PASS media credential rejection before task handoff',flush=True)
                for name in ('source.mp4','playlist.m3u8','manifest.mpd'):
                    response=query('add',url=f'http://127.0.0.1:{media_server.server_port}/{name}',filename=name+'.mkv',media=True);assert response['ok'],response
                    output=wait_complete(response['taskId'],90);assert output.stat().st_size>1000
                    probe=muxer.with_name('ffprobe.exe' if os.name == 'nt' else 'ffprobe');result=subprocess.run([str(probe),'-v','error','-show_entries','stream=codec_type','-of','json',str(output)],capture_output=True,text=True,timeout=15,creationflags=flags)
                    assert result.returncode==0,result.stderr
                    kinds=[stream['codec_type'] for stream in json.loads(result.stdout)['streams']];assert 'audio' in kinds and 'video' in kinds,kinds
                    print('PASS real media helper and audio/video output:',name,flush=True)
                response=query('add',url=f'http://127.0.0.1:{media_server.server_port}/source.mp4',filename='audio-only.m4a',media=True,audioOnly=True);assert response['ok'],response
                output=wait_complete(response['taskId'],90)
                result=subprocess.run([str(muxer.with_name('ffprobe.exe' if os.name == 'nt' else 'ffprobe')),'-v','error','-show_entries','stream=codec_type','-of','json',str(output)],capture_output=True,text=True,timeout=15,creationflags=flags)
                assert result.returncode==0 and [stream['codec_type'] for stream in json.loads(result.stdout)['streams']]==['audio']
                print('PASS real audio-only extraction',flush=True)
        finally:
            server.shutdown();server.server_close()
            if media_server:media_server.shutdown();media_server.server_close()
            process.terminate()
            try:process.communicate(timeout=10)
            except subprocess.TimeoutExpired:process.kill();process.communicate()

if __name__=='__main__':
    args=argparse.ArgumentParser();args.add_argument('host',type=Path);args.add_argument('desktop',type=Path);args.add_argument('--media-parser',type=Path);args.add_argument('--media-muxer',type=Path);options=args.parse_args()
    run(options.host.resolve(),options.desktop.resolve(),options.media_parser.resolve() if options.media_parser else None,options.media_muxer.resolve() if options.media_muxer else None)
