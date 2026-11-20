BEGIN IMMEDIATE;
ALTER TABLE tasks ADD COLUMN http_method INTEGER NOT NULL DEFAULT 0 CHECK(http_method IN(0,1));
ALTER TABLE tasks ADD COLUMN post_content_type TEXT NOT NULL DEFAULT 'application/x-www-form-urlencoded';
PRAGMA user_version=16;
COMMIT;
