# ADR 0016: Release numbering and promotion

> Amendment 2026-10-28: the numbering of D1/D2 (plain `X.Y.Z` per beta, `VERSION` as floor) is replaced by `R.B` builds in [ADR 0036](0036-issues-milestones-wiki-and-numbering.md) D4. Promotion without rebuild stays. Amendment 2026-10-28 (bottom): a beta is published only on demand, not for every build.


- Status: Accepted (Fabio, 2026-10-08)
- Date: 2026-10-08
- Deciders: Fabio

## Context

[ADR 0011](0011-automatic-updates.md) D1/D3 numbered betas `X.Y.Z-beta.<run>` and made a stable release a rebuild from a `vX.Y.Z` tag, plus a PR that bumped `VERSION`. In practice the numbers drifted: promoting 0.7.1 proposed 0.8.0 as the next version although no 0.8 work had started, `VERSION` bumps depended on a PR that had to merge, and the run number in the beta suffix told users nothing. One number per build, and a promotion that does not build again, is simpler to reason about.

## Decisions

### D1 One plain number per beta

- Every push to `main` (every merged PR) builds a beta with a plain version `X.Y.Z`, published as GitHub prerelease tag `vX.Y.Z` and listed in the update index under channel `beta`. No `-beta.N` suffix any more.
- PR and branch builds stay `X.Y.Z-dev.<run>`, channel `dev`, and are never published.
- Pushing a `v*` tag builds and releases nothing.

### D2 Number assignment

- The root file `VERSION` is the floor for the next beta. CI uses `max(VERSION, highest existing plain tag vX.Y.* of that X.Y line + 1)`. After `v0.7.1` the next merge is 0.7.2, then 0.7.3.
- Only a milestone PR that starts a new minor line raises `VERSION` to `X.(Y+1).0` (for example 0.8.0). Otherwise nobody touches `VERSION`.
- The CI version job reserves the number by creating the tag on the commit before building. A failed or re-run build reuses or leaves its number, so gaps are possible.
- CI runs on `main` no longer cancel each other.

### D3 Promote to release

- `.github/workflows/promote.yml` ("Promote to release", manual). Input `version` is optional; empty means the newest beta prerelease.
- No rebuild, no new number, no new tag, no `VERSION` bump PR. The workflow adds the same artifacts to the signed `updates-index` under channel `stable` (from the `index-hub.json` / `index-player.json` assets stored on each beta release) and flips the GitHub release from prerelease to latest stable.
- Stable numbers therefore skip (0.7.1 -> 0.8.3 -> 0.8.6), as with Chrome.
- Betas built before this change (`vX.Y.Z-beta.N`) cannot be promoted. `v0.7.1` is the last stable made the old way.

### D4 Pruning

- All beta prereleases (including legacy `-beta.N`) are kept so the release notes stay traceable; only retired `-test.N` prereleases are deleted. Promoted releases are never pruned.

### D5 Default update channel

- Release builds of Hub and Player are compiled with channel `beta`. Dev builds stay off.
- With no channel selected, the first successful index load stores a resolved default: `update_channel_default` (Hub: `settings` table; Player: key in `player.json`). It is `stable` if the index lists the running version as stable, else `beta`.
- It is never re-resolved automatically: a beta install stays on beta after its version is promoted, a fresh install of a stable release follows stable. An explicit channel selection always wins.
- Automatic install default: on for beta, off for stable (unchanged).

## Rejected

- Keep `X.Y.Z-beta.N` and only fix the promote bump: the number still changes between beta and stable.
- Rebuild on the release tag: the shipped binary would differ from the tested beta.
- Compile `stable` into promoted builds: needs a rebuild (see above); D5 resolves the channel at runtime instead.

## Consequences

- The tested beta binary is the stable binary; its embedded channel is `beta`, so the channel is resolved at runtime (D5).
- Beta numbers are dense but may have gaps; stable numbers are sparse. Users see plain `X.Y.Z` everywhere.
- Debian versions no longer need `~beta.N` for published builds; `~` stays for `-dev` and legacy pre-release versions.
- Needs `index-hub.json` / `index-player.json` assets on every beta release.

## Supersedes in ADR 0011

- D1: the `X.Y.Z-beta.<run>` beta version, the tag-to-stable rule and "tag must equal `VERSION`".
- D3: the tag-triggered stable release and the `v*-beta.*` pruning rule.
- The "milestone PR raises `VERSION`" wording and promote's `VERSION` bump PR.
- The compiled default channel logic of D5 (now D5 here).

## Amendment 2026-10-19 (release pipeline hardening)

Semantics of D1 to D5 are unchanged: one plain number per beta, the tag is reserved by the CI version job before the build, a release is only published after a green CI run on `main`, promotion adds the same files to the `stable` channel. What changes is how the pipeline protects them:

