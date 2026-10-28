# ADR 0036: Issues, milestones, wiki and release numbering

- Status: Accepted
- Date: 2026-10-28
- Deciders: Fabio (2026-10-28), Claude (details)

## Context

The roadmap had grown into a work list: leftovers and test lists per milestone, a checklist per planned milestone and the plan of the test phase. It was long, hard to keep current and not visible in the repositories where the work happens. The numbering 0.x with a future "1.0" no longer fits parallel work packages, and user guides lived only in `docs/` next to developer docs.

## Decisions

### D1 Issues and milestones

- Open work is tracked as GitHub issues in milestones, in `phabioo/uzume` and all official plugin repositories. A milestone is a release and has the same title in every repository: "Uzume 21" (first stable of the new numbering: Fabio's local tests and the release steps) and "Uzume 22" (next line: leftovers and new work packages).
- Themes are labels: `emulation`, `randomizer`, `plugins`, `together`, `platforms`, `app`, `install-and-sharing`. Process labels: `work-package` (one thread, one PR), `leftover` (left open when finished in code), `local-test` (needs Fabio's real hardware, games or networks), `triage` (not yet sorted), `release`.
- One thread works one issue; the PR body says "Closes #N". Findings become issues in the right milestone (`triage` when unsure). Error reports filed through "Report on GitHub" ([ADR 0035](0035-error-groups-and-github-issues.md)) arrive without milestone and get triaged.
- Cross-repository view: issues search `is:open user:phabioo milestone:"Uzume 21"`.

### D2 Roadmap scope

`docs/roadmap.md` keeps only direction: overview, the internal goal, the table of finished milestones, After/Later/Out of scope and open decisions. Open work and tests are not listed there but live in the milestones. `docs/history.md` and `CHANGELOG.md` stay the record of finished work. The internal goal (a group of friends completes a SoulLink run in Omega Ruby/Alpha Sapphire over Uzume) is neither a milestone nor a version; "1.0" is dropped as a version.

### D3 Wiki

User guides live in `wiki/` in the repository and are mirrored to the GitHub wiki by the workflow "Publish wiki". Edits go through PRs (review, link check, changelog check); a cloud session cannot push to the wiki repository directly. Developer docs stay in `docs/`. There is one central wiki in `phabioo/uzume`; plugin repositories have no own wiki (their user page is the wiki page "Plugins"; developer docs stay in each plugin repository).

### D4 Release numbering (replaces ADR 0016 D1/D2 numbering)

- Chrome-like `R.B`: R is the release line ("Uzume 21"), B is a build number counting up with every push to `main`; a build becomes a published beta only on demand (ADR 0016 amendment 2026-10-28).
- The canonical string is `R.B.0` in tags (`vR.B.0`), files, index and MSI; people see `R.B`.
- A stable release is a promoted build, without rebuild (as in ADR 0016). A hotfix is a later build of the same line, promoted.
- `VERSION` holds the line (`21.0.0`); a release PR starts the next line by raising it to `22.0.0`.
- Line 21 continues after the 0.20.N betas (every 21.x sorts after 0.20.x). "Uzume 21" replaces the planned stable 0.21.0 (ADR 0016 amendment 2026-10-27).
- MSI limit: B <= 255 per line; CI fails beyond that and asks for a new line.
- The implementation (CI, `VERSION`, update index) follows in its own PR.

## Rejected

- Firefox-like `21.0` with `b`-suffix betas: the stable would be a rebuilt, untested binary and the whole pipeline would run again at release.
- Calendar numbers like `2026.11`: the MSI major version is limited to 255, which needs a mapping or another installer.
- SemVer `0.x` with `-beta.N` suffixes.
- Keeping open work in the roadmap: long, not visible where the work happens.

## Consequences

- Roadmap, history and workflow docs are updated with this ADR; the issues exist in the milestones.
- ADR 0016 D1/D2 are superseded for numbering; promotion without rebuild stays.
- Version strings in docs written before this ADR (0.20.N, 0.21.0) stay as history.
