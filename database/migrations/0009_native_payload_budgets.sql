-- Applied transactionally after backup. Historical traffic intentionally has no task/queue delete cascade.
ALTER TABLE download_queues ADD COLUMN daily_payload_budget INTEGER NOT NULL DEFAULT 0 CHECK(daily_payload_budget BETWEEN 0 AND 1125899906842624);
CREATE TABLE traffic_clock(singleton INTEGER PRIMARY KEY CHECK(singleton=1),period INTEGER NOT NULL CHECK(period>=0 AND period%86400=0));
INSERT INTO traffic_clock VALUES(1,0);
CREATE TABLE queue_traffic_usage(queue_id TEXT NOT NULL,period INTEGER NOT NULL CHECK(period>=0 AND period%86400=0),
 charged INTEGER NOT NULL DEFAULT 0 CHECK(charged BETWEEN 0 AND 1125899906842624),
 observed INTEGER NOT NULL DEFAULT 0 CHECK(observed>=0 AND observed<=charged),PRIMARY KEY(queue_id,period));
CREATE TABLE queue_traffic_runs(id TEXT PRIMARY KEY,task_id TEXT NOT NULL,queue_id TEXT NOT NULL,
 period INTEGER NOT NULL CHECK(period>=0 AND period%86400=0),status INTEGER NOT NULL DEFAULT 0 CHECK(status IN(0,1,2)),
 reserved INTEGER NOT NULL DEFAULT 0 CHECK(reserved BETWEEN 0 AND 1125899906842624),
 observed INTEGER NOT NULL DEFAULT 0 CHECK(observed>=0 AND observed<=reserved));
CREATE INDEX traffic_runs_status ON queue_traffic_runs(status);
PRAGMA user_version=9;
