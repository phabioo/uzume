# fb-diag DataChannel

Between the apps of a Session (owner and each viewer) a negotiated WebRTC DataChannel `fb-diag` (id 0, same SDP flow, no server involvement) is opened. It brings up SCTP (RTT measurement) and carries small JSON text messages. Unknown message types and unknown fields are ignored by every reader, so new optional messages need no `protocol_version` bump.

| Message | Direction | Since | Schema |
|---|---|---|---|
| `{"t":"rx","loss":0..1,"kbps":n,"fps":n?,"dec":"h264"?}` | viewer to owner, once a second | 0.4 (`fps`, `dec`: 0.6) | ADR 0012 D5, ADR 0014 D7 |
| `{"t":"state","paused":bool,"by":"Name"?}` | owner to viewers | 0.14 | [schemas/dc-fb-diag-state.schema.json](schemas/dc-fb-diag-state.schema.json) |
| `{"t":"quality","pick":"auto\|high\|medium\|low"}` | viewer to owner | 0.23 | ADR 0033 D5, below |
| `{"t":"ping","n":int,"ts":ms}` / `{"t":"pong","n":int,"ts":ms}` | both directions | 0.23 | ADR 0033 D5, below |
| `{"t":"tier","tier":"auto\|high\|medium\|low","kbps":n}` | owner to viewer | 0.23 | ADR 0033 D5, below |
| `{"t":"control_caps","ports":0..2}` | owner to viewer | 0.23 | ADR 0033 D8, below |
| `{"t":"control_request","port":0\|1}` | viewer to owner | 0.23 | ADR 0033 D8, below |
| `{"t":"control","port":n,"granted":bool,"reason":"..."?}` | owner to viewer | 0.23 | ADR 0033 D8, below |
| `{"t":"control_end","port":n,"reason":"host\|viewer\|left"}` | owner to viewer | 0.23 | ADR 0033 D8, below |
| `{"t":"control_release"}` | viewer to owner | 0.23 | ADR 0033 D8, below |

## Host state (`state`, ADR 0026 D3)

The owner sends `state` when its game is paused or resumed, and once to every viewer whose channel opens while the game is paused. Viewers show "Paused by `<by>`" on the frozen tile and drop it on `paused: false`. `by` is the owner's display name; without it the viewer uses the Session owner's name. The message carries state only, never a picture or input.

Compatibility: a 0.13 viewer sets no message handler on the channel and a 0.13 owner never sends the message; a 0.13 owner reading a `state` message treats it like any non-`rx` message and ignores it. Viewers that connect to an older owner simply never see a paused state.

## Quality per viewer and latency (0.23, ADR 0033 D5)

All three are additive: an older owner or viewer ignores them (unknown types), and an owner that never sees `quality` serves everybody from its single adaptive encoder as before. A reader ignores a message that is not well-formed (bad JSON, wrong field types, unknown `pick` or `tier` token, `n` or `ts` not a whole number, more than 512 bytes).

- `quality` (viewer to owner): the viewer's pick. It is sent when the channel opens (only if it is not `auto`) and whenever the viewer changes it. `auto` (the default) follows the adaptive encoder; `high` is 4000 kbit/s, `medium` 1500, `low` 600, the fixed tiers at 30 fps (a 60 fps source drops every second frame). The owner keeps one encoder per tier in use, at most three alive at a time (Auto counts as one); the viewer's video track is fed by the encoder of its tier, and a switch starts with a keyframe of the new encoder. When all three encoders are busy the owner serves the closest tier and says so in `tier`. Only viewers on `auto` take part in the bitrate adaptation (`rx` reports of the others are kept for diagnostics only).
- `tier` (owner to viewer): what the viewer gets, sent when the channel opens, after every pick, and for `auto` again when the adaptive target moves by 15 % or more. `tier` is the served tier; for `auto` it is `auto` and `kbps` is the adaptive target. The viewer names an `auto` stream by the closest tier (3000 kbit/s and up High, 1000 and up Medium, else Low).
- `ping` / `pong`: each side sends `{"t":"ping","n":<count>,"ts":<ms>}` every 2 s (`ts` is the sender's own monotonic clock in milliseconds); the receiver answers at once with `{"t":"pong","n":..,"ts":..}` echoing both values. The sender's round trip is `now - ts`; a pong whose `ts` lies in the future or more than 60 s back is dropped. Both apps therefore know the latency without synchronised clocks. This is the number the People tab shows ("Direct · 18 ms").

## Controller hand-over (0.23, ADR 0033 D8)

All additive: an older app ignores the messages and the second channel, and a viewer never gets a button from an owner that does not send `control_caps`. A reader ignores a message that is not well-formed (bad JSON, `port` other than the whole numbers 0 and 1, wrong field types, more than 512 bytes).

- `control_caps` (owner to viewer): how many controller ports the owner's running system offers (0 = none, 1, 2; the system manifest's `input.ports`, default 1). Sent when the channel opens and whenever it changes (the game ends, another one starts). A viewer offers "Take over P1" with 1 or more, "Play as P2" with 2.
- `control_request` (viewer to owner): the viewer asks for a port. The owner's person answers; one request per viewer, one holder per port.
- `control` (owner to viewer): the answer. `granted: true` starts the hold. `granted: false` carries `reason`: `denied` (the host said no), `timeout` (no answer in 20 s), `busy` (the port is held, or another viewer asked first), `holding` (this viewer already holds a port or has a request open), `unsupported` (no such port, no game, or the `fb-input` channel is not open).
- `control_end` (owner to viewer): the hold ended. `reason` is `host` (Take back, Esc twice, the system ended the port) or `left`. A viewer that ends its own hold sends `control_release` instead (also while a request is open, which withdraws it); the owner does not answer it. A viewer that disappears (PeerConnection closed) ends its hold on the owner's side as `left`.

### Input channel `fb-input`

A second negotiated DataChannel on the same PeerConnection: `fb-input`, id 1, unordered, `maxRetransmits` 0, binary messages. Both sides create it at setup (same as `fb-diag`); an owner or viewer without it never grants or asks. Every message is one frame of 21 bytes, little endian:

| Offset | Size | Field |
|---|---|---|
| 0 | u8 | version, 1 |
| 1 | u8 | port, 0 or 1 |
| 2 | u16 | sequence, wraps |
| 4 | u16 | sender time in ms, wraps (the viewer's own monotonic clock) |
| 6 | u16 | buttons: libretro joypad bits 0 to 15 (B Y Select Start Up Down Left Right A X L R L2 R2 L3 R3) |
| 8 | 4 x i16 | analog axes: left x, left y, right x, right y (-32768 to 32767) |
| 16 | i16 x 2 | touch x, y: 0 to 32767 over the whole video frame |
| 20 | u8 | flags: bit 0 = touch pressed, other bits 0 |

A reader drops a frame of another size or version, with a port above 1, a negative touch coordinate or unknown flag bits. The viewer sends a frame on every change of its input (at most one per 16 ms, one per emulated frame) and repeats the current state every 100 ms as a keep-alive. The owner applies a frame only from the viewer that holds that port and drops it when it is out of order (16-bit sequence compared as a signed difference, equal or older is dropped) or stale: the sender time minus the arrival time is tracked per hold, and a frame that arrives more than 250 ms later than the fastest frame so far is dropped. When no frame has been applied for 500 ms the port is neutral (all buttons up, touch released) until the next frame; the grant stays. The owner turns the sticks into the digital direction bits the system profile reads (beyond half deflection), and a hold that ends puts the port to neutral.
