#!/usr/bin/env bash
# Calls one MCP tool of a live-test app (scripts/live-app.sh up) over JSON-RPC with curl and prints the result text.
# Images are saved as PNG under $STATE/shots and the path is printed. Exit 1 when the tool reports an error.
# Usage: scripts/mcp-call.sh <app-index> <tool> ['<json-args>']      e.g. scripts/mcp-call.sh 1 status '{}'
set -uo pipefail
STATE="${UZUME_LIVE_STATE:-$HOME/.cache/uzume/live}"
[ $# -ge 2 ] || { sed -n '2,5p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 2; }
IDX="$1" TOOL="$2" ARGS="${3:-{\}}"
[ -f "$STATE/live.env" ] || { echo "mcp-call: harness not running (scripts/live-app.sh up)" >&2; exit 2; }
# shellcheck disable=SC1091
. "$STATE/live.env"
PORT_VAR="LIVE_MCP_PORT_$IDX" TOK_VAR="LIVE_MCP_TOKEN_$IDX"
PORT="${!PORT_VAR:-}" TOK="${!TOK_VAR:-}"
[ -n "$PORT" ] && [ -n "$TOK" ] || { echo "mcp-call: no app $IDX" >&2; exit 2; }
URL="http://127.0.0.1:$PORT/mcp"
rpc() { # rpc <json body>; prints the response body
  curl -sS --max-time "${MCP_TIMEOUT:-120}" -H "Authorization: Bearer $TOK" -H 'Content-Type: application/json' \
    -H 'Accept: application/json, text/event-stream' -d "$1" "$URL"
}
rpc '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"mcp-call","version":"1"}}}' >/dev/null || exit 1
rpc '{"jsonrpc":"2.0","method":"notifications/initialized"}' >/dev/null
BODY=$(python3 -I -c 'import json, sys; print(json.dumps({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":sys.argv[1],"arguments":json.loads(sys.argv[2])}}))' "$TOOL" "$ARGS") \
  || { echo "mcp-call: args are not valid JSON" >&2; exit 2; }
mkdir -p "$STATE/shots"
rpc "$BODY" | python3 -I -c '
import base64, json, sys, time
r = json.load(sys.stdin)
if "error" in r:
    print("rpc error:", json.dumps(r["error"])); sys.exit(1)
res = r["result"]; n = 0
for c in res.get("content", []):
    if c.get("type") == "image":
        n += 1
        p = "%s/shots/%s-%d-%d.png" % (sys.argv[1], sys.argv[2], int(time.time()), n)
        open(p, "wb").write(base64.b64decode(c["data"])); print("image:", p)
    else:
        print(c.get("text", ""))
sys.exit(1 if res.get("isError") else 0)' "$STATE" "$TOOL"
