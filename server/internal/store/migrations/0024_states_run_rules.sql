-- 0.20 package 2: save states per user, game, save slot and state number; core run rules; "cheats used" marker on saves.
-- The state blobs and previews live under <data>/states/<user_id>/<game_id>/<slot>/<n>.state and <n>.png; this table
-- holds metadata only. Like saves, states have no foreign key to games: deleting a game keeps its states (the
-- admin clean-up "deleted_game_states" removes them). A removed user takes the rows with them (the service removes the files).
CREATE TABLE save_states (
    user_id       TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    game_id       TEXT NOT NULL,
    slot          TEXT NOT NULL,
    n             INTEGER NOT NULL CHECK (n >= 0 AND n <= 10), -- 0 = quick slot
    core_id       TEXT NOT NULL,
    core_version  TEXT NOT NULL,
    size_bytes    INTEGER NOT NULL,
    preview_bytes INTEGER NOT NULL DEFAULT 0, -- 0 = no preview
    sha256        TEXT NOT NULL,
    device_id     TEXT NOT NULL, -- no foreign key: the row keeps the id after a device is deleted
    created_at    INTEGER NOT NULL,
    PRIMARY KEY (user_id, game_id, slot, n)
);
CREATE INDEX save_states_game ON save_states(game_id);

-- Core run rules (not plugin settings): {"states":bool,"rewind":bool,"cheats":"off|logged","memory_write":"off|logged"}.
-- Existing runs get the defaults (everything off).
ALTER TABLE runs ADD COLUMN rules TEXT NOT NULL DEFAULT '{"states":false,"rewind":false,"cheats":"off","memory_write":"off"}';

-- A checkpoint or history version uploaded while cheats or memory writes were active (apps send cheats_used).
ALTER TABLE save_slots ADD COLUMN cheats_used INTEGER NOT NULL DEFAULT 0;
ALTER TABLE save_history ADD COLUMN cheats_used INTEGER NOT NULL DEFAULT 0;
