# Architecture: identity, pairing, TLS

## 14. Hub connections, identity and pairing [PoC]

### Roles, user creation and identity

FrameBeam has no global account infrastructure, no public registration and no global user ID. The Hub manages **Hub-local user IDs**; the same person can have different identities on different Hubs.

At least **admin** and **user** are provided. The admin manages library, users, clients, systems/cores including firmware, Hub settings and pairings. Normal users use library, their saves and Sessions according to Session permissions. Library management is admin-only by default. The optional Hub setting **"Allow users to upload games"** permits normal users their own ROM uploads into the same central library. Entries receive `uploaded_by` as a Hub-local user ID. This permission grants no general library management and in particular does not allow deleting or managing existing entries of others.

On the first Uzume server start, an admin account with **username + password** is set up (from the app, with a setup code on a headless server; [ADR 0030](../adr/0030-one-app-one-server-uzume.md) D4). The server stores a secure password hash (Argon2id). Further users are created exclusively under admin control, preferably via short-lived invite codes/invite links. The user redeems the invite in the app, chooses a display name and optionally a password; the server creates the local user and can directly authorize the first device in the process. User onboarding links are not Session share links. Invite lifetimes and the redemption endpoint are in the protocol reference.

**Every user can have a password; admins must** (built in 0.18 package 4, handshake feature `accounts_v1`, ADR 0030 D5). A password has at least 12 characters. Admins cannot remove theirs; the admin setting `require_passwords` blocks removing a password for members and forces nothing on existing users. A user without a password still gets onto a new device by approval. User and device are separate entities; a user can own multiple devices. The app generates the device ID and device name locally. This identity as well as the local profile/display name serve UX and do not constitute authentication. The admin assigns a pairing request to an existing local user; a freely entered user ID is not blindly accepted.

- **Sign-in on a new device**: `POST /api/v1/auth/sign-in` with user name and password trusts the device directly and replaces waiting for the admin. Every failure is one generic `invalid_credentials` (unknown names too). Limits: 10 attempts a minute per IP; 15 min lock of a user name after 10 failures.
- **Several users on one PC**: the app sends an `install_id`. Each user on that PC is its own device row with its own device ID and credentials; the rows share the `install_id`. Switching needs no new pairing. The optional PIN for switching is app-side only and never reaches the server.
- **Confirm it's you**: before dangerous steps (backup restore and delete, update rollback, password reset, accounts settings; the admin API adds more) the app asks for the password again (`POST /api/v1/account/confirm`, valid 120, 300 or 900 s per user and device). Without it the server answers 403 `confirmation_required`. 3 wrong passwords lock confirming for 5 min on that device.
- **Own devices**: each user lists their devices, signs one out or all out (`/api/v1/account/devices`); admins see all devices under Clients.
- **Forgot password**: an admin resets it (`password-reset`: temporary password, the user must change it); `reset-admin` on the server for admins. There is no email recovery.

**Setup code and server commands** (built in 0.18 package 6, handshake feature `setup_code_v1`, ADR 0030 D4). A server without a screen is set up from the app on another PC:

