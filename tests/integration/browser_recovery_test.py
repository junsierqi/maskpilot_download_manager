"""Actual native-host/desktop recovery with isolated SQLite, cookies and partial HTTP data."""
import argparse
from contextlib import closing
import hashlib
import http.server
import json
import os
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

ORIGIN = "chrome-extension://epdfgbhpbpkpephcfmemmadlblpodnfo/"
OLD = bytes(range(256)) * 32768
NEW = bytes(reversed(range(256))) * 32768
OLD_COOKIE = "session=old-recovery-fixture"
NEW_COOKIE = "session=fresh-recovery-fixture"


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    block_on_close = False

    def handle_error(self, request, client_address):
        if not isinstance(sys.exception(), (ConnectionResetError, ConnectionAbortedError, BrokenPipeError)):
            super().handle_error(request, client_address)


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def do_HEAD(self):
        self.transfer(False)

    def do_GET(self):
        self.transfer(True)

    def transfer(self, body):
        old = self.path.startswith("/old")
        expected = OLD_COOKIE if old else NEW_COOKIE
        with self.server.observations_lock:
            self.server.observations.append((self.path, self.headers.get("Cookie")))
        if self.headers.get("Cookie") != expected:
            self.send_response(401)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        payload = OLD if old else NEW
        start, end = 0, len(payload) - 1
        value = self.headers.get("Range", "")
        if value:
            parts = value.removeprefix("bytes=").split("-", 1)
            start = int(parts[0])
            if parts[1]:
                end = min(end, int(parts[1]))
        self.send_response(206 if value else 200)
        self.send_header("Content-Length", str(end - start + 1))
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("ETag", '"old-fixture"' if old else '"new-fixture"')
        self.send_header("Content-Type", "application/octet-stream")
        if value:
            self.send_header("Content-Range", f"bytes {start}-{end}/{len(payload)}")
        self.end_headers()
        if not body:
            return
        try:
            for offset in range(start, end + 1, 16384):
                self.wfile.write(payload[offset:min(offset + 16384, end + 1)])
                self.wfile.flush()
                if old:
                    time.sleep(0.02)
        except (BrokenPipeError, ConnectionError):
            pass


def frame(value):
    body = json.dumps(value).encode("utf-8")
    return struct.pack("<I", len(body)) + body


