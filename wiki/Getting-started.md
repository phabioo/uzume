# Try the main features

At a glance: a short tour for a server with two apps, using only homebrew ROMs or dummy files. All server steps are done in the first app as admin (sidebar item "Server"); the server has no web pages.

1. In the first app, Server › People › Invites: create an invite (the code is shown only once). In a second app choose "I have an invite code" and enter it.
2. Start a Session on the first app with Private and with server users; check that the second user is rejected, then let in ([sessions-over-the-internet.md](Sessions-over-the-internet)).
3. In Server › Games, enable "Members can add games"; upload a homebrew ROM from the second app.
4. In Server › Systems & cores, switch NDS to native firmware mode without files; the app shows "Firmware required" / "Firmware missing".
5. Upload your own BIOS/firmware dumps (`bios7.bin`, `bios9.bin`, `firmware.bin`; never in the repo) and launch.
6. Connect a gamepad and remap it on Controllers.
7. Install the app via the `uzume-windows-x64-msi` artifact.

Keyboard defaults: see [app.md](Using-the-app).
