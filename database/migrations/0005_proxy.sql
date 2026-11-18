-- Applied transactionally by the repository after a migration backup.
-- Proxy usernames and passwords are deliberately absent from task persistence.
ALTER TABLE tasks ADD COLUMN proxy_auth_required INTEGER NOT NULL DEFAULT 0;
PRAGMA user_version=5;