- **Setup code**: 6 Crockford base32 characters shown as `7H4K-2Q`, from the system random generator. While the server has no admin it holds one code, valid 60 minutes and usable once. The serving process creates a fresh one at every start (a restart replaces the old code) and prints it in a banner (terminal, journal, `docker logs`, installer last page) with the address and the security code; `uzume-server setup-code` writes a new one. It is stored in the settings table (the database already gives full control of the server) and appears in the log only in that banner.
- **Redemption**: `POST /api/v1/setup` (no auth, any address) trades the code, a user name and a password of at least 12 characters and the device fields for the first admin and a paired device in one step. The code is consumed in the same transaction as the admin, so two parallel requests have exactly one winner. Wrong tries: 10 requests a minute per IP, and 10 wrong tries in total delete the code. Errors: 401 `invalid_setup_code`, 410 `setup_code_expired`, 409 `already_set_up`. `getHubInfo` tells with `setup_required` whether the server still needs setup.
- **Security code**: the first 60 bits of the SHA-256 of the leaf certificate the server serves, as 12 Crockford characters (`K7Q4-9WMP-2HXD`). It is shown next to the setup code and by `uzume-server code`; the app computes it from the certificate it pins and asks the person to compare before it sends the setup code or trusts the server. Exact derivation and test vectors: [protocol reference](../reference/protocol.md#security-code-and-setup-code). It complements the SHA-256 fingerprint (still shown under Details) with something a person can compare.
- **Emergencies**: `reset-admin` sets a one-time password for an admin (the user must change it at the next sign-in, the sign-in lock ends, devices stay signed in, nothing else is touched); `reset` deletes all data after a safety backup; `status`, `update`, `backup` and `restore` complete the set. They run on the server (Linux with `sudo`, Windows as administrator, Docker with `docker exec`): [server-configuration.md](../guides/server-configuration.md#server-commands).

### Hub profiles and device credentials

Multiple Hub profiles can be stored locally. A profile contains Hub ID, display name, address, Hub user ID, local device ID, credential reference, pinned certificate fingerprint and time of the last connection.

Each authorized device has its own **Hub-specific credentials**: a short-lived access token and a long-term, revocable device/refresh credential, bound to Hub, user and device. The long-term secret lives exclusively in the **OS credential store** (Windows Credential Manager; on Linux the freedesktop Secret Service over D-Bus, used when the session bus offers it, otherwise memory only; macOS is not planned), never in profile files or logs. The Hub stores a secure verification representation instead of plaintext. Exact lifetimes and renewal rules remain open. A lost device can be revoked individually; revocation must also withdraw further access with its access tokens.

**Exactly one Hub is active per connected Player instance.** Before a connection or after disconnecting, no Hub can be active. Library, saves, Sessions and core registry come exclusively from the selected Hub; profiles create neither parallel connections nor a merged library. Local Hub data and pending sync stay separated by Hub ID and, where applicable, Hub user ID, even with identical ROM hashes. "Play on this PC" is such a Hub too: the app starts `uzume-server -child` (loopback only, exits with the app, hidden admin whose password lives only in the credential store); "Share with others" moves it to the Windows service and the profile switches to the service's port with the same certificate ([ADR 0030](../adr/0030-one-app-one-server-uzume.md) D6).

### Connection and approval pairing [PoC]

1. On the connection screen, the user enters the Hub address or selects a profile.
2. The Player identifies the Hub via a FrameBeam info endpoint, for example `GET /.well-known/uzume`, with Hub ID, name, Hub version and protocol information; the concrete path remains open. This is address-based identification, not a necessary automatic network discovery. TLS trust is checked according to the following section; the capability handshake happens on connect.
3. Without valid device credentials, the Player issues a **pairing request**. In the app (Server › People › Devices; formerly the Hub web interface, removed in 0.18, [ADR 0030](../adr/0030-one-app-one-server-uzume.md) D2) a **pending request** appears with device name, platform and Player version.
4. The admin assigns the user and clicks **Allow / Deny**. A user with a password can skip this by signing in (see above); a pairing request and an invite carry the optional `install_id`. Only Allow registers and authorizes the device. Requests/attempts are rate-limited; the info endpoint and a pending request grant no rights. A valid admin onboarding invite can directly authorize the first device.
5. After successful approval, the device receives Hub ID, user assignment and its own access/device credentials. Further connections use short-lived access tokens; the device/refresh credential enables their renewal.
6. **Clients** shows **Trusted / Revoked** and **Revoke access**. On revocation or invalid credentials, the Player returns to the required approval.

A short-lived pairing code may be provided as an alternative; **Player request → admin Allow/Deny is the preferred default and the mandatory PoC flow**. A code-only flow does not replace it.

Optionally the Player connects to the last used Hub at startup. If it is unreachable, the connection selection appears with an understandable error and a retry option.

### Transport security and certificate trust [PoC]

In production operation, **HTTPS/WSS is mandatory** for API, signaling, authentication and file transfer. HTTP/WS is allowed exclusively explicitly in dev mode or on localhost.

For simple self-hosted/LAN use, the Hub can generate its own TLS certificate on first start. Your own cert/key and operation behind a reverse proxy are also possible. On first pairing, the Player uses **trust-on-first-use / certificate pinning**: show/confirm the certificate fingerprint (as the short security code, see above) and store it in the Hub profile. Later mismatches produce a warning/error and are not silently accepted as new trust. Concrete certificate management including renewal and confirmed pin change remains to be specified. A custom ACME client is outside the PoC. WebRTC is separately encrypted independently of this.

### Accounts on one PC (0.18, package 7)

- **Several accounts per server on one PC**: every person has their own device row on the server with their own device ID; all share the PC's `install_id`. The first account on a server uses the install ID as its device ID. `profiles.json` lists the accounts of a server; secrets stay in the OS credential store.
- **PIN for this PC**: four digits, set per person, used to switch people. It is never sent to a server. The hash (PBKDF2-HMAC-SHA256 with a random salt, the device ID mixed in) lives only in the OS credential store (`Uzume/pin/<hub_id>/<device_id>`); `profiles.json` knows only that a PIN exists and the count of wrong tries. After 3 wrong PINs the PIN is locked and the password is needed. It keeps the next person at the keyboard out; it is not a boundary against someone who controls the PC's user session.
- **Sign-in**: with user name and password a new device is trusted directly (ADR 0030 D5); a user without a password uses an invite or the admin's approval.
- **Setup**: a server without an admin is set up with its setup code. The app computes the security code from the certificate it pinned and the person compares it with the server's screen before the setup code or an invite is sent; a mismatch stops with nothing sent.
- **Single instance and links**: only one app runs per user and data directory; a second start passes its `uzume://join/...` link to the running one over a local socket and exits ([join link](../reference/protocol.md#join-link)). On Windows the app registers the `uzume` scheme under HKCU (`HKCU\Software\Classes\uzume`), without admin rights.

### Switching and removing

A Hub switch first ends the old connection including presence and signaling. Running local/shared Sessions are ended before the switch; pending saves are uploaded to the previous Hub or unambiguously secured for this Hub for later retry. If reconciliation fails, the UI must allow a deliberate decision. An upload must never be redirected to the newly selected Hub. Only then is the new Hub connected.

When removing a profile, the Player deletes the local profile assignment and its credentials; open saves are resolved beforehand. Hub account, ROMs and central saves are not deleted by this. Device credentials can be revoked in the Hub under **Clients**; local removal alone does not replace a server-side revocation.

The PoC covers this Hub profile/pairing base model. Simultaneous multi-Hub use, federation, cross-Hub Sessions, OAuth and central FrameBeam accounts are excluded. Shared Sessions in the PoC connect users of the same active Hub.
