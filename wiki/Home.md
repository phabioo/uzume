Uzume is a self-hosted retro gaming platform. The Uzume server keeps your ROM library, versioned saves, users and devices in one place; the Uzume app emulates games locally and shares running Sessions directly with other apps over WebRTC. You administer the server in the app. Systems: DS, 3DS, Game Boy, Game Boy Color, Game Boy Advance; firmware is yours, Uzume never ships it. Platforms: Windows app; Linux server (Debian, Raspberry Pi), also Windows and Docker.

These pages are the user guides. Developer documentation, architecture and decisions are in the [repository docs](https://github.com/phabioo/uzume/blob/main/docs/README.md); the project is at [https://github.com/phabioo/uzume](https://github.com/phabioo/uzume).

## Start

- [Getting started](Getting-started): a short tour for a server with two apps, using only homebrew ROMs.
- [Local ROMs](Local-ROMs): play ROMs from your own folders without uploading them.

## The app

- [Using the app](Using-the-app): install, pairing, playing, data directory, command line, environment, reporting problems.
- [Saves](Saves): sync, conflicts, history, restore, retention.
- [Sessions over the internet](Sessions-over-the-internet): share a running game on a LAN or over the internet.
- [Updates](Updates): channels, automatic updates, rollback.

## Running a server

- [Server installation](Server-installation): Debian package or systemd script on Linux.
- [Server in Docker](Server-in-Docker): official image, volumes, compose, upgrade.
- [Server configuration](Server-configuration): commands, flags, environment variables, certificates.
- [Server backup](Server-backup): one archive, schedule, restore on a new machine.
- [coturn on a VPS](coturn-on-a-VPS): your own TURN relay for Sessions outside your LAN.

## Plugins

- [Plugins](Plugins): what plugins do, how to install them and the official plugins.
