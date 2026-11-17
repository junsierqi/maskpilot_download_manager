-- Credentials deliberately remain process-local and are never stored here.
ALTER TABLE tasks ADD COLUMN ftp_tls INTEGER NOT NULL DEFAULT 0;
ALTER TABLE tasks ADD COLUMN ftp_active INTEGER NOT NULL DEFAULT 0;
ALTER TABLE tasks ADD COLUMN ftp_ca TEXT NOT NULL DEFAULT '';
PRAGMA user_version=3;