- **Queue instead of a shared concurrency group.** `release.yml`, `promote.yml` and the new `index-refresh.yml` use one concurrency group per run and serialise the index read-modify-write in `scripts/wait-index-turn.sh` (oldest run id first). A shared group keeps only one pending run and would cancel the middle one.
- **Orphan tags.** A tag reserved by a build that never became green is deleted by `release.yml` once its commit is older than 8 days and it has no release (the CI artifacts are gone after 7 days, so it can never be released). Until then the tag stays so that re-running failed jobs keeps the number. Gaps in the numbers remain possible.
- **Signed index, one file.** Each index (updates, plugin) is additionally published as `<index>.json.signed`: the signature line, a newline, the exact index bytes. Current Hubs and Players prefer it (no mismatched pair possible) and fall back to `<index>.json` + `.sig` only while the bundle is absent.
- **Domain separation.** The bundle signature is `ed25519ctx <key_id> <context> <sig>` over `context + "\n" + index`, with a context per document type (`framebeam-updates-index-v2`, `framebeam-plugin-index-v2`, `framebeam-cores-index-v2`). A signature for one type never verifies for another. The legacy `.sig` (raw bytes, same key) keeps being published unchanged for installed Hubs and Players.
- **Expiry.** Updates and plugin indexes carry a signed `expires_at` (`generated_at` + 60 days). Hub and Player reject an expired index; the weekly `index-refresh.yml` and `plugin-index.yml` runs re-sign with new dates. Indexes without `expires_at` are accepted during the transition.
- **Transition.** Both forms are published for at least two minor lines (installed clients keep working on the legacy pair and ignore the new `expires_at` field). A later amendment makes the bundle, the context and `expires_at` mandatory and drops the legacy pair. Until then a client whose bundle download is blocked can be served the legacy pair; this is accepted for the transition.
- **Rejected:** embedding the signature in the index JSON (needs a canonical form to sign), versioned file names (needs a mutable pointer), a signing key per index type (key distribution is the hard part, the context gives the separation).

## Amendment 2026-10-23 (betas are no longer pruned)

Beta prereleases are no longer pruned (previously the newest 5 were kept). The signed updates-index still keeps the newest 5 per product and channel (`uzume-sign release-add`), so older betas stay downloadable from GitHub but are not offered as updates.

## Amendment 2026-10-27 (milestones built in parallel)

On 2026-10-27 the milestones 0.20 Emulation, 0.21 Randomizer, 0.22 Plugins, 0.23 Together and 0.24 Platforms were built at the same time in separate threads and sessions. Only 0.20 package 1 raised `VERSION` (to 0.20.0), so every merge of all five milestones became a beta `0.20.N`: for example the Linux AppImage (0.24) shipped in 0.20.12 and Discord Rich Presence (0.22) in 0.20.15. No stable release had been promoted since 0.18, so nobody installed a number that now means something else. Decided by Fabio on 2026-10-27:

- **Milestone numbers are themes, beta numbers count builds.** Until the batch is released, no package raises `VERSION`; features and fixes of all five milestones keep shipping as `0.20.N`. A package keeps its milestone in the PR title (for example "(0.22 package 3)"), and the release notes stay per PR as before. Sessions outside the project (the 0.23 session) follow the same rule.
- **0.21 is the first stable release of the batch.** When Fabio and the agents are both content with the state, one release PR raises `VERSION` to 0.21.0; its beta `0.21.0` (or a later `0.21.N` with fixes) is promoted with "Promote to release" (D3, no rebuild). Stable 0.21 contains the milestones 0.20 to 0.24.
- **The milestone labels 0.20 to 0.24 stay as they are** in the roadmap, history and ADRs; they name the themes of this batch, not the release that carries them. History records for each milestone the range of betas its packages shipped in.
- **After the release** D2 applies unchanged: a new milestone gets the next free minor line (0.22 and up) when it starts and raises `VERSION` with its first PR.
- **Rejected:** a bump per finished milestone (more `VERSION` conflicts between parallel sessions, and 0.24 was finished before 0.22 and 0.23), and 0.24.0 as the release number (Fabio prefers the next line after the betas).

## Amendment 2026-10-28: beta on demand

D1 said every push to `main` is published as a beta. That changes: every push to `main` still gets its build number `R.B.0` (tag reserved by the CI version job) and its CI artifacts, but a beta prerelease and update index entry are only made when the owner runs the workflow "Release" (`release.yml`, manual, optional input `build`, default the newest green main build that is not published yet). It publishes the CI artifacts without a rebuild; the release notes cover all PRs since the previous released tag, so skipped builds are not lost. Why: fewer betas and less release noise for installs that update automatically. Consequences: gaps in the published numbers are normal; CI artifacts are kept 7 days, so only builds of the last 7 days can be published; tags of builds that are never published are pruned after 8 days. Promotion (D3) is unchanged. Decided by the owner on 2026-10-28.
