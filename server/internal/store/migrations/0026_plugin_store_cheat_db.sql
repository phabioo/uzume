-- 0.20 package 3: the plugin user store (fb.store) and the hash cache of the cheat database matching.
-- plugin_store: one JSON value per user, plugin id and key; revision counts the writes of the key (the app sends the
-- revision it based a change on and gets a conflict when the server has moved on). A removed user takes the rows with them.
CREATE TABLE plugin_store (
    user_id    TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    plugin_id  TEXT NOT NULL,
    key        TEXT NOT NULL,
    value      TEXT NOT NULL, -- compact JSON, at most 256 KiB
    size_bytes INTEGER NOT NULL,
    revision   INTEGER NOT NULL CHECK (revision >= 1),
    updated_at INTEGER NOT NULL,
    PRIMARY KEY (user_id, plugin_id, key)
);

-- rom_hashes: SHA-1 and CRC32 of a stored ROM (key: its SHA-256), computed once when a cheat list is looked up, because
-- the No-Intro DATs name ROMs by those hashes. No foreign key: it is a cache and can be dropped at any time.
CREATE TABLE rom_hashes (
    sha256 TEXT PRIMARY KEY,
    sha1   TEXT NOT NULL,
    crc32  TEXT NOT NULL
);
