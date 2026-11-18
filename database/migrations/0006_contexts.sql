-- Applied transactionally after backup. Secrets and per-start grants are never stored here.
ALTER TABLE tasks ADD COLUMN owner_id TEXT NOT NULL DEFAULT 'standalone';
ALTER TABLE tasks ADD COLUMN request_id TEXT NOT NULL DEFAULT '';
ALTER TABLE tasks ADD COLUMN context_revision INTEGER NOT NULL DEFAULT 0;
ALTER TABLE tasks ADD COLUMN proxy_credential_ref TEXT NOT NULL DEFAULT '';
ALTER TABLE tasks ADD COLUMN origin_auth_required INTEGER NOT NULL DEFAULT 0;
ALTER TABLE tasks ADD COLUMN request_name TEXT NOT NULL DEFAULT '';
UPDATE tasks SET request_id=id WHERE owner_id='standalone';
CREATE UNIQUE INDEX tasks_owner_request ON tasks(owner_id,request_id) WHERE request_id<>'';
CREATE TABLE download_contexts(
 id TEXT PRIMARY KEY, external_key TEXT NOT NULL UNIQUE, label TEXT NOT NULL,
 download_root TEXT NOT NULL, proxy_endpoint TEXT NOT NULL,
 proxy_auth_required INTEGER NOT NULL CHECK(proxy_auth_required IN(0,1)),
 proxy_credential_ref TEXT NOT NULL, origin_auth_required INTEGER NOT NULL CHECK(origin_auth_required IN(0,1)),
 revision INTEGER NOT NULL CHECK(revision>0), enabled INTEGER NOT NULL CHECK(enabled IN(0,1)));
CREATE TABLE credential_records(reference TEXT PRIMARY KEY,owner_id TEXT NOT NULL,proxy_endpoint TEXT NOT NULL,
 ownership_nonce TEXT NOT NULL DEFAULT '');
CREATE TABLE store_metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL);
PRAGMA user_version=6;
