"""Real isolated Windows Firefox -> native host -> desktop acceptance.

Requires serialized permission: temporarily replaces only the HKCU64 Mozilla host
default, restores it in finally, and never uses a real browser profile.
"""
import argparse
import base64
import contextlib
import hashlib
import http.server
import json
import os
from pathlib import Path
import secrets
import shutil
import socket
import sqlite3
import struct
import subprocess
import sys
import tempfile
import threading
import time
import uuid
from urllib.parse import parse_qs, urlsplit
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'support'))
from marionette import Marionette

ADDON_ID = 'downloads@maskpilot.invalid'
HOST_NAME = 'com.maskpilot.downloads'
REGISTRY_PATH = r'Software\Mozilla\NativeMessagingHosts\com.maskpilot.downloads'
PAYLOAD = bytes(range(251)) * 20000
HASH = hashlib.sha256(PAYLOAD).hexdigest()
FLAGS = getattr(subprocess, 'CREATE_NO_WINDOW', 0)


def until(operation, description, timeout=35):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            value = operation()
            if value:
                return value
        except (OSError, sqlite3.Error, RuntimeError) as error:
            last = error
        time.sleep(.1)
    raise AssertionError(f'Timed out: {description}; last={last}')


@contextlib.contextmanager
def registered(manifest):
    import winreg
    access = winreg.KEY_READ | winreg.KEY_WRITE | winreg.KEY_WOW64_64KEY
    existed, previous = True, None
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, REGISTRY_PATH, 0, access) as key:
            try:
                previous = winreg.QueryValueEx(key, '')
            except FileNotFoundError:
                pass
    except FileNotFoundError:
        existed = False
    with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, REGISTRY_PATH, 0, access) as key:
        winreg.SetValueEx(key, '', 0, winreg.REG_SZ, str(manifest))
    try:
        yield
    finally:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, REGISTRY_PATH, 0, access) as key:
            assert winreg.QueryValueEx(key, '') == (str(manifest), winreg.REG_SZ), 'Mozilla default changed concurrently'
            if previous is None:
                winreg.DeleteValue(key, '')
            else:
                winreg.SetValueEx(key, '', 0, previous[1], previous[0])
            children, values, _ = winreg.QueryInfoKey(key)
        if not existed and not children and not values:
            winreg.DeleteKeyEx(winreg.HKEY_CURRENT_USER, REGISTRY_PATH, winreg.KEY_WOW64_64KEY, 0)


class Origin(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self):
        self.requests = []
        self.cookies = {'standard': secrets.token_hex(20), 'container': secrets.token_hex(20)}
        self.secrets = list(self.cookies.values())
        super().__init__(('127.0.0.1', 0), Handler)
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)
        self.thread.start()

    def url(self, name):
        return f'http://127.0.0.1:{self.server_port}/{name}'

    def close(self):
        self.shutdown()
        self.server_close()
        self.thread.join(5)


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *arguments):
        pass

    def do_GET(self):
        parsed = urlsplit(self.path)
        name = parsed.path.lstrip('/')
        if name == 'panel':
            body = (b'<!doctype html><meta charset="utf-8"><title>Owned resource panel</title>'
                    b'<h1>Two independent resources</h1><a href="/panel-first" download="first.bin">First</a>'
                    b'<a href="/panel-second" download="second.bin">Second</a>')
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if name == 'page':
            target = parse_qs(parsed.query).get('target', [''])[0]
            if target not in ('captured', 'standard', 'container', 'fallback', 'private', 'partitioned'):
                self.send_error(400)
                return
            body = ('<!doctype html><meta charset="utf-8"><title>Owned Firefox download</title>'
                    '<h1>Browser handoff fixture</h1><a id="download" href="/' + target + '" download="' +
                    target + '.bin">Download owned fixture</a>').encode()
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        cookie = self.headers.get('Cookie', '')
        authenticated = (name not in self.server.cookies or
                         cookie == 'owned_session=' + self.server.cookies[name])
        self.server.requests.append({'path': name, 'range': bool(self.headers.get('Range')),
                                     'authenticated': authenticated,
                                     'cookieNames': [part.split('=', 1)[0].strip() for part in cookie.split(';') if part]})
        if not authenticated:
            self.send_response(401)
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        begin, end = 0, len(PAYLOAD) - 1
        value = self.headers.get('Range')
        if value:
            left, right = value.removeprefix('bytes=').split('-')
            begin, end = int(left), int(right or end)
        self.send_response(206 if value else 200)
        self.send_header('Content-Type', 'application/octet-stream')
        self.send_header('Content-Length', str(end - begin + 1))
        self.send_header('Accept-Ranges', 'bytes')
        self.send_header('ETag', '"firefox-owned-v1"')
        self.send_header('Content-Disposition', f'attachment; filename="{name}.bin"')
        if value:
            self.send_header('Content-Range', f'bytes {begin}-{end}/{len(PAYLOAD)}')
        self.end_headers()
        try:
            for offset in range(begin, end + 1, 16384):
                self.wfile.write(PAYLOAD[offset:min(offset + 16384, end + 1)])
                self.wfile.flush()
                time.sleep(.003)
        except (BrokenPipeError, ConnectionError):
            pass


