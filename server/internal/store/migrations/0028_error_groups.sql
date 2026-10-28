-- 0.24 Error events and error groups (ADR 0035 D1, handshake feature reports_v2): apps and the server record errors
-- of known kinds; events with the same fingerprint form one group. A group is kept until an admin deletes it, at
-- most 200 events per group are kept (the oldest are dropped, count keeps the total). The group "seen" state is a
-- revision pair: every event raises rev, "seen" copies it into seen_rev, so seen = (rev = seen_rev) needs no clock.
-- User and device names are copied: the events stay readable when either is removed.
CREATE TABLE error_groups (
    fingerprint TEXT PRIMARY KEY,
    kind        TEXT NOT NULL,
    component   TEXT NOT NULL,
    title       TEXT NOT NULL,
    count       INTEGER NOT NULL DEFAULT 0,
    first_seen  INTEGER NOT NULL,
    last_seen   INTEGER NOT NULL,
    issue_url   TEXT,
    rev         INTEGER NOT NULL DEFAULT 1,
    seen_rev    INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX error_groups_last_seen ON error_groups(last_seen DESC);

CREATE TABLE error_events (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    fingerprint TEXT NOT NULL REFERENCES error_groups(fingerprint) ON DELETE CASCADE,
    device_id   TEXT NOT NULL,
    device_name TEXT NOT NULL,
    user_name   TEXT NOT NULL DEFAULT '',
    app_version TEXT NOT NULL DEFAULT '',
    os          TEXT NOT NULL DEFAULT '',
    message     TEXT NOT NULL DEFAULT '',
    occurred_at INTEGER NOT NULL,
    received_at INTEGER NOT NULL
);
CREATE INDEX error_events_group ON error_events(fingerprint, id);

-- Distinct devices that ever sent an event of the group (the events table is capped).
CREATE TABLE error_group_devices (
    fingerprint TEXT NOT NULL REFERENCES error_groups(fingerprint) ON DELETE CASCADE,
    device_id   TEXT NOT NULL,
    PRIMARY KEY (fingerprint, device_id)
);
