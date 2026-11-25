"""Exercise unattended desktop startup faults without touching user data."""
import argparse
from contextlib import closing
import hashlib
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile


def exercise(desktop):
    environment = dict(os.environ, QT_QPA_PLATFORM="offscreen")
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)

    def start(label, data, expected, *options):
        try:
            result = subprocess.run(
                [str(desktop), "--background", "--data-dir", str(data), *options],
                env=environment, capture_output=True, timeout=15, creationflags=flags)
        except subprocess.TimeoutExpired as error:
            raise AssertionError(f"{label}: background startup did not exit within 15 seconds") from error
        assert result.returncode == expected, (
            label, result.returncode, result.stderr[-1500:].decode(errors="replace"))
        if data.is_dir():
            assert not (data / "instance.lock").exists(), f"{label}: startup retained its lock"

    with tempfile.TemporaryDirectory(prefix="maskpilot startup failure ") as temporary:
        root = Path(temporary).resolve()
        blocked = root / "data-is-file"
        marker = b"owned startup blocker\n"
        blocked.write_bytes(marker)
        start("Invalid data directory", blocked, 1)
        assert blocked.read_bytes() == marker
        print("PASS invalid data directory fails unattended and preserves the existing file", flush=True)

        token_data = root / "token-blocked"
        token_marker = token_data / "ipc.token" / "keep"
        token_marker.parent.mkdir(parents=True)
        token_marker.write_bytes(marker)
        start("Token publication failure", token_data, 1)
        assert token_marker.read_bytes() == marker
        assert not (token_data / "tasks.db").exists()
        print("PASS token publication failure exits without replacing its owned blocker", flush=True)

        future_data = root / "future-schema"
        future_data.mkdir()
        future_database = future_data / "tasks.db"
        with closing(sqlite3.connect(future_database)) as database:
            with database:
                database.execute("CREATE TABLE fixture_marker(value TEXT)")
                database.execute("INSERT INTO fixture_marker VALUES ('owned future data')")
                database.execute("PRAGMA user_version=9999")
        before = hashlib.sha256(future_database.read_bytes()).digest()
        start("Future database schema", future_data, 1)
        assert hashlib.sha256(future_database.read_bytes()).digest() == before
        print("PASS future-schema refusal exits unattended and preserves database bytes", flush=True)

        healthy = root / "healthy"
        start("Healthy background smoke", healthy, 0, "--smoke-test")
        database_uri = (healthy / "tasks.db").as_uri() + "?mode=ro"
        with closing(sqlite3.connect(database_uri, uri=True)) as database:
            assert database.execute("SELECT COUNT(*) FROM tasks").fetchone()[0] == 0
            assert database.execute("PRAGMA user_version").fetchone()[0] > 0
        for filename in ("ipc.token", "ipc-admin.token"):
            token = (healthy / filename).read_bytes()
            assert len(token) == 64 and all(value in b"0123456789abcdef" for value in token)
        print("PASS healthy background startup initializes storage and bridge before clean exit", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--desktop", type=Path, required=True)
    arguments = parser.parse_args()
    exercise(arguments.desktop.resolve(strict=True))