def rows(data):
    with contextlib.closing(sqlite3.connect('file:' + (data / 'tasks.db').as_posix() + '?mode=ro', uri=True)) as db:
        db.row_factory = sqlite3.Row
        return [dict(row) for row in db.execute('SELECT id,state,url,destination,error FROM tasks')]


def stop(process):
    if process and process.poll() is None:
        subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'], capture_output=True,
                       creationflags=FLAGS, timeout=15)
        process.wait(timeout=15)


def exercise(build, extension, firefox, evidence_directory=None):
    assert os.name == 'nt', 'This qualification exercises Windows native manifest lookup'
    desktop, source_host = build / 'maskpilot.exe', build / 'maskpilot_native_host.exe'
    for path in (desktop, source_host, extension / 'manifest.json', firefox):
        assert path.is_file(), path
    environment = os.environ.copy()
    environment.update(QT_QPA_PLATFORM='offscreen', NO_PROXY='127.0.0.1,localhost',
                       no_proxy='127.0.0.1,localhost')
    environment['PATH'] = str(build) + os.pathsep + environment.get('PATH', '')
    evidence = {'result': 'failed', 'addonId': ADDON_ID, 'assertions': [],
                'qualification': 'Temporary owned addon; signed distribution installation is not tested.'}
    if evidence_directory:
        evidence_directory = evidence_directory / ('run-' + uuid.uuid4().hex)
        evidence_directory.mkdir(parents=True)
    with tempfile.TemporaryDirectory(prefix='maskpilot-firefox-') as temporary:
        root = Path(temporary).resolve()
        data, downloads, browser_downloads, profile, host_directory = [root / name for name in
            ('data', 'downloads', 'browser-downloads', 'profile', 'host')]
        for path in (data / 'MaskPilot', downloads, browser_downloads, profile, host_directory):
            path.mkdir(parents=True)
        (data / 'MaskPilot/MaskPilot.ini').write_text('[General]\nlanguage=en\ncompletionAction=0\n'
            'downloadDirectory=' + downloads.as_posix() + '\n', encoding='utf-8')
        host = host_directory / source_host.name
        shutil.copyfile(source_host, host)
        (host_directory / 'native-bridge.json').write_text(json.dumps({
            'allowedOrigins': [], 'allowedFirefoxExtensions': [ADDON_ID],
            'desktopPath': str(desktop), 'dataDirectory': str(data)}), encoding='utf-8')
        manifest = host_directory / (HOST_NAME + '.firefox.json')
        manifest.write_text(json.dumps({'name': HOST_NAME, 'description': 'Owned Firefox test',
            'path': str(host), 'type': 'stdio', 'allowed_extensions': [ADDON_ID]}), encoding='utf-8')
        # These rejected invocations must not start the configured desktop.
        for arguments in ([str(manifest), 'wrong@maskpilot.invalid'], [str(root / manifest.name), ADDON_ID],
                          [r'\\owned-invalid.invalid\share\manifest.json', ADDON_ID],
                          [str(manifest), ADDON_ID, 'unexpected']):
            result = subprocess.run([str(host), *arguments], input=b'', capture_output=True,
                                    env=environment, cwd=root, creationflags=FLAGS, timeout=10)
            assert result.returncode != 0, (arguments, result.returncode, result.stdout)
            assert 4 <= len(result.stdout) <= 65540, 'Rejected native argv response exceeds framing bound'
            length = struct.unpack('<I', result.stdout[:4])[0]
            assert length == len(result.stdout) - 4, 'Rejected native argv response has invalid framing'
            answer = json.loads(result.stdout[4:])
            assert answer.get('ok') is False and isinstance(answer.get('error'), str), answer
            assert not any(path.name in ('ipc.token', 'tasks.db', 'tasks.db-wal', 'tasks.db-shm')
                           for path in data.rglob('*')), 'Rejected native argv launched desktop'
        print('PASS Firefox native wrong manifest/identity/UNC/extra args reject before desktop launch')
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            port = listener.getsockname()[1]
        preferences = {'marionette.port': port, 'marionette.enabled': True,
            'browser.shell.checkDefaultBrowser': False, 'browser.startup.homepage': 'about:blank',
            'browser.startup.page': 0, 'browser.download.folderList': 2,
            'browser.download.dir': str(browser_downloads), 'browser.download.useDownloadDir': True,
            'browser.download.always_ask_before_handling_new_types': False,
            'browser.download.alwaysOpenPanel': False, 'browser.safebrowsing.downloads.enabled': False,
            'datareporting.policy.dataSubmissionEnabled': False, 'toolkit.telemetry.enabled': False,
            'privacy.userContext.enabled': True, 'extensions.update.enabled': False,
            'app.update.auto': False, 'browser.newtabpage.enabled': False}
        (profile / 'user.js').write_text(''.join(f'user_pref({json.dumps(key)}, {json.dumps(value)});\n'
                                                for key, value in preferences.items()), encoding='utf-8')
        addon = root / 'owned-addon.xpi'
        with zipfile.ZipFile(addon, 'w', zipfile.ZIP_DEFLATED) as archive:
            for path in extension.rglob('*'):
                if path.is_file():
                    archive.write(path, path.relative_to(extension).as_posix())
        desktop_process = firefox_process = driver = None
        origin = Origin()
        try:
            with registered(manifest), open(root / 'desktop.log', 'wb') as desktop_log, open(root / 'firefox.log', 'wb') as firefox_log:
                try:
                    desktop_process = subprocess.Popen([str(desktop), '--background', '--data-dir', str(data)],
                        env=environment, cwd=root, stdout=desktop_log, stderr=desktop_log, creationflags=FLAGS)
                    until(lambda: (data / 'ipc.token').is_file() and (data / 'tasks.db').is_file(), 'desktop startup')
                    firefox_process = subprocess.Popen([str(firefox), '-no-remote', '-profile', str(profile),
                        '-marionette', '-headless', '--remote-allow-system-access', 'about:blank'], env=environment,
                        cwd=root, stdout=firefox_log, stderr=firefox_log, creationflags=FLAGS)
                    driver = Marionette(port)
                    # Permit the owned addon in this owned private window so production rejection is actually exercised.
                    assert driver.install(addon, allow_private=True) == ADDON_ID
                    driver.context('chrome')
                    popup = driver.script('return WebExtensionPolicy.getByID(arguments[0]).getURL("popup.html");', [ADDON_ID])
                    driver.context('content')
                    driver.navigate(popup)
                    principal_handle = driver.command('WebDriver:GetWindowHandle')
                    until(lambda: driver.evaluate('return !!browser?.runtime?.sendNativeMessage;'), 'extension principal')

                    def native(command, **fields):
                        request = {'version': 1, 'requestId': uuid.uuid4().hex, 'command': command, **fields}
                        answer = driver.evaluate('return await browser.runtime.sendNativeMessage(arguments[0],arguments[1]);',
                                                 [HOST_NAME, request])
                        assert answer['version'] == 1 and answer['requestId'] == request['requestId'], answer
                        return answer

                    def completed(task_id):
                        row = until(lambda: next((row for row in rows(data) if row['id'] == task_id and row['state'] == 9), None),
                                    'native transfer completion')
                        assert hashlib.sha256(Path(row['destination']).read_bytes()).hexdigest() == HASH
                        return row

                    assert native('ping')['ok'] is True
                    prepared = native('prepare', url=origin.url('manual'), filename='manual.bin')
                    assert prepared['ok'], prepared
                    task = next(row for row in rows(data) if row['id'] == prepared['taskId'])
                    assert task['state'] == 5 and not Path(task['destination']).exists()
                    assert native('resume', taskId=task['id'])['ok']
                    completed(task['id'])
                    until(lambda: driver.script('return document.querySelector("#connection")?.textContent.includes("已连接");'),
                          'actual connected extension popup')
                    if evidence_directory:
                        screenshot = driver.command('WebDriver:TakeScreenshot', {'full': False, 'scroll': False})
                        (evidence_directory / 'extension-popup.png').write_bytes(base64.b64decode(screenshot, validate=True))
                    evidence['assertions'].append('Actual addon native API durable Paused ACK and exact resume payload')
                    print('PASS genuine Firefox extension principal/native API durable Paused ACK and resume hash')

                    def open_http(url, store=None, private=False):
                        previous = set(driver.command('WebDriver:GetWindowHandles'))
                        if private:
                            driver.evaluate('return await browser.windows.create(arguments[0]);',
                                            [{'url': url, 'incognito': True}])
                        else:
                            details = {'url': url, 'active': True}
                            if store:
                                details['cookieStoreId'] = store
                            driver.evaluate('return await browser.tabs.create(arguments[0]);', [details])
                        handles = until(lambda: set(driver.command('WebDriver:GetWindowHandles')) - previous,
                                        'new owned HTTP tab')
                        assert len(handles) == 1, handles
                        handle = handles.pop()
                        driver.command('WebDriver:SwitchToWindow', {'handle': handle})
                        until(lambda: driver.command('WebDriver:GetCurrentURL') == url, 'owned HTTP page')
                        return handle

                    def trusted_click(element):
                        driver.command('WebDriver:ElementClick', {'id': element['element-6066-11e4-a52e-4f735466cecf']})

                    def browser_click(name, store=None, private=False):
                        open_http(origin.url('page?target=' + name), store, private)
                        try:
                            element = driver.command('WebDriver:FindElement', {'using': 'css selector', 'value': '#download'})
                            # WebDriver input produces an actual trusted user click in the HTTP document.
                            trusted_click(element)
                        finally:
                            driver.command('WebDriver:SwitchToWindow', {'handle': principal_handle})
                        entry = until(lambda: next(iter(driver.evaluate(
                            'return await browser.downloads.search({url:arguments[0]});', [origin.url(name)])), None),
                            'browser-created download record')
                        assert entry.get('byExtensionId') != ADDON_ID, 'Fixture bypassed the real browser download path'
                        return entry['id']

                    def capture(name, store=None):
                        browser_id = browser_click(name, store)
                        task = until(lambda: next((row for row in rows(data) if row['url'] == origin.url(name)
                                                   and row['state'] == 9), None), 'automatic Firefox handoff')
                        completed(task['id'])
                        entry = until(lambda: driver.evaluate('return (await browser.downloads.search({id:arguments[0]}))[0];',
                                                               [browser_id]), 'browser handoff state')
                        assert entry['state'] == 'interrupted', entry
                        return task

                    driver.evaluate('await browser.storage.local.set({autoCapture:true,includeCookies:false});')
                    capture('captured')
                    evidence['assertions'].append('Actual Firefox download automatically captured')
                    print('PASS actual Firefox automatic capture reaches durable native download')
                    container = driver.evaluate('return await browser.contextualIdentities.create({name:"Owned fixture",color:"blue",icon:"circle"});')
                    for name, store in (('standard', 'firefox-default'), ('container', container['cookieStoreId'])):
                        for selected_store in ('firefox-default', container['cookieStoreId']):
                            value = origin.cookies[name] if selected_store == store else secrets.token_hex(20)
                            origin.secrets.append(value)
                            driver.evaluate('return await browser.cookies.set(arguments[0]);', [{'url': origin.url(name),
                                'name': 'owned_session', 'value': value, 'path': '/' + name, 'storeId': selected_store}])
                        other_value = secrets.token_hex(20)
                        origin.secrets.append(other_value)
                        driver.evaluate('return await browser.cookies.set(arguments[0]);', [{'url': origin.url('unrelated'),
                            'name': 'owned_other_path', 'value': other_value, 'path': '/unrelated', 'storeId': store}])
                        driver.evaluate('await browser.storage.local.set({autoCapture:true,includeCookies:true});')
                        capture(name, store)
                        requests = [entry for entry in origin.requests if entry['path'] == name and entry['range']]
                        assert len(requests) >= 2 and all(entry['authenticated'] and entry['cookieNames'] == ['owned_session']
                                                         for entry in requests), requests
                    print('PASS Firefox standard/container cookie stores remain isolated with selected-URL credentials')
                    evidence['assertions'].append('Actual standard/container cookie stores isolated')
                    # Private forwarding must reject even when the test addon is explicitly allowed in private windows.
                    driver.evaluate('await browser.storage.local.remove("lastError");')
                    private_id = browser_click('private', private=True)
                    private_result = until(lambda: _complete_browser(driver, private_id), 'private browser fallback')
                    assert hashlib.sha256(Path(private_result['filename']).read_bytes()).hexdigest() == HASH
                    error = until(lambda: driver.evaluate('return (await browser.storage.local.get("lastError")).lastError;'),
                                  'private-context rejection')
                    assert '隐私窗口' in error, error
                    assert not any(row['url'] == origin.url('private') for row in rows(data))
                    print('PASS real private browser context rejects native cookie forwarding and preserves original download')
                    evidence['assertions'].append('Actual private download rejects forwarding and retains browser payload')

                    partition = {'url': origin.url('partitioned'), 'name': 'owned_partition', 'value': secrets.token_hex(20),
                                 'path': '/partitioned', 'storeId': 'firefox-default',
                                 'partitionKey': {'topLevelSite': 'https://owned-top.example'}}
                    origin.secrets.append(partition['value'])
                    driver.evaluate('return await browser.cookies.set(arguments[0]);', [partition])
                    actual_partition = driver.evaluate('return await browser.cookies.getAll(arguments[0]);',
                        [{'url': origin.url('partitioned'), 'storeId': 'firefox-default', 'partitionKey': {}}])
                    assert any(value['name'] == partition['name'] and value.get('partitionKey', {}).get('topLevelSite')
                               for value in actual_partition), 'Browser did not create the actual partitioned cookie'
                    driver.evaluate('await browser.storage.local.remove("lastError");')
                    partitioned_id = browser_click('partitioned', 'firefox-default')
                    partitioned_result = until(lambda: _complete_browser(driver, partitioned_id), 'partitioned browser fallback')
                    assert hashlib.sha256(Path(partitioned_result['filename']).read_bytes()).hexdigest() == HASH
                    error = until(lambda: driver.evaluate('return (await browser.storage.local.get("lastError")).lastError;'),
                                  'partition-context rejection')
                    assert '分区' in error, error
                    assert not any(row['url'] == origin.url('partitioned') for row in rows(data))
                    print('PASS actual partitioned cookie is detected; native handoff fails closed without browser data loss')
                    evidence['assertions'].append('Actual partitioned-cookie API context rejects native forwarding')

                    driver.evaluate('await browser.storage.local.set({autoCapture:false,includeCookies:false,pagePanel:true});')
                    panel_handle = open_http(origin.url('panel'))
                    until(lambda: driver.script('return !!document.querySelector("#maskpilot-download-panel")?.shadowRoot;'),
                          'actual injected page panel')

                    def panel_element(selector):
                        return driver.script('return document.querySelector("#maskpilot-download-panel").shadowRoot.querySelector(arguments[0]);',
                                             [selector])

                    trusted_click(panel_element('#panel-toggle'))
                    until(lambda: driver.script('return document.querySelector("#maskpilot-download-panel").shadowRoot.querySelectorAll("#panel-items input").length===2;'),
                          'two actual scanned resources')
                    trusted_click(panel_element('#panel-items input'))
                    baseline = {row['id'] for row in rows(data)}
                    driver.script('const root=document.querySelector("#maskpilot-download-panel").shadowRoot;'
                                  'const fields=[...root.querySelectorAll("#panel-items input")];'
                                  'for(const field of fields){field.value=fields[1].value;field.checked=true;}'
                                  'root.querySelector("#panel-send").click();')
                    time.sleep(.3)
                    assert {row['id'] for row in rows(data)} == baseline, 'Synthetic DOM click created native tasks'
                    trusted_click(panel_element('#panel-send'))
                    first = until(lambda: next((row for row in rows(data) if row['url'] == origin.url('panel-first')), None),
                                  'trusted panel-selected resource ACK')
                    completed(first['id'])
                    assert not any(row['url'] == origin.url('panel-second') for row in rows(data)), 'DOM tampering expanded trusted selection'
                    until(lambda: driver.script('return document.querySelector("#maskpilot-download-panel").shadowRoot.querySelector("#panel-items input")?.disabled;'),
                          'accepted panel resource is disabled')
                    until(lambda: driver.script('return document.querySelector("#maskpilot-download-panel").shadowRoot.querySelector("#panel-status")?.textContent.includes("已发送 1 项");'),
                          'actual panel accepted status')
                    if evidence_directory:
                        screenshot = driver.command('WebDriver:TakeScreenshot', {'full': False, 'scroll': False})
                        (evidence_directory / 'page-panel.png').write_bytes(base64.b64decode(screenshot, validate=True))
                    trusted_click(panel_element('#panel-items label:nth-child(2) input'))
                    driver.command('WebDriver:SwitchToWindow', {'handle': principal_handle})
                    driver.evaluate('await browser.storage.local.set({pagePanel:false});')
                    driver.command('WebDriver:SwitchToWindow', {'handle': panel_handle})
                    driver.script('const host=document.querySelector("#maskpilot-download-panel");host.hidden=false;'
                                  'host.shadowRoot.querySelector("#panel-body").hidden=false;')
                    trusted_click(panel_element('#panel-send'))
                    time.sleep(.3)
                    assert not any(row['url'] == origin.url('panel-second') for row in rows(data)), 'Disabled panel submitted a resource'
                    driver.command('WebDriver:SwitchToWindow', {'handle': principal_handle})
                    print('PASS trusted Firefox panel keeps closure selection despite DOM tampering and rejects disabled submission')
                    evidence['assertions'].append('Actual trusted panel sends only selected resource; synthetic and disabled clicks cannot add')
                    driver.evaluate('await browser.storage.local.set({autoCapture:true,includeCookies:false});')
                    moved = host.with_suffix('.unavailable')
                    until(lambda: _move(host, moved), 'owned host quiescence')
                    try:
                        revoked = driver.evaluate('return await browser.runtime.sendMessage({kind:"preferences",autoCapture:true,includeCookies:false,pagePanel:false});')
                        assert revoked['ok'], 'Consent revocation must not require the unavailable native host'
                        browser_id = browser_click('fallback')
                        entry = until(lambda: _complete_browser(driver, browser_id), 'Firefox fallback', timeout=45)
                        assert hashlib.sha256(Path(entry['filename']).read_bytes()).hexdigest() == HASH
                        assert not any(row['url'] == origin.url('fallback') for row in rows(data))
                    finally:
                        moved.rename(host)
                    print('PASS unavailable owned host preserves browser fallback without native duplicate')
                    evidence['assertions'].append('Unavailable native host leaves browser fallback with exact payload')
                    for secret in origin.cookies.values():
                        for path in data.rglob('*'):
                            if path.is_file():
                                content = path.read_bytes()
                                assert secret.encode() not in content and secret.encode('utf-16le') not in content
                    print('PASS selected cookie secrets are absent from durable desktop data')
                    evidence['result'] = 'passed'
                finally:
                    if driver:
                        with contextlib.suppress(Exception):
                            driver.command('Marionette:Quit', {'flags': ['eAttemptQuit']})
                        driver.close()
                    stop(firefox_process)
                    stop(desktop_process)
        except Exception as error:
            evidence['result'] = 'failed'
            evidence['failure'] = str(error)
            for name in ('desktop.log', 'firefox.log'):
                if (root / name).exists():
                    text = (root / name).read_text(errors='replace')[-5000:]
                    for secret in origin.secrets:
                        text = text.replace(secret, '[redacted]')
                    print(name + ': ' + text, file=sys.stderr)
            raise
        finally:
            origin.close()
            if evidence_directory:
                evidence['originRequests'] = origin.requests
                for name in ('desktop.log', 'firefox.log'):
                    if (root / name).exists():
                        text = (root / name).read_text(errors='replace')
                        for secret in origin.secrets:
                            text = text.replace(secret, '[redacted]')
                        (evidence_directory / name).write_text(text, encoding='utf-8')
                serialized = json.dumps(evidence, indent=2, ensure_ascii=False)
                for secret in origin.secrets:
                    serialized = serialized.replace(secret, '[redacted]')
                (evidence_directory / 'evidence.json').write_text(serialized, encoding='utf-8')
                print('Evidence:', evidence_directory)


def _move(source, destination):
    source.rename(destination)
    return True


def _complete_browser(driver, identifier):
    entry = driver.evaluate('return (await browser.downloads.search({id:arguments[0]}))[0];', [identifier])
    return entry if entry and entry['state'] == 'complete' else None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build-directory', type=Path, required=True)
    parser.add_argument('--extension', type=Path, default=Path('browser_extension/dist-firefox'))
    parser.add_argument('--browser', type=Path, default=Path(os.environ.get('LOCALAPPDATA', '')) /
                        'ms-playwright/firefox-1522/firefox/firefox.exe')
    parser.add_argument('--evidence-directory', type=Path)
    args = parser.parse_args()
    exercise(args.build_directory.resolve(), args.extension.resolve(), args.browser.resolve(),
             args.evidence_directory.resolve() if args.evidence_directory else None)


if __name__ == '__main__':
    main()
