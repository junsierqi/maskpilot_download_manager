-- Original schema; runtime uses the same shape through the repository adapter.
CREATE TABLE IF NOT EXISTS tasks (
    id TEXT PRIMARY KEY, url TEXT NOT NULL, destination TEXT NOT NULL,
    category TEXT NOT NULL, referrer TEXT NOT NULL, user_agent TEXT NOT NULL,
    proxy TEXT NOT NULL, expected_sha256 TEXT NOT NULL,
    connections INTEGER NOT NULL, priority INTEGER NOT NULL, max_retries INTEGER NOT NULL,
    speed_limit INTEGER NOT NULL, scheduled_at INTEGER NOT NULL, stop_at INTEGER NOT NULL,
    state INTEGER NOT NULL, downloaded INTEGER NOT NULL, total INTEGER NOT NULL,
    created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL, retries INTEGER NOT NULL,
    resumable INTEGER NOT NULL, error TEXT NOT NULL
);
PRAGMA user_version=1;
