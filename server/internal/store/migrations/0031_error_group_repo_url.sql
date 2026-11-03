-- 0.24 Plugin error events may carry the plugin's manifest issues_url (ADR 0035 D4). A development plugin is not in the
-- plugins table, so the group keeps the repository the app saw; the installed manifest still wins.
ALTER TABLE error_groups ADD COLUMN repo_url TEXT NOT NULL DEFAULT '';