def exercise(desktop, host):
    environment = os.environ.copy()
    environment.update(QT_QPA_PLATFORM="offscreen", NO_PROXY="127.0.0.1,localhost",
                       no_proxy="127.0.0.1,localhost")
    environment["PATH"] = str(host.parent) + os.pathsep + environment.get("PATH", "")
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    with tempfile.TemporaryDirectory(prefix="maskpilot-browser-recovery-") as temporary:
        root = Path(temporary).resolve()
        data, downloads, isolated = root / "data", root / "downloads", root / "host"
        downloads.mkdir()
        isolated.mkdir()
        (data / "MaskPilot").mkdir(parents=True)
        (data / "MaskPilot/MaskPilot.ini").write_text(
            "[General]\ndownloadDirectory=" + downloads.as_posix() + "\nlanguage=en\n", encoding="utf-8")
        isolated_host = isolated / host.name
        shutil.copy2(host, isolated_host)
        (isolated / "native-bridge.json").write_text(json.dumps({
            "allowedOrigins": [ORIGIN], "desktopPath": str(desktop), "dataDirectory": str(data)}), encoding="utf-8")
        server = Server(("127.0.0.1", 0), Handler)
        server.observations, server.observations_lock = [], threading.Lock()
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        process = None

        def query(command, **fields):
            request = {"version": 1, "requestId": uuid.uuid4().hex, "command": command, **fields}
            result = subprocess.run([str(isolated_host), ORIGIN], input=frame(request), capture_output=True,
                                    env=environment, timeout=20, creationflags=flags)
            assert len(result.stdout) >= 4, (result.returncode, "No framed native response")
            length = struct.unpack("<I", result.stdout[:4])[0]
            assert length == len(result.stdout) - 4
            response = json.loads(result.stdout[4:])
            assert response["requestId"] == request["requestId"]
            assert (result.returncode == 0) == response["ok"]
            return response

        def start():
            nonlocal process
            token_file = data / "ipc.token"
            previous = token_file.read_bytes() if token_file.exists() else b""
            process = subprocess.Popen([str(desktop), "--background", "--data-dir", str(data)],
                                       env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                       creationflags=flags)
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                assert process.poll() is None, "Owned desktop exited before ready"
                if token_file.exists() and token_file.read_bytes() != previous:
                    assert query("ping")["ok"]
                    return
                time.sleep(0.05)
            raise AssertionError("Owned desktop startup timeout")

        def stop():
            nonlocal process
            if process is not None:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)
                process = None

        def record(task_id):
            with closing(sqlite3.connect(data / "tasks.db")) as database:
                database.row_factory = sqlite3.Row
                row = database.execute("SELECT * FROM tasks WHERE id=?", (task_id,)).fetchone()
                return dict(row) if row else None

        def wait(task_id, predicate, label, timeout=30):
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                row = record(task_id)
                if row and predicate(row):
                    return row
                assert process.poll() is None, "Owned desktop exited"
                time.sleep(0.05)
            row = record(task_id) or {}
            raise AssertionError(f"{label}; state={row.get('state')}, downloaded={row.get('downloaded')}")

        try:
            start()
            origin = f"http://127.0.0.1:{server.server_port}"
            prepared = query("prepare", url=origin + "/old", cookie=OLD_COOKIE,
                             referrer=origin + "/page", filename="original-target.bin")
            assert prepared["ok"]
            task_id = prepared["taskId"]
            original = record(task_id)
            assert original and not server.observations
            assert query("resume", taskId=task_id)["ok"]
            # Progress is deliberately published through IPC, not written to SQLite on every chunk.
            deadline = time.monotonic() + 15
            partial = None
            while time.monotonic() < deadline:
                partial = next((row for row in query("list")["tasks"] if row["id"] == task_id), None)
                if partial and 0 < partial["downloaded"] < len(OLD):
                    break
                time.sleep(0.05)
            else:
                raise AssertionError("No real partial HTTP progress through IPC")
            assert query("pause", taskId=task_id)["ok"]
            paused = wait(task_id, lambda row: row["state"] == 5, "Partial task did not pause")
            assert 0 < paused["downloaded"] < len(OLD), "Paused task must preserve actual partial bytes"
            assert not Path(original["destination"]).exists()
            stale = query("recovery.begin", taskId=task_id)
            assert stale["ok"]
            assert query("pause", taskId=task_id)["ok"]
            material = dict(url=origin + "/new?signature=fixture", cookie=NEW_COOKIE,
                            referrer=origin + "/page", userAgent="recovery-fixture")
            assert not query("recovery.apply", recoveryToken=stale["recoveryToken"], **material)["ok"]
            cancelled = query("recovery.begin", taskId=task_id)
            assert query("recovery.cancel", recoveryToken=cancelled["recoveryToken"])["ok"]
            assert not query("recovery.apply", recoveryToken=cancelled["recoveryToken"], **material)["ok"]
            session = query("recovery.begin", taskId=task_id)
            token = session["recoveryToken"]
            assert not query("recovery.resume", recoveryToken=token)["ok"], "Unapplied session resumed a task"
            assert session["pageUrl"] == origin + "/page"
            assert not query("recovery.apply", version=2, sessionToken="0" * 32,
                             params={"recoveryToken": token, **material})["ok"]
            assert not query("recovery.apply", recoveryToken=token, **{**material, "cookie": ""})["ok"]
            assert not query("recovery.apply", recoveryToken=token, destination="C:/outside.bin", **material)["ok"]
            assert not query("recovery.apply", recoveryToken=token, **{**material, "url": "ftp://example.test/file"})["ok"]
            with closing(sqlite3.connect(data / "tasks.db")) as database:
                database.execute("CREATE TRIGGER recovery_fixture_save_failure BEFORE UPDATE ON tasks "
                                 "WHEN NEW.url LIKE '%rejectsave%' BEGIN "
                                 "SELECT RAISE(ABORT, 'owned recovery fixture'); END")
                database.commit()
            assert not query("recovery.apply", recoveryToken=token,
                             **{**material, "url": origin + "/rejectsave"})["ok"]
            assert record(task_id)["url"] == original["url"], "Failed save changed the original task"
            with closing(sqlite3.connect(data / "tasks.db")) as database:
                database.execute("DROP TRIGGER recovery_fixture_save_failure")
                database.commit()
            assert query("recovery.apply", recoveryToken=token, **material)["taskId"] == task_id
            saved = record(task_id)  # The actual native ACK must follow durable persistence.
            assert saved["url"] == material["url"] and saved["state"] == 5
            assert saved["destination"] == original["destination"]
            assert saved["queue_id"] == original["queue_id"]
            assert query("recovery.apply", recoveryToken=token, **material)["taskId"] == task_id
            assert not query("recovery.apply", recoveryToken=token, **{**material, "cookie": "changed"})["ok"]
            restart_session = query("recovery.begin", taskId=task_id)
            assert not query("recovery.resume", recoveryToken=token)["ok"], "Replaced receipt resumed a task"
            stop()
            start()
            assert not query("recovery.apply", recoveryToken=restart_session["recoveryToken"], **material)["ok"]
            before = len(server.observations)
            assert query("resume", taskId=task_id)["ok"]
            wait(task_id, lambda row: row["state"] == 13, "Missing persisted authentication requirement")
            assert len(server.observations) == before, "Restart anonymously contacted the source"
            assert query("pause", taskId=task_id)["ok"]
            session = query("recovery.begin", taskId=task_id)
            assert session["ok"]
            assert query("recovery.apply", recoveryToken=session["recoveryToken"], **material)["taskId"] == task_id
            assert query("recovery.resume", recoveryToken=session["recoveryToken"])["taskId"] == task_id
            completed = wait(task_id, lambda row: row["state"] == 9, "Recovered HTTP task did not complete")
            output = Path(completed["destination"])
            assert hashlib.sha256(output.read_bytes()).digest() == hashlib.sha256(NEW).digest()
            assert not query("recovery.begin", taskId=task_id)["ok"]
            assert len(query("list")["tasks"]) == 1, "Recovery created a duplicate task"
            for path in data.glob("tasks.db*"):
                raw = path.read_bytes()
                for secret in (OLD_COOKIE, NEW_COOKIE, token, restart_session["recoveryToken"], session["recoveryToken"]):
                    assert secret.encode() not in raw, "Recovery secret appeared in durable task data"
            assert output.name == "original-target.bin"
            print("PASS actual partial HTTP recovery, durable same-ID ACK, replay/stale/cancel rejection,")
            print("     restart login requirement, no secret persistence and changed-source full-file hash.")
        finally:
            stop()
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--desktop", type=Path, required=True)
    parser.add_argument("--host", type=Path, required=True)
    args = parser.parse_args()
    exercise(args.desktop.resolve(), args.host.resolve())
