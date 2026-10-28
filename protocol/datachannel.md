# fb-diag DataChannel

Between the apps of a Session (owner and each viewer) a negotiated WebRTC DataChannel `fb-diag` (id 0, same SDP flow, no server involvement) is opened. It brings up SCTP (RTT measurement) and carries small JSON text messages. Unknown message types and unknown fields are ignored by every reader, so new optional messages need no `protocol_version` bump.

| Message | Direction | Since | Schema |
|---|---|---|---|
| `{"t":"rx","loss":0..1,"kbps":n,"fps":n?,"dec":"h264"?}` | viewer to owner, once a second | 0.4 (`fps`, `dec`: 0.6) | ADR 0012 D5, ADR 0014 D7 |
| `{"t":"state","paused":bool,"by":"Name"?}` | owner to viewers | 0.14 | [schemas/dc-fb-diag-state.schema.json](schemas/dc-fb-diag-state.schema.json) |
| `{"t":"quality","pick":"auto\|high\|medium\|low"}` | viewer to owner | 0.23 | ADR 0033 D5, below |
| `{"t":"ping","n":int,"ts":ms}` / `{"t":"pong","n":int,"ts":ms}` | both directions | 0.23 | ADR 0033 D5, below |
| `{"t":"tier","tier":"auto\|high\|medium\|low","kbps":n}` | owner to viewer | 0.23 | ADR 0033 D5, below |

## Host state (`state`, ADR 0026 D3)

The owner sends `state` when its game is paused or resumed, and once to every viewer whose channel opens while the game is paused. Viewers show "Paused by `<by>`" on the frozen tile and drop it on `paused: false`. `by` is the owner's display name; without it the viewer uses the Session owner's name. The message carries state only, never a picture or input.

Compatibility: a 0.13 viewer sets no message handler on the channel and a 0.13 owner never sends the message; a 0.13 owner reading a `state` message treats it like any non-`rx` message and ignores it. Viewers that connect to an older owner simply never see a paused state.

## Quality per viewer and latency (0.23, ADR 0033 D5)

All three are additive: an older owner or viewer ignores them (unknown types), and an owner that never sees `quality` serves everybody from its single adaptive encoder as before. A reader ignores a message that is not well-formed (bad JSON, wrong field types, unknown `pick` or `tier` token, `n` or `ts` not a whole number, more than 512 bytes).

- `quality` (viewer to owner): the viewer's pick. It is sent when the channel opens (only if it is not `auto`) and whenever the viewer changes it. `auto` (the default) follows the adaptive encoder; `high` is 4000 kbit/s, `medium` 1500, `low` 600, the fixed tiers at 30 fps (a 60 fps source drops every second frame). The owner keeps one encoder per tier in use, at most three alive at a time (Auto counts as one); the viewer's video track is fed by the encoder of its tier, and a switch starts with a keyframe of the new encoder. When all three encoders are busy the owner serves the closest tier and says so in `tier`. Only viewers on `auto` take part in the bitrate adaptation (`rx` reports of the others are kept for diagnostics only).
- `tier` (owner to viewer): what the viewer gets, sent when the channel opens, after every pick, and for `auto` again when the adaptive target moves by 15 % or more. `tier` is the served tier; for `auto` it is `auto` and `kbps` is the adaptive target. The viewer names an `auto` stream by the closest tier (3000 kbit/s and up High, 1000 and up Medium, else Low).
- `ping` / `pong`: each side sends `{"t":"ping","n":<count>,"ts":<ms>}` every 2 s (`ts` is the sender's own monotonic clock in milliseconds); the receiver answers at once with `{"t":"pong","n":..,"ts":..}` echoing both values. The sender's round trip is `now - ts`; a pong whose `ts` lies in the future or more than 60 s back is dropped. Both apps therefore know the latency without synchronised clocks. This is the number the People tab shows ("Direct · 18 ms").
