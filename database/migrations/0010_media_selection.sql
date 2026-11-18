-- Applied transactionally after backup. Legacy automatic/audio presets retain their existing columns.
ALTER TABLE tasks ADD COLUMN media_selection_mode INTEGER NOT NULL DEFAULT 0 CHECK(media_selection_mode BETWEEN 0 AND 4);
ALTER TABLE tasks ADD COLUMN media_primary_format TEXT NOT NULL DEFAULT '';
ALTER TABLE tasks ADD COLUMN media_audio_format TEXT NOT NULL DEFAULT '';
ALTER TABLE tasks ADD COLUMN media_container INTEGER NOT NULL DEFAULT 0 CHECK(media_container BETWEEN 0 AND 7);
PRAGMA user_version=10;
