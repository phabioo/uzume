# ADR 0033: Together (0.23)

- Status: Accepted for implementation; scope decided by Fabio on 2026-10-21 (roadmap 0.23), details proposed by Claude on 2026-10-27.
- Date: 2026-10-27
- Deciders: Fabio (scope, roadmap 2026-10-21/23)

## Overview

0.23 makes shared play social: an online roster, notifications, ready check and shared pause, chat, a viewer role with quality per viewer, an audio mixer, voice chat and controller hand-over. D1..D8 below fix the wire names, limits and app behaviour. UI follows [design/uzume/play-and-plugins.md](../design/uzume/play-and-plugins.md) (3g-7, 3g-9, 3g-11, 3g-12, 3r, 6w-1, 6w-2, 6r-1, 6r-2) and `decisions.md`; there is no own design round. Rule from Fabio (2026-10-27): new or changed UI is simple and flat (few clicks, nothing nested), the Uzume design stays. The plugin Lua API and the run rules are only extended, never restructured.

## Context

What exists today:

- `presence_update` is broadcast over WSS, but the app shows nothing of it.
- There is no chat, no ready check and no notifications.
- A host runs one encoder for all viewers; quality cannot be chosen per viewer.
- The app has one audible surface (the game or one Multiview tile) and no mixer.
- There is no microphone and no voice.
- Viewers cannot send input; there is no controller hand-over.

Parallel milestones: DB migration numbers, the OpenAPI `info.version` and the ADR number are assigned at merge time (next free on `main`, renumber on rebase).

## Decisions

### D1 Online status

