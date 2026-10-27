# Update index (schema 1)

At a glance: Schema 1 of the signed release feed (`updates-index.json`) that the server and app updaters read. Use of it: [updates.md](../guides/updates.md).

Release feed of the updater ([ADR 0011](../adr/0011-automatic-updates.md)). No change to `protocol_version` (stays 1) or OpenAPI. Release `updates-index` (titled "Uzume update index") holds `updates-index.json` and `updates-index.json.sig` (default URL `https://github.com/phabioo/uzume/releases/download/updates-index/updates-index.json`; signature URL = index URL + `.sig`). Signature as for the core index: Ed25519 over the exact bytes, one line `ed25519 <key_id> <base64 sig>`, same release key.

Schema 1:

```json
{
  "schema": 1,
  "generated_at": "2026-10-07T10:00:00Z",
  "releases": [
    {
      "product": "hub",
      "channel": "beta",
      "version": "0.8.3",
      "commit": "<40 hex>",
      "published_at": "2026-10-07T10:00:00Z",
      "notes_url": "https://github.com/phabioo/uzume/releases/tag/v0.8.3",
      "protocol_version": 1,
      "min_protocol_version": 1,
      "artifacts": [
        {"platform": "linux-arm64", "kind": "deb", "name": "uzume-server_0.8.3_arm64.deb",
         "size": 123, "sha256": "<64 lowercase hex>", "url": "https://github.com/.../uzume-server_0.8.3_arm64.deb"}
      ]
    }
  ]
}
```

- `product`: `hub` or `player`. `channel`: `stable` or `beta`. `notes_url` is optional. `notes` is optional too: a short plain-text "What's new" (groups like `### Added`, bullets `- line`), at most 4000 bytes UTF-8, valid UTF-8, no control characters except newline and tab; signed with the index. `release-add` and the server reject an invalid value, the app drops the field and keeps the release.
- Platforms and kinds: hub `linux-amd64`, `linux-arm64` with `deb` and `binary`; hub `windows-amd64` with `msi` and `binary`; player `windows-x64` with `msi` and `zip`. Server `msi` is applied by `msiexec` through the Windows updater service with `ALLUSERS=1`. App `msi` is what the app installs (launcher `--apply-msi-update`); the former `installer` kind (Inno shell) is no longer produced, and an app ignores artifacts of unknown kinds ([ADR 0021](../adr/0021-one-windows-installer.md), [updates.md](../guides/updates.md)).
- Artifact URLs must be https (`file://` only when the index itself was loaded from `file://`, for tests). Integrity comes from size and SHA-256 in the signed index.
- `schema_version` (server releases, optional): the database schema the server of this release uses, from `uzume-server version --json`. Update rollback needs it to tell whether the data can be kept; an entry without it is "unknown" and not rolled back to.
- `msi_downgrade` (optional, `true` only): the MSI of this release was built with `AllowDowngrades` and can be installed over a newer version. Update rollback offers an MSI only then. Both fields are copied from the `version --json` / `--version-json` output of the build by `release.yml`.
- Unknown fields are ignored, an unknown schema is an error, invalid releases are skipped and reported, a duplicate (product, channel, version) rejects the whole index.
- Versions are SemVer 2.0; a consumer picks the highest release of its product, channel, platform and kind that is strictly newer than the running version and protocol-compatible. Maintained with `uzume-sign release-add` (newest 5 per product and channel).
- Forward compatibility: since 0.10.x consumers drop artifacts with a platform unknown for the product or a kind not allowed on it instead of rejecting the release (a release without remaining artifacts is skipped). `uzume-sign release-add` stays strict for the new release.
- Legacy companion: servers <= 0.8.0 reject a whole release that has an unknown platform (`windows-amd64` since 0.8.1) and so saw no update. `release-add` therefore adds per hub channel, for the newest regular release only, an entry `X.Y.Z-legacy` with just the `linux-amd64`/`linux-arm64` `deb`/`binary` artifacts (skipped when the base version has a prerelease, when all artifacts are legacy already, or without a legacy `deb`). It is a prerelease, not build metadata, because the app's parser keys duplicates without build metadata. It sorts just below `X.Y.Z`: old servers install it and then report `X.Y.Z`; newer servers prefer `X.Y.Z`; Windows servers never select it. Companions are regenerated on every `release-add` and not counted by `-keep`.
