#!/usr/bin/env bash
# Live-test harness (ADR 0032): a local uzume-server with test data plus N Uzume apps under one Xvfb display, each with
# the debug MCP server. Drive the apps with scripts/mcp-call.sh or `claude mcp add` (lines printed by `up`).
#   scripts/live-app.sh up [--apps N]   start (idempotent: a running harness is only reported); N = 1..4, default 1
#   scripts/live-app.sh down            stop everything
#   scripts/live-app.sh env             print $STATE/live.env (server URL, fingerprint, MCP ports, tokens, passwords)
# State: $UZUME_LIVE_STATE (default $HOME/.cache/uzume/live), outside the repo. Dummy data only, never real ROMs.
# App binary: $UZUME_APP_BIN or client/build/$CLIENT_PRESET/app/uzume (CLIENT_PRESET default linux-debug).
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STATE="${UZUME_LIVE_STATE:-$HOME/.cache/uzume/live}"
ENVF="$STATE/live.env"
PIDS="$STATE/pids"
export CGO_ENABLED=0 GOTOOLCHAIN="${GOTOOLCHAIN:-local}"
USERS=(lena max sam)

die() { echo "live-app: $*" >&2; exit 1; }
alive() { [ -f "$PIDS/$1.pid" ] && kill -0 "$(cat "$PIDS/$1.pid")" 2>/dev/null; }
wait_for() { local n=$(( $1 * 10 )); shift; while [ "$n" -gt 0 ]; do "$@" >/dev/null 2>&1 && return 0; sleep 0.1; n=$((n-1)); done; return 1; }
rand_token() { openssl rand -base64 32 | tr '+/' '-_' | tr -d '=\n'; }
free_port() {
  python3 -I - <<'PY'
import socket
t = socket.socket(); t.bind(("127.0.0.1", 0)); print(t.getsockname()[1]); t.close()
PY
}

summary() {
  # shellcheck disable=SC1090
  . "$ENVF"
  echo "server   $LIVE_SERVER_URL  fingerprint $LIVE_FINGERPRINT"
  echo "users    admin / ${USERS[*]} (passwords in $ENVF)"
  echo "display  $LIVE_DISPLAY"
  local i
  for i in $(seq 1 "$LIVE_APPS"); do
    local port="LIVE_MCP_PORT_$i" tok="LIVE_MCP_TOKEN_$i"
    echo "app $i    http://127.0.0.1:${!port}/mcp"
    echo "  claude mcp add --transport http uzume$i http://127.0.0.1:${!port}/mcp --header \"Authorization: Bearer ${!tok}\""
  done
  echo "example  scripts/mcp-call.sh 1 server_sign_in '{\"url\":\"$LIVE_SERVER_URL\",\"username\":\"lena\",\"password\":\"<LIVE_PW_lena>\",\"trust_fingerprint\":\"$LIVE_FINGERPRINT\"}'"
  echo "example  curl -s -H \"Authorization: Bearer \$LIVE_MCP_TOKEN_1\" -H 'Content-Type: application/json' -H 'Accept: application/json, text/event-stream' -d '{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}' http://127.0.0.1:\$LIVE_MCP_PORT_1/mcp"
}

cmd_down() {
  local f p
  if [ -d "$PIDS" ]; then
    for f in $(ls -r "$PIDS" 2>/dev/null); do
      p=$(cat "$PIDS/$f" 2>/dev/null) || continue
      kill "$p" 2>/dev/null
      wait_for 5 bash -c "! kill -0 $p 2>/dev/null" || kill -9 "$p" 2>/dev/null
    done
    rm -rf "$PIDS"
  fi
  rm -f "$ENVF"
  echo "live-app: stopped"
}