- `presence_update` (client to server) gains an optional `session_id` (the device's shared session). The server broadcast adds `user_id`, `display_name`, `state` (`online|in_game|offline`), `game_id`, `game_title`, `system`, and `session_id` only for devices the session ACL allows (otherwise omitted). In memory only, no table.
- App: an online roster in AppNav ONLINE lists people, not devices (a user is online if any device is): name and what they play. Row actions "Watch" (when a visible session exists) and "Invite" (to my shared session or to my run); one click each, no submenu.

### D2 Notifications

- App-side only, derived from existing WSS events: `session_invite`, `session_update` for a newly started session of someone else, `run_updated` invite, `run_event` of notable kinds, chat @mention, app update available, server problems (admins). No server table, no migration.
- Bell in AppNav with unread count. Popover "Notifications": entry text plus a Mono line "time · ACTION" (WATCH, OPEN RUN, WHAT'S NEW, OPEN STATUS); one click acts. Kept per server in memory, last 50.
- Global toast queue (bottom left; in game on the stage). Settings › This PC › Notifications: per kind a Segmented "Toast / List only" (Invites, Sessions started, Run events, Updates, Server problems (admins)); Toggles "Quiet in fullscreen" (only @mentions toast) and "Sound for toasts".

### D3 Ready check and shared pause

- WSS client to server: `ready_check_start {scope: "run"|"session", id}`, `ready_check_answer {check_id, ready: bool}`, `ready_check_cancel {check_id}`, `pause_request {scope, id, paused: bool}`.
- Server to clients: `ready_check {check_id, scope, id, by, status: open|all_ready|cancelled|expired, expires_at, members:[{user_id, state: pending|ready|not_ready}]}` and `pause_state {scope, id, paused, by}`.
- Members are the joined run participants, or the session owner plus viewers. Any member may start a check or pause/resume. One open check per scope; 30 s expiry; in memory only.
- App: "Ready check…" and "Pause for everyone" (Shift+Esc) in the ⋯ menu and the quick menu (group Together). The ready-check card has Ready / Not yet (two buttons, keys 1/2) and the member list. `pause_state` pauses the local game of every member in a run; in a session the host's game pauses (viewers see it via the existing DC state). Toast "Lena paused for everyone" stays until resumed. The run status "paused" (run lifecycle) is unrelated and unchanged.
- Feature flag `together_v1` in `hello_ack`.

### D4 Chat

- WSS client to server: `chat_send {scope, id, client_id, text}` (1..500 chars, 2 msg/s). Server to members: `chat_message {scope, id, seq, user_id, text, sent_at, mentions:[user_id], client_id}`.
- Run chat is stored (migration: table `run_chat_messages`, max 10000 per run, deleted with the run) and read with `GET /api/v1/runs/{id}/chat?after_seq=&limit=`. Session chat lives in memory (ring of 200) and ends with the session; `GET /api/v1/sessions/{id}/chat` returns the ring.
- Emotes are fixed shortcodes (:gg: :wave: :lol: :hype: :sad: :ok:) rendered as unicode by the app. Pings: `@name` mentions resolve to user IDs on the server; a mention raises a notification that ignores "quiet". Quick replies 1/2/3 (Down!, Gym next, Wait).
- Panel tabs become Session · People · Chat · Saves (run: Run · Chat · Saves · <plugin>); states live in Saves; Console stays a developer tab. Chat tab: day separators, ChatMessage with person colour and time, run events inline in a run, composer (Enter sends, Esc back to game).

### D5 Viewer role and quality per viewer

- Roles: HOST, VIEWER, PLAYER (a viewer with a granted controller, D8). Viewers watch, chat and talk; nothing else.
- Quality per viewer: Auto (adaptive, default), High (4 Mbit/s), Medium (1.5 Mbit/s), Low (0.6 Mbit/s, 30 fps). The viewer picks it in the People tab (Segmented), sent as DC `{"t":"quality","pick":"auto|high|medium|low"}`. The host keeps at most three encoders, one per tier in use (Auto shares the adaptive encoder); GPU-direct only for the adaptive encoder, extra tiers in software.
- Latency: DC `{"t":"ping","n":..}` / `{"t":"pong","n":..}` every 2 s; the RTT is shown in People ("Direct · 18 ms · gets High" / "picked Medium").

### D6 Audio mixer stage 2

- One `AudioMixer` and one output sink in the app. Channels: game (local), tiles (each watched session, per-person volume), voice (per person, D7), plugin sounds. Mixing in Multiview is optional: Toggle "Mix all tiles" (off = the one audible tile, as today).
- Sound popover (3g-9): sliders My game, Voice, Plugin sounds. Mixer popover in Multiview: one Slider per person plus "Mix all tiles".
- Plugin sounds: additive Lua API `fb.sound.play(path, {volume=0..1})` and `fb.sound.stop_all()`, playing a 16-bit PCM WAV from the plugin package; new permission `sound`. Additive only.

### D7 Voice chat

- Spike result: no libwebrtc. The speexdsp preprocessor (noise suppression) and its echo canceller with the mixer output as far-end reference, held back by the sink buffer. Differs from the plan: speexdsp 1.2.1 has no real VAD (a placeholder hack), so voice activation is a level threshold with hangover on the processed signal, and AGC stays off so that the threshold holds. Measured on synthetic signals: noise −22 dB, echo −57 dB (100 ms filter); per 20 ms frame 0.4 ms suppression plus echo cancellation, 0.6 ms Opus VOIP encode (24 kbit/s, FEC), 0.04 ms decode per talker (debug build). Push-to-talk is the default; headphones recommended.
- Transport: P2P mesh over WebRTC (libdatachannel), one Opus mono audio track (24 kbit/s, 20 ms) per peer pair. A room is a run or a session, at most 8 members. TURN fallback as for sessions.
- WSS: `voice_join {scope,id}`, `voice_leave`, `voice_state {mic: on|off|ptt, muted_by_host?}`, `voice_mute {scope,id,user_id,muted}` (host only), `voice_signal` (relay between room members, like `signal`). Server to clients: `voice_room {scope,id,members:[{user_id,device_id,mic,muted_by_host}]}`. Room membership in memory only.
- App: mic capture (QAudioSource); push-to-talk (V, Select+R3 hold) or voice activation (threshold); mic toggle Shift+M; per-person volume; ducking ("Lower game volume while someone speaks" halves game and tiles); speaking marks; Settings › This PC › Voice (6w-1) and the six header mic states (6w-2). Voice joins automatically for runs and invite-only sessions (setting).

### D8 Controller hand-over

- A viewer asks in People: "Play as P2" (systems with 2+ ports) or "Take over P1", sent as DC `{"t":"control_request","port":0|1}`. The host gets one confirm dialog (Allow / Deny); the answer is `{"t":"control","port":n,"granted":bool}`.
- Input travels over a second negotiated DC `fb-input` (unordered, maxRetransmits 0) as compact frames {port, buttons bitmask, analog, touch}, at most one per frame. The host applies them to that libretro port. For a P1 take-over the host's own P1 input is suspended, host hotkeys stay; "Take back" (one click / Esc twice) ends it. Disconnect or leaving ends it. The server is not involved.

## Consequences

- Protocol additions are additive: no `protocol_version` bump, announced by the feature `together_v1` in `hello_ack`. Older apps ignore the new messages.
- One migration (`run_chat_messages`); everything else is in memory or app-side.
- New dependency speexdsp (BSD-3); `THIRD-PARTY-NOTICES` and the app notices are updated.
- UI rule "simple and flat" applies to all 0.23 screens; deviations need Fabio's decision.
- The host carries up to three encoders and a voice mesh of up to 8; Low tier (30 fps) and software encoding for extra tiers bound the cost.
- Voice quality without libwebrtc depends on speexdsp echo cancellation; headphones stay the recommendation.
