-- Run chat (0.23 Together, ADR 0033 D4). seq is per run, from 1; the hub keeps at most 10000 rows per run and
-- deletes the oldest on insert. client_id makes sends idempotent. Session chat lives in memory only.
CREATE TABLE run_chat_messages (
    run_id    TEXT NOT NULL REFERENCES runs(id) ON DELETE CASCADE,
    seq       INTEGER NOT NULL,
    user_id   TEXT NOT NULL,
    text      TEXT NOT NULL,
    mentions  TEXT NOT NULL DEFAULT '[]',
    client_id TEXT NOT NULL,
    sent_at   INTEGER NOT NULL,
    PRIMARY KEY (run_id, seq),
    UNIQUE (run_id, client_id)
);
