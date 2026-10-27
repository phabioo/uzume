# Publishing an official plugin

At a glance: How maintainers build, sign and publish an official `.uzplugin` and add it to the signed plugin index. Needs the Uzume release signing key. Installing plugins: [plugin-api.md](plugin-api.md#installing-plugins-and-origins).

For plugin maintainers with access to the Uzume release signing key. Background: [12-plugins.md](../architecture/12-plugins.md#1610-distribution), [ADR 0023](../adr/0023-plugins.md). Server side: [server-configuration.md](server-configuration.md#plugins).

An official plugin is a `.uzplugin` file (zip with `manifest.json` at the root) that carries a `SIGNATURE.json` signed with the Uzume release key. A plugin index lists signed files so that a server admin can install or update them with one click. The tool is `server/cmd/uzume-sign`. The signing key is read from the environment variable `UZUME_SIGNING_KEY` (base64 seed); never write it to a file in a repository or to logs. Plugin repositories keep it as a CI secret. The signature prefix is `uzume-plugin-v1`: plugins and the plugin index signed before the rename (FrameBeam) do not verify and must be signed again with "Publish plugin".

1. **Build the zip.** Zip the plugin folder (manifest, entry file, assets, i18n) as `x.uzplugin`. Limits: 32 MiB packed, 64 MiB unpacked, 2000 entries, relative `/` paths only, no symlinks.
2. **Sign it.**
   ```
   uzume-sign plugin-sign -in x.uzplugin -out x-signed.uzplugin
   ```
   Validates the file and adds or replaces `SIGNATURE.json` (SHA-256 of every file plus an Ed25519 signature). `-out` may equal `-in`.
3. **Add it to the index.**
   ```
   uzume-sign plugin-add -index plugin-index.json -plugin x-signed.uzplugin -url https://example.com/releases/x-signed.uzplugin
   ```
   `-url` is where the signed file will be published (https). The command refuses unsigned or invalid files, creates the index when missing, and adds the entry for the manifest's id and version; other entries stay. A published version is immutable (run pins and server copies are keyed by it): when the index already has this id and version with a different SHA-256 the command fails; the same file again only refreshes the entry. `-replace` overrides this with a warning and is only for fixing a broken release before any server has offered it. It records name, version, `api`, description, permissions, SHA-256 and size from the file.
4. **Sign the index.**
   ```
   uzume-sign sign -index plugin-index.json
   ```
   Writes `plugin-index.json.sig` next to the index (same scheme as the updates index).
5. **Publish** the signed `.uzplugin` at the URL from step 3, and `plugin-index.json` plus `plugin-index.json.sig` side by side at one location.

For a self-hosted index, a server admin sets `-plugin-index-url` (env `UZUME_PLUGIN_INDEX_URL`, https or `file://`, for example `https://hub.example.com/plugin-index.json`) and installs the plugin from the Plugins page. Without a setting every server uses the official index (see below); the value `none` turns the index off for offline servers, and signed or unsigned files can still be uploaded or imported with `uzume-server import-plugins <dir>`.

Checks: the server accepts the signature only for the release key or a key given with `-core-trust-key`; a file that fails validation or whose SHA-256 differs from the index is not installed.

## Official index

Servers read the official index by default: `https://github.com/phabioo/uzume/releases/download/plugin-index/plugin-index.json` (+ `.sig`), the release `plugin-index`, titled "Uzume plugin index", of the Uzume repository (a prerelease, never the latest Uzume release). It is published by the manual workflow "Publish plugin" (`.github/workflows/plugin-index.yml`, environment `release`, secret `FRAMEBEAM_SIGNING_KEY`):

1. Release the plugin in its own repository (`phabioo/uzume-plugin-<name>`) with the `.uzplugin` file as a release asset.
2. Run "Publish plugin" with `repo` (for example `phabioo/uzume-plugin-soullink-core`) and `tag` (the release tag). The workflow downloads the `*.uzplugin` assets (legacy `*.fbplugin` assets are accepted and published as `.uzplugin`) (secret `PLUGIN_REPOS_TOKEN`, a read-only PAT, when set, otherwise the workflow token; needed for private plugin repos), signs each with the release key, uploads it as `<plugin-id>-<version>.uzplugin` to the `plugin-index` release (versions never overwrite each other), adds it with `plugin-add`, then signs and uploads `plugin-index.json` and `plugin-index.json.sig`.
   The workflow must not overwrite an asset that is already published for this id and version (no `gh release upload --clobber` for an existing asset) and runs `plugin-add` without `-replace`; a re-run for an already published tag with a different file must fail. Release a new version instead.
3. The optional input `remove_ids` (space- or comma-separated plugin ids) removes those ids from the index and, after the new index is signed and uploaded, deletes their `<id>-<version>.uzplugin` assets from the release; it works alone or together with `repo`/`tag` (the same id cannot be added and removed in one run).
4. Without inputs the workflow only re-signs and publishes the existing index (an empty one when none exists yet).

Servers pick up the change at their next check (every 24 h) or with "Check now".
