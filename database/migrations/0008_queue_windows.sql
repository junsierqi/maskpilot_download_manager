-- Applied transactionally after backup; previous queues stay unrestricted until explicitly enabled.
ALTER TABLE download_queues ADD COLUMN schedule_enabled INTEGER NOT NULL DEFAULT 0 CHECK(schedule_enabled IN(0,1));
ALTER TABLE download_queues ADD COLUMN schedule_weekdays INTEGER NOT NULL DEFAULT 127 CHECK(schedule_weekdays BETWEEN 1 AND 127);
ALTER TABLE download_queues ADD COLUMN schedule_start INTEGER NOT NULL DEFAULT 0 CHECK(schedule_start BETWEEN 0 AND 1439);
ALTER TABLE download_queues ADD COLUMN schedule_end INTEGER NOT NULL DEFAULT 0 CHECK(schedule_end BETWEEN 0 AND 1439);
ALTER TABLE download_queues ADD COLUMN schedule_timezone TEXT NOT NULL DEFAULT 'UTC';
PRAGMA user_version=8;
