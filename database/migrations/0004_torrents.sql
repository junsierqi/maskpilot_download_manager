-- Append-only kind/state mapping preserves all earlier task values and media JSON compatibility.
ALTER TABLE tasks ADD COLUMN kind INTEGER NOT NULL DEFAULT 0;
ALTER TABLE tasks ADD COLUMN bt_all INTEGER NOT NULL DEFAULT 1;
ALTER TABLE tasks ADD COLUMN bt_selection TEXT NOT NULL DEFAULT '';
ALTER TABLE tasks ADD COLUMN bt_seed INTEGER NOT NULL DEFAULT 0;
ALTER TABLE tasks ADD COLUMN bt_upload_limit INTEGER NOT NULL DEFAULT 0;
ALTER TABLE tasks ADD COLUMN bt_metadata BLOB NOT NULL DEFAULT X'';
ALTER TABLE tasks ADD COLUMN bt_published INTEGER NOT NULL DEFAULT 0;
ALTER TABLE tasks ADD COLUMN bt_uploaded INTEGER NOT NULL DEFAULT 0;
UPDATE tasks SET kind=CASE WHEN media<>0 THEN 2
    WHEN url LIKE 'ftp://%' OR url LIKE 'ftps://%' THEN 1 ELSE 0 END;
PRAGMA user_version=4;
