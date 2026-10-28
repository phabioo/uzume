# Security policy

At a glance: report vulnerabilities privately through GitHub ([Report a vulnerability](https://github.com/phabioo/uzume/security/advisories/new)), never in a public issue. Only the newest build of the current release line gets fixes. Expect a first answer within 7 days.

## Supported versions

Uzume versions are `R.B` (release line and build number, [ADR 0036](docs/adr/0036-issues-milestones-wiki-and-numbering.md)). A fix ships as a new build of the current line, so update to get it.

| Version | Supported |
| --- | --- |
| Newest stable and newest beta of the current line (Uzume 21.N) | Yes |
| Older builds of the current line | No, update to the newest build |
| Earlier lines, 0.x betas and FrameBeam releases | No |

Official plugins are supported in their newest release on the official plugin index.

## Reporting a vulnerability

Use GitHub private vulnerability reporting: open [Security → Report a vulnerability](https://github.com/phabioo/uzume/security/advisories/new) in this repository. Only the maintainers see the report.

Please do not report security problems in public issues, discussions, pull requests or through "Report on GitHub" in Server › Reports: those issues are public. If an error group in your server looks like a security problem, report it privately instead.

A useful report names the affected component and version (`R.B`, plugin id and version), the steps to reproduce, the impact you expect and whether it needs a paired device, an account on the server or local access. Never attach ROMs, BIOS, firmware, keys, saves or private data of your instance (host names, IP addresses, invite or pairing codes); use placeholders such as `hub.example.com`.

## What to expect

Uzume is maintained by a small team in spare time; these are targets, not guarantees.

- First answer within 7 days.
- An assessment (accepted or not, severity) within 14 days.
- A fix in a new build within 90 days for accepted reports, sooner for severe ones. We keep you posted in the advisory.
- Disclosure is coordinated: the advisory is published once a fixed build is available. Credit is given in the advisory unless you prefer not to be named.

## Scope

In scope:

- The Uzume app and the Uzume server, including pairing, TLS, accounts and permissions, the REST and WebSocket API, Sessions (WebRTC, the embedded TURN relay) and the debug MCP server of the app.
- Installers and packages: Windows MSI, Linux `.deb` and AppImage, the Docker image, and the self-update path (update index, signatures, root helper).
- Plugins: the Lua sandbox and its permissions, plugin signatures and the signed plugin index, and the official plugins (`phabioo/uzume-plugin-*`). Report plugin problems here as well.
- Saves and backups: anything that can overwrite, leak or send saves to another server.

Out of scope, please report elsewhere:

- The patched Azahar core: report privately in [phabioo/azahar](https://github.com/phabioo/azahar/security/advisories/new), never to the upstream Azahar project.
- Other emulator cores (downloaded from the libretro buildbot) and third-party libraries: report to their projects. If Uzume ships a vulnerable version, a report here is welcome.
- Third-party plugins: report to their authors, unless the plugin escapes the sandbox or its permissions (that is in scope).
- Problems that need an already compromised device or administrator account, or that come from a deliberately unsafe configuration of your own server.
