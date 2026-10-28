-- Run chat: client_id is unique per sender, not per run (0.23 Together review). 0027 declared UNIQUE (run_id,
-- client_id), but a resend is recognised per (run, user, client_id): two members choosing the same client_id made the
-- insert of the second one fail. SQLite cannot alter a constraint, so the table is rebuilt; rows keep their values.
CREATE TABLE run_chat_messages_new (
    run_id    TEXT NOT NULL REFERENCES runs(id) ON DELETE CASCADE,
    seq       INTEGER NOT NULL,
    user_id   TEXT NOT NULL,
    text      TEXT NOT NULL,
    mentions  TEXT NOT NULL DEFAULT '[]',
    client_id TEXT NOT NULL,
    sent_at   INTEGER NOT NULL,
    PRIMARY KEY (run_id, seq),
    UNIQUE (run_id, user_id, client_id)
);
INSERT INTO run_chat_messages_new(run_id, seq, user_id, text, mentions, client_id, sent_at)
    SELECT run_id, seq, user_id, text, mentions, client_id, sent_at FROM run_chat_messages;
DROP TABLE run_chat_messages;
ALTER TABLE run_chat_messages_new RENAME TO run_chat_messages;
