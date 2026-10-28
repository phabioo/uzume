-- Run plugin updates (uzume issue #335, feature run_plugin_updates_v1): a run's pinned plugin version can be switched
-- by the creator or an admin. The version pinned before the last switch is kept (restorable, and protected from
-- pruning while the run is unfinished). NULL = never switched.
ALTER TABLE run_plugins ADD COLUMN previous_version TEXT;
ALTER TABLE run_plugins ADD COLUMN previous_sha256 TEXT;