cmd_up() {
  local apps=1
  while [ $# -gt 0 ]; do
    case "$1" in
      --apps) apps="${2:-}"; shift 2 ;;
      *) die "unknown option $1" ;;
    esac
  done
  case "$apps" in 1|2|3|4) ;; *) die "--apps must be 1..4" ;; esac

  if [ -f "$ENVF" ] && alive server; then
    echo "live-app: already running"; summary; return 0
  fi
  [ -f "$ENVF" ] || [ -d "$PIDS" ] && cmd_down >/dev/null  # stale state of a crashed run

  local bin="${UZUME_APP_BIN:-$ROOT/client/build/${CLIENT_PRESET:-linux-debug}/app/uzume}"
  [ -x "$bin" ] || die "app binary not found: $bin (build it: make check-client, or set UZUME_APP_BIN)"
  for t in Xvfb openssl curl python3 go; do command -v "$t" >/dev/null || die "$t is required"; done

  rm -rf "$STATE/server" "$STATE"/app[0-9]* "$STATE/shots"
  mkdir -p "$STATE/bin" "$STATE/server" "$STATE/logs" "$PIDS" "$STATE/shots"
  (cd "$ROOT/server" && go build -o "$STATE/bin/uzume-server" ./cmd/uzume-server) >"$STATE/logs/build.log" 2>&1 \
    || { tail -n 15 "$STATE/logs/build.log"; die "server build failed"; }

  # --- server
  local admin_pw="Adm-$(openssl rand -hex 8)"
  export UZUME_DATA_DIR="$STATE/server"
  printf '%s\n' "$admin_pw" | "$STATE/bin/uzume-server" setup-admin -username admin >"$STATE/logs/setup.log" 2>&1 \
    || { tail -n 15 "$STATE/logs/setup.log"; die "setup-admin failed"; }
  local port addr
  port=$(free_port); addr="127.0.0.1:$port"
  "$STATE/bin/uzume-server" -listen "$addr" -plugin-index-url none >"$STATE/logs/server.log" 2>&1 &
  echo $! >"$PIDS/server.pid"
  wait_for 20 curl -fsk "https://$addr/.well-known/uzume" || { tail -n 15 "$STATE/logs/server.log"; cmd_down >/dev/null; die "server did not start"; }
  local fp
  fp=$(openssl s_client -connect "$addr" </dev/null 2>/dev/null | openssl x509 -noout -fingerprint -sha256 2>/dev/null | sed 's/.*=//')
  [ -n "$fp" ] || { cmd_down >/dev/null; die "could not read the certificate fingerprint"; }

  # --- seed through the admin API (helpers of the e2e scripts)
  ADDR="$addr"; TMP="$STATE"
  # shellcheck source=scripts/e2e-admin-api.sh
  . "$ROOT/scripts/e2e-admin-api.sh"
  admin_login "$admin_pw" || { cmd_down >/dev/null; die "admin sign-in failed (see $STATE/logs/server.log)"; }
  head -c 4096 /dev/zero | tr '\0' 'U' >"$STATE/live-dummy.nds"  # dummy bytes, not a ROM
  [ "$(upload_game "$STATE/live-dummy.nds" "Live Dummy Game")" = "201" ] || { cmd_down >/dev/null; die "dummy game upload failed"; }
  local u pw code body
  { echo "LIVE_SERVER_URL=https://$addr"; echo "LIVE_SERVER_ADDR=$addr"; echo "LIVE_FINGERPRINT=$fp"
    echo "LIVE_ADMIN_PASSWORD=$admin_pw"; echo "LIVE_APPS=$apps"; } >"$ENVF"
  for u in "${USERS[@]}"; do
    pw="Pw-$(openssl rand -hex 8)"
    body=$(python3 -I -c '
import json, sys, uuid
print(json.dumps({"code": sys.argv[1], "display_name": sys.argv[2], "password": sys.argv[3], "device_id": str(uuid.uuid4()),
                  "device_name": "live-seed", "platform": "linux", "arch": "x86_64", "player_version": "0.0.0", "protocol_version": 1}))' \
      "$(mkinvite)" "$u" "$pw") || { cmd_down >/dev/null; die "invite for $u failed"; }
    code=$(curl -sk -o /dev/null -w '%{http_code}' -H 'Content-Type: application/json' -d "$body" "https://$addr/api/v1/invites/redeem")
    [ "$code" = "200" ] || { cmd_down >/dev/null; die "creating user $u failed (HTTP $code)"; }
    echo "LIVE_PW_$u=$pw" >>"$ENVF"
  done

  # --- Xvfb: the first free display from :99
  local d=99
  while [ -e "/tmp/.X11-unix/X$d" ] || [ -e "/tmp/.X$d-lock" ]; do d=$((d+1)); done
  Xvfb ":$d" -screen 0 1280x800x24 -nolisten tcp >"$STATE/logs/xvfb.log" 2>&1 &
  echo $! >"$PIDS/xvfb.pid"
  wait_for 10 test -e "/tmp/.X11-unix/X$d" || { cmd_down >/dev/null; die "Xvfb did not start (see $STATE/logs/xvfb.log)"; }
  echo "LIVE_DISPLAY=:$d" >>"$ENVF"

  # --- apps
  local i mport tok
  for i in $(seq 1 "$apps"); do
    mport=$((47615 + i)); tok=$(rand_token)
    mkdir -p "$STATE/app$i/xdg"
    DISPLAY=":$d" QT_QPA_PLATFORM=xcb LIBGL_ALWAYS_SOFTWARE=1 QT_QUICK_BACKEND=software XDG_CONFIG_HOME="$STATE/app$i/xdg" \
      UZUME_MCP_TOKEN="$tok" "$bin" --data-dir "$STATE/app$i" --mcp --mcp-port "$mport" >"$STATE/logs/app$i.log" 2>&1 &
    echo $! >"$PIDS/app$i.pid"
    { echo "LIVE_MCP_PORT_$i=$mport"; echo "LIVE_MCP_TOKEN_$i=$tok"; } >>"$ENVF"
    wait_for 40 curl -fs -o /dev/null -H "Authorization: Bearer $tok" -H 'Content-Type: application/json' \
      -d '{"jsonrpc":"2.0","id":1,"method":"ping"}' "http://127.0.0.1:$mport/mcp" \
      || { tail -n 15 "$STATE/logs/app$i.log"; cmd_down >/dev/null; die "app $i: MCP server did not answer on port $mport"; }
  done
  chmod 600 "$ENVF"
  echo "live-app: up"; summary
}

case "${1:-}" in
  up) shift; cmd_up "$@" ;;
  down) cmd_down ;;
  env) [ -f "$ENVF" ] || die "not running (no $ENVF)"; cat "$ENVF" ;;
  *) sed -n '2,8p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
