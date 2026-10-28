# Releasing

At a glance: How a maintainer promotes a beta to a stable release: the workflow "Promote to release", what it changes in the signed index and how the release notes and `CHANGELOG.md` are produced. What users and admins see (channels, automatic updates, rollback): [Updates](https://github.com/phabioo/uzume/wiki/Updates). Decisions: [ADR 0016](../adr/0016-release-numbering-and-promotion.md), [ADR 0028](../adr/0028-release-notes-and-changelogs.md).

## Promote a beta to a release

GitHub Actions > "Promote to release" > Run workflow (`.github/workflows/promote.yml`). Optional inputs: `version` (default: newest beta prerelease) and `highlights` (free Markdown text placed at the top of the release notes).

The workflow does not rebuild and creates no tag or `VERSION` PR. It adds the beta's artifacts to the signed `updates-index` under channel `stable` (from the `index-hub.json` / `index-player.json` assets on the beta release) and turns the GitHub release from prerelease into the latest stable release. Stable numbers therefore skip (0.7.1, 0.8.3, 0.8.6). Betas built as `vX.Y.Z-beta.N` cannot be promoted; `v0.7.1` is the last stable made the old way. Promoted releases are never pruned.

Release notes ([ADR 0028](../adr/0028-release-notes-and-changelogs.md)): the stable release text is "Changes since <previous stable>", built from the `## Changelog` sections of all PRs between the previous stable release and the promoted beta (all betas in between, deduplicated), below the optional highlights. The same range, filtered per product, replaces the `notes` field of the stable index entries. The workflow also opens the PR "docs: changelog for X.Y.Z" (branch `changelog/vX.Y.Z`, auto-merge) that adds the section to `CHANGELOG.md`; it dispatches CI and the Changelog check on that branch because `GITHUB_TOKEN` events start no workflows. Merging that PR builds one more beta with the text "No user-visible changes." If the repository setting "Allow GitHub Actions to create and approve pull requests" is off, the PR is not created, the run summary shows a warning and the section is added to `CHANGELOG.md` by hand. A failure there never undoes the promotion.
