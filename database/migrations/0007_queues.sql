-- Applied transactionally after backup. Queue membership does not change task ownership or paths.
CREATE TABLE download_queues(id TEXT PRIMARY KEY,name TEXT NOT NULL UNIQUE,revision INTEGER NOT NULL CHECK(revision>=1),
 enabled INTEGER NOT NULL CHECK(enabled IN(0,1)),max_active INTEGER NOT NULL CHECK(max_active BETWEEN 1 AND 8),
 download_limit INTEGER NOT NULL CHECK(download_limit=0 OR download_limit>=1024),
 upload_limit INTEGER NOT NULL CHECK(upload_limit=0 OR upload_limit>=1024));
INSERT INTO download_queues VALUES('default','Default',1,1,8,0,0);
ALTER TABLE tasks ADD COLUMN queue_id TEXT NOT NULL DEFAULT 'default';
ALTER TABLE tasks ADD COLUMN run_intent INTEGER NOT NULL DEFAULT 1 CHECK(run_intent IN(0,1));
UPDATE tasks SET run_intent=0 WHERE state=1;
CREATE INDEX tasks_queue_state ON tasks(queue_id,state,priority,created_at,id);
PRAGMA user_version=7;
