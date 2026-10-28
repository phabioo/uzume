Plugins are small sandboxed programs (Lua) that add game-specific features to the Uzume app, for example SoulLink runs, cheats or a split timer. They run inside the app, have no sidebar entry of their own and show their UI in the in-game panel, the HUD, the run tab or the game page. This page is for users; to write a plugin, see the [Plugin API](https://github.com/phabioo/uzume/blob/main/docs/guides/plugin-api.md).

## Install a plugin

- The server offers plugins from the official signed plugin index by default (an admin can turn this off for offline servers, upload a plugin file or import a folder). Apps install plugins from the server, or from a file. See Settings › Plugins in the app and Server › Plugins for admins.
- Plugins signed with the Uzume key show as Official; others show as Unverified or Local.
- Every plugin lists the permissions it needs. You are asked for consent before a plugin gets them, and an invite to a run asks for consent to the run's plugins.

## Plugins in a run

A run plays with the exact plugin versions it was started with, so everyone has the same rules. The person who started the run, or an admin, can move it to a newer version while the run is not over:

- Open the run: "Update available: <plugin> <old> → <new>" with "Update" appears when your server offers a newer version. Plugins it needs are updated with it.
- Your run data, boards and log stay. If a plugin declares that the new version cannot take over a run that old, the update is refused with a message.
- "Restore <old>" goes back to the version before the last update, once.
- Everyone's plugins restart once after an update; the log shows who updated what. If the new version asks for permissions the old one did not, you are asked again before "Play run".
- Older servers do not offer this.

## Official plugins

Each lives in its own repository.

- [SoulLink & Nuzlocke](https://github.com/phabioo/uzume-plugin-soullink): Nuzlocke alone or SoulLink with 2 to 4 people: detects routes, encounters, catches and deaths, applies the run rules, shows teams, routes and a region map.
- [SoulLink Core](https://github.com/phabioo/uzume-plugin-soullink-core): detects the Pokémon game and reads team, boxes, location, battles and badges from the running game; the shared base for the other Pokémon plugins.
- [Cheats](https://github.com/phabioo/uzume-plugin-cheats): cheat lists per game, added by hand, imported or taken from the server's cheat database.
- [Creature Editor](https://github.com/phabioo/uzume-plugin-monster-editor): edits team and box creatures and money in a running game (with your consent and a save copy first).
- [Randomizer](https://github.com/phabioo/uzume-plugin-randomizer): presets for randomized games and runs where each person plays their own randomized world.
- [Splits](https://github.com/phabioo/uzume-plugin-splits): split timer with game time and real time, personal bests and autosplit rules.
- [Randomizer Tracker](https://github.com/phabioo/uzume-plugin-tracker): spoiler-free notes of what you have seen in a randomized run.

Plugins built on SoulLink Core need it installed as well.

## Writing plugins

Plugin developers start with the [template](https://github.com/phabioo/uzume-plugin-template) and the [Plugin API](https://github.com/phabioo/uzume/blob/main/docs/guides/plugin-api.md).
