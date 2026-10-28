At a glance: Play ROMs from your own folders without uploading them: add folders in Settings › Library folders, the app scans and registers them. Why: [ADR 0025](https://github.com/phabioo/uzume/blob/main/docs/adr/0025-local-roms.md).

Since 0.12 you can play ROMs from your own folders without uploading them to the Uzume server. Use only homebrew or ROMs you dumped yourself.

## Library folders

1. In the app open Settings → Library folders and add one or more folders. Use rescan after you copy new files.
2. The app scans for the file types of the server's systems, reads the game code from the header (NDS, 3DS) and registers each file with the server. A large file is hashed once; the result is remembered by path, size and modification time.
3. Games appear in the Library. A game that is not on the server shows **Local**; a game whose file is not on this device shows **Missing**.
4. Start the game as usual. A local file is started straight from the folder (no copy into the ROM cache). Saves sync with the server like for any game.

If the file moved or was deleted, the game shows "File missing on this device" until you add the folder again or the game is on the server.

## Upload to the server

- In the game detail, "Upload to Hub" sends the file in chunks. It resumes after a lost connection or an app restart. Once a file is stored on the server, the game is visible to everyone and others can download it.
- "Upload ROM" in the Library header uses the same resumable upload when the server supports it.
- The upload needs the permission to upload and is limited to 4 GiB per file. Unfinished uploads are removed after 7 days.

## What others see

- A game that is only local is visible to you (and to other users who registered the same file or a file of the same game) and to server admins, who see it as "Local only" in the app's library.
- Files are never sent between apps. Another user needs their own copy of the file, or you upload it to the server.
- Different dumps of the same game (same system, game code and version) become one game with several files, so saves are shared. Different versions or regions stay separate games.
- Files without a readable header (for example `.zcci`) match by hash only: another dump of the same game is a separate game.

## Remove a registration

For a local-only game the game detail has "Remove from library". It drops your registrations on the server (`DELETE /api/v1/games/{id}/registration`); the game and its saves stay. The game then disappears for you unless a file of it is stored on the server.
