-- 0.21 Randomizer (ADR 0031): the tools catalog (the randomizer engine, offered to apps the way cores are), game variants
-- (a library game plus a recipe, played in an own save slot) and runs with a randomizer recipe. The server stores
-- recipes only: no ROM data, patches or spoiler logs of a variant.

-- One row per tool version and platform. url/size come from the release list (sync) and stay when the file is
-- cached; sha256 is pinned at the first download or import and a later download with another hash is refused.
-- cached_at is NULL while the zip is not in <data>/tools/<tool>/<version>/<platform>.zip. zip_root is the folder
-- inside the zip that holds UPR-FVX.jar ('' = the zip root).
CREATE TABLE tool_versions (
    tool_id   TEXT NOT NULL,
    version   TEXT NOT NULL,
    platform  TEXT NOT NULL,
    url       TEXT NOT NULL DEFAULT '',
    size      INTEGER NOT NULL DEFAULT 0,
    sha256    TEXT NOT NULL DEFAULT '',
    zip_root  TEXT NOT NULL DEFAULT '',
    cached_at INTEGER,
    origin    TEXT NOT NULL CHECK (origin IN ('github','import')),
    added_at  INTEGER NOT NULL,
    PRIMARY KEY (tool_id, version, platform)
);

-- Per tool: the admin's offered switch (default off, the admin decides) and the state of the release source.
CREATE TABLE tool_settings (
    tool_id        TEXT PRIMARY KEY,
    offered        INTEGER NOT NULL DEFAULT 0,
    latest_version TEXT NOT NULL DEFAULT '',
    last_check     INTEGER,
    last_success   INTEGER,
    last_error     TEXT NOT NULL DEFAULT ''
);

-- A variant of a library game. slot is "var-" plus the first 8 hex characters of the id. No foreign key to games on
-- purpose: the service removes the rows with the game. deleted_at marks "saves kept": the recipe stays so the
-- variant can be built again.
CREATE TABLE game_variants (
    id             TEXT PRIMARY KEY,
    user_id        TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    game_id        TEXT NOT NULL,
    name           TEXT NOT NULL,
    slot           TEXT NOT NULL,
    tool_id        TEXT NOT NULL,
    tool_version   TEXT NOT NULL,
    settings       TEXT NOT NULL,
    seed           INTEGER NOT NULL,
    base_sha256    TEXT NOT NULL DEFAULT '',
    title_id       TEXT NOT NULL DEFAULT '',
    update_version TEXT NOT NULL DEFAULT '',
    spoiler_log    INTEGER NOT NULL DEFAULT 0,
    run_id         TEXT,
    created_at     INTEGER NOT NULL,
    deleted_at     INTEGER
);
CREATE UNIQUE INDEX game_variants_name ON game_variants(user_id, game_id, name COLLATE NOCASE) WHERE deleted_at IS NULL;
CREATE UNIQUE INDEX game_variants_slot ON game_variants(user_id, game_id, slot);
CREATE INDEX game_variants_tool ON game_variants(tool_id, tool_version) WHERE deleted_at IS NULL;

-- The randomizer recipe of a run (a row only for mode 'each' or 'same'; no row = off). run_seed is never delivered.
-- base_sha256 pins the creator's game (Gen 1 to 5), title_id the 3DS title.
CREATE TABLE run_randomizer (
    run_id       TEXT PRIMARY KEY REFERENCES runs(id) ON DELETE CASCADE,
    mode         TEXT NOT NULL CHECK (mode IN ('each','same')),
    preset_id    TEXT NOT NULL DEFAULT '',
    settings     TEXT NOT NULL,
    tool_id      TEXT NOT NULL,
    tool_version TEXT NOT NULL,
    allow_own    INTEGER NOT NULL DEFAULT 0,
    run_seed     INTEGER NOT NULL,
    base_sha256  TEXT NOT NULL DEFAULT '',
    title_id     TEXT NOT NULL DEFAULT ''
);
CREATE INDEX run_randomizer_tool ON run_randomizer(tool_id, tool_version);

-- Which variant a participant plays in a randomizer run (NULL in other runs). variant_id is private to the owner.
ALTER TABLE run_participants ADD COLUMN variant_kind TEXT CHECK (variant_kind IN ('run','own'));
ALTER TABLE run_participants ADD COLUMN variant_id TEXT;
ALTER TABLE run_participants ADD COLUMN variant_state TEXT CHECK (variant_state IN ('not_built','building','ready'));
