# ADR 0035: Error groups and GitHub issues

- Status: Accepted (implemented; GitHub lookup against api.github.com and Windows untested)
- Date: 2026-10-28
- Deciders: Fabio (variant "grouped through Server › Reports", 2026-10-28), Claude (details)

## Context

Problem reports ([ADR 0029](0029-operations-and-safety.md) D3) go only to the user's own server and are written by hand. Errors that happen without anybody reporting them (a crash, a core that fails to load, a plugin error, a failed save sync) are lost in logs, and the maintainers learn about them only when a user files a GitHub issue by hand. Fabio asked for errors to become GitHub issues automatically. Constraints: no GitHub token may ship in the app or server; no private instance data (host names, IPs, invite codes, personal paths) may reach a public issue; no browser components in the code (decision of 2026-10-22); reports never go to a third party without a person deciding it.

## Decisions

### D1 Detect and group on the own server

- The app and the server record **error events** of known kinds: app crash (detected on the next start through an unclean-exit marker), core load failure or crash, plugin error (Lua error, quota or watchdog stop), save sync failure, update failure, server panic or error of a request handler.
- Each event carries a **fingerprint**: SHA-256 of kind, component and a normalized message (numbers, paths, addresses and ids replaced by placeholders), shortened to 12 hex digits and shown as `uz-<12 hex>`. The version is not part of it, so one problem keeps its group across versions.
- The app sends error events to the server it is connected to, through the reports API with a new kind `error` (handshake feature `reports_v2`), only when the person turned on "Send errors to the server automatically" (asked once per app, off until answered). The server records its own events directly.
- The server groups events by fingerprint: Server › Reports gets an "Errors" view with one row per group (title, count, first and last seen, versions, devices). Limits: at most 20 error events per device and hour, at most 200 events kept per group (oldest dropped), groups kept until an admin deletes them.

### D2 Report a group on GitHub with one click

- An admin opens a group and chooses "Report on GitHub". The app first looks up the fingerprint in the public issues of the target repository (unauthenticated GitHub search API; on failure it skips the lookup). If an issue exists it offers "Open issue" instead.
- Otherwise the app opens the system browser (`QDesktopServices::openUrl`, https only) with a prefilled `issues/new?template=bug_report.yml` URL: area, app and server versions, fingerprint, title, a short public summary and the OS. The admin reviews it and submits it with their own GitHub account. Logs are never added to the URL; the admin can attach a bundle by hand after checking it.
- The group remembers the issue URL once the admin pastes or opens it, and shows it in the row.

### D3 Public redaction

- Text for a public issue passes a stricter redaction than the diagnostics bundle (which keeps hosts and IPs on purpose): host names, IP addresses, ports, URLs other than github.com, invite and pairing codes, user and display names, home and user paths (`C:\Users\<name>`, `/home/<name>`), game titles and ROM file names are replaced by placeholders. A preview shows the exact text before the browser opens.

### D4 Target repository

- Uzume app and server errors go to `phabioo/uzume`.
- Plugin errors go to the plugin's repository from a new optional manifest field `issues_url` (https only); without it the plugin group has no GitHub button.
- Core errors of the patched Azahar core go only to `phabioo/azahar`, never upstream (Azahar does not accept AI-driven issues). Other cores get no GitHub button.

### D5 Issue templates

- `.github/ISSUE_TEMPLATE/` in this repository follows Azahar's set (bug report, feature request, developers-only blank issue, `config.yml` without blank issues) with Uzume fields: area, app and server versions, fingerprint, system, logs with a redaction note and a required privacy checkbox. The plugin template repository gets the same set so every plugin repository can copy it.

## Rejected

- **Fully automatic issues through a relay** (a hosted service with a GitHub App token): needs hosting and abuse protection, and receives data of every instance without a person looking at it.
- **Prefilled issue link straight from each app**, without the server: no grouping and no deduplication, every member of a server would file the same issue.
- **A GitHub token in the app or server**: a shipped token is public.

## Consequences

- ADR 0029 D3 "never to a third party" stays true for automatic data: only an admin decision sends text, and only to GitHub, through their own browser and account.
- New: error events in the reports API (`reports_v2`), the "Errors" view, the public redaction, the manifest field `issues_url`, the unclean-exit marker in the app.
- Open: a crash handler with minidumps (none today) would give better crash fingerprints; not part of this decision.

## Implementation notes

Where the implementation refines or differs from the decisions above:

- Fingerprint: normalization lowercases ASCII only and applies its regexes literally in a fixed order, so a time such as 21:04:55 becomes `<ip>`. Shared vectors in `protocol/fixtures/error-fingerprint.json` are tested by the server (`errfp`) and the app (`errorfingerprint`).
- API: 400 answers use `bad_request`; an invalid fingerprint in a path answers 404. PATCH takes `{"issue_url": string|null}` (null forgets the link) or `{"seen": true}`; an empty body is 400. Events over the per-device limit are dropped and the 202 answer carries `dropped`.
- Redaction (`redact.Public`) replaces host names with two or more labels (github.com URLs and tokens that end in a common file extension stay), IPs, other URLs, codes, home paths, known user and device names and game titles.
- The server records recovered handler panics, 5xx internal errors, failed backups and a failed self-update (device "Uzume server"), at most 20 per hour. Groups are not capped; each keeps 200 events.
- App: an unclean exit (`<data>/running.marker`) becomes `app_crash` with the redacted last ERROR or FATAL line of `logs/player.1.log`. Further events: core load failure or crash, plugin paused by error, watchdog or quota, first failed save upload of a slot, failed self-update (not a plain failed update check). The queue is `<data>/errors-pending.json` (100 at most, the same fingerprint within 10 minutes merged), sent in batches of 20. A one-time banner asks; "Send automatically" and "Not now" are both answers.
- Server › Reports has a "Reports | Errors" switch (only with `reports_v2`). "Report on GitHub" does the unauthenticated search first, stores a hit and offers "Open issue #N"; otherwise it previews the public text and then opens the URL. It is hidden when the group has no target repository. The Reports badge counts new reports plus unseen groups.
- The manifest field `issues_url` is `https://github.com/<owner>/<repo>`; an invalid value is a warning and ignored. Issue templates are in `.github/ISSUE_TEMPLATE` and in `uzume-plugin-template`.
- Untested: the lookup against the real api.github.com and Windows.
