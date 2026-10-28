# Architecture: UI and navigation

## 9. Product terms, navigation and presentation

In the user interface, a running or shared game session is consistently called a **Session**: "Share Session", "Watch Session" and "Add to multiview". **Stream** denotes only the technical transmission path in diagnostics. Technical terms in implementation and protocol remain permitted.

Player and Hub each support a selectable **dark/light presentation** with a common design language. The choice is available in the respective settings.

| FrameBeam Hub | FrameBeam Player |
|---|---|
| Library | Library |
| Saves including history and conflict state | Emulation |
| Systems & Cores | Controllers |
| Clients (pending requests, Trusted/Revoked, Revoke access) | Settings |
| Users (admin-controlled users and onboarding invites) | — |
| Settings | During play: running Session, multiview, optional diagnostics |

> Hub navigation (Round 4, `docs/design/legacy/hub.md` 0-3 E): the sidebar has three groups. Play (no eyebrow): Library, Saves, Runs, Plugins; SETUP: Systems & Cores, Clients, Users; ADMIN: Status, Settings (Reports joins ADMIN with its page). Members would see only Play and Settings; the web interface is admin-only today, so the member view is only prepared (`navGroups` in `server/internal/web/nav.go`). Since 0.18 package 12 ([ADR 0030](../adr/0030-one-app-one-server-uzume.md) D2) this web interface no longer exists; the admin screens are in the app (SERVER group, Server › Settings, `docs/design/uzume/`). Status carries the most urgent problem as an amber badge. Settings has a section column in groups HUB (General, Network & TURN, Security, Updates, Diagnostics), DATA (Saves retention; Backups and Storage come with their features) and INTEGRATIONS (Metrics). **Status** (`/status`, admin) is the Hub health page; **Settings → Metrics** switches the optional Prometheus `/metrics` endpoint (bearer token, off by default). Details: `https://github.com/phabioo/uzume/wiki/Server-configuration`.

> Refined by [ADR 0014](../adr/0014-player-ui-pass.md) (Player, 0.6: new shell, Settings with Updates / Hubs / Appearance / Diagnostics as jump list, split diagnostics) and [ADR 0015](../adr/0015-hub-ui-pass.md) (Hub, 0.7: fragment navigation, live updates, Settings sub-pages Updates / General / Network / Security; Diagnostics added in 0.10.x: admin-only download of the Hub diagnostics bundle, see `https://github.com/phabioo/uzume/wiki/Server-configuration`). Navigation items are unchanged.

Before the main navigation, the Player gets a **start/connection screen** with stored Hubs, connection status, "Add Hub" and "Connect". Without a successful connection, this screen remains reachable; optional auto-connect to the last used Hub leads directly to the library on success. A compact Hub switcher as well as **Settings → Hubs** offer switching, removal and the auto-connect setting. No additional main navigation page is created. On switching, running Sessions are ended first and pending save uploads are secured or resolved (see section 14).

The existing Hub pages remain; user management supplements them. The admin manages library, users, clients/pairings, systems/cores/firmware and Hub settings. Normal users use library, their saves and Sessions; the visible actions follow their permissions. Pending requests appear on Clients with device name, platform, Player version and Allow/Deny. "Allow users to upload games" lives in Hub settings. Firmware required/missing, pairing, certificate and compatibility errors are actionable states and remain directly visible. Later provider configuration lives under **Settings → Metadata**; metadata actions live directly in the library entry (see section 15).

The Hub gets **no Sessions management page**, no live Sessions dashboard and no video preview. Its Session metadata remains internal for presence, visibility/permissions and signaling. Session discovery, sharing, watching and multiview belong in the Player. The existing Session protocol is retained for this.

FPS, latency, encoder, codec, bitrate, WebRTC/streaming statistics and technical debug data appear exclusively in an optionally shown or collapsible diagnostics area. They do not form the primary UX. Actionable states such as download needed and save conflicts, by contrast, remain immediately visible.
