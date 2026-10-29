# protocol

Shared definition of the server API and the WebSocket messages. Rules for agents: `CLAUDE.md`.

- `openapi/uzume.yaml`: OpenAPI 3.0.3 for `/api/v1`, current spec version 1.49.0. It is the source of the Go code generated into `server/internal/api` (`make generate`; CI fails on stale generated code).
- `schemas/`: WSS message schemas (JSON Schema draft 2020-12) in `schemas/ws-<type>.schema.json` with examples in `schemas/examples/`.
- `datachannel.md`: messages on the app-to-app `fb-diag` DataChannel (rx report, host paused state; optional, no version bump).
- `protocol_version` (currently 1) is an integer separate from the product versions. Server and app each report `protocol_version` and `min_protocol_version`; an incompatible pair is rejected with `player_too_old` or `hub_too_old`. Optional capabilities are negotiated as handshake features (for example `saves_v2`, `turn_v1`).
- Errors: `{"error": {"code": <enum>, "message": string}}`. Auth: Bearer tokens.

Full reference (endpoint table, error codes, WSS messages, history per milestone): [docs/reference/protocol.md](../docs/reference/protocol.md). Release feed of the updaters: [docs/reference/update-index.md](../docs/reference/update-index.md).

Changes to the protocol always go together with server and app changes; a breaking change needs a `protocol_version` bump.
