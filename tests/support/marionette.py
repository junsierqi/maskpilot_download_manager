"""Small bounded Marionette client for an explicitly owned Firefox test process."""
import json
import socket
import time


class Marionette:
    def __init__(self, port, timeout=30):
        deadline = time.monotonic() + timeout
        while True:
            try:
                self.socket = socket.create_connection(('127.0.0.1', port), timeout=2)
                break
            except OSError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(.1)
        self.socket.settimeout(timeout)
        self.next_id = 0
        greeting = self._receive()
        assert greeting.get('marionetteProtocol') == 3, greeting
        self.command('WebDriver:NewSession', {'capabilities': {'alwaysMatch': {
            'acceptInsecureCerts': False, 'pageLoadStrategy': 'normal'}}})
        self.command('WebDriver:SetTimeouts', {'script': 30000, 'pageLoad': 30000, 'implicit': 0})

    def _exact(self, length):
        result = bytearray()
        while len(result) < length:
            part = self.socket.recv(length - len(result))
            if not part:
                raise ConnectionError('Owned Marionette connection closed')
            result.extend(part)
        return bytes(result)

    def _receive(self):
        prefix = bytearray()
        while True:
            value = self._exact(1)
            if value == b':':
                break
            if len(prefix) >= 9 or not value.isdigit():
                raise ValueError('Invalid Marionette frame length')
            prefix.extend(value)
        length = int(prefix)
        if not 0 < length <= 16 * 1024 * 1024:
            raise ValueError('Marionette frame exceeds fixture bound')
        return json.loads(self._exact(length))

    def command(self, name, parameters=None):
        self.next_id += 1
        packet = json.dumps([0, self.next_id, name, parameters or {}], ensure_ascii=False).encode('utf-8')
        self.socket.sendall(str(len(packet)).encode() + b':' + packet)
        response = self._receive()
        assert isinstance(response, list) and len(response) == 4 and response[:2] == [1, self.next_id], response
        if response[2] is not None:
            raise RuntimeError(f'{name}: {response[2]}')
        result = response[3]
        return result.get('value') if isinstance(result, dict) and set(result) == {'value'} else result

    def context(self, name):
        return self.command('Marionette:SetContext', {'value': name})

    def script(self, body, arguments=None):
        return self.command('WebDriver:ExecuteScript', {'script': body, 'args': arguments or [],
                                                       'newSandbox': False, 'sandbox': None})

    def evaluate(self, expression, arguments=None):
        body = ("const done=arguments[arguments.length-1];const browser=(window.wrappedJSObject ?? window).browser;"
                "Promise.resolve((async()=>{" + expression + "})()).then("
                "value=>done(value===undefined?null:JSON.parse(JSON.stringify(value))),"
                "error=>done({__fixtureError:String(error)}));")
        value = self.command('WebDriver:ExecuteAsyncScript', {'script': body, 'args': arguments or [],
                                                             'newSandbox': False, 'sandbox': None})
        if isinstance(value, dict) and '__fixtureError' in value:
            raise RuntimeError(value['__fixtureError'])
        return value

    def navigate(self, url):
        return self.command('WebDriver:Navigate', {'url': url})

    def install(self, path, allow_private=False):
        return self.command('Addon:Install', {'path': str(path), 'temporary': True,
                                             'allowPrivateBrowsing': allow_private})

    def close(self):
        self.socket.close()
