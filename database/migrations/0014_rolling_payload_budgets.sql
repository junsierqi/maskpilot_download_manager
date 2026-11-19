BEGIN IMMEDIATE;
ALTER TABLE download_queues ADD COLUMN payload_window_hours INTEGER NOT NULL DEFAULT 0
 CHECK(payload_window_hours BETWEEN 0 AND 168);
ALTER TABLE queue_traffic_runs ADD COLUMN closed_utc INTEGER NOT NULL DEFAULT 0 CHECK(closed_utc>=0);
UPDATE queue_traffic_runs SET closed_utc=period+86400 WHERE status<>0;
ALTER TABLE traffic_clock ADD COLUMN utc_highwater INTEGER NOT NULL DEFAULT 0 CHECK(utc_highwater>=0);
UPDATE traffic_clock SET utc_highwater=period;
ALTER TABLE queue_traffic_usage RENAME TO queue_traffic_usage_v13;
CREATE TABLE queue_traffic_usage(queue_id TEXT NOT NULL,period INTEGER NOT NULL CHECK(period>=0 AND period%86400=0),
 charged INTEGER NOT NULL DEFAULT 0 CHECK(charged>=0),
 observed INTEGER NOT NULL DEFAULT 0 CHECK(observed>=0 AND observed<=charged),PRIMARY KEY(queue_id,period));
INSERT INTO queue_traffic_usage SELECT * FROM queue_traffic_usage_v13;
DROP TABLE queue_traffic_usage_v13;
CREATE INDEX traffic_runs_queue_close ON queue_traffic_runs(queue_id,status,closed_utc);
PRAGMA user_version=14;
COMMIT;
