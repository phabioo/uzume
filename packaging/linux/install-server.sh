#!/usr/bin/env bash
# Install / upgrade / uninstall the Uzume server as a systemd service.
# Installs a local binary; nothing is downloaded. See wiki/Server-installation.md.
set -euo pipefail

SVC=uzume-server
SVC_USER=uzume
DEFAULT_DATA_DIR=/var/lib/uzume
DEFAULT_LISTEN=:8443

# UZUME_INSTALL_ROOT prefixes all paths; when set, user/chown/systemctl/runuser
# calls are only printed (used by scripts/check.sh to test without root).
P="${UZUME_INSTALL_ROOT:-}"
BIN_DEST="$P/usr/local/bin/uzume-server"
ENV_DIR="$P/etc/uzume"
ENV_FILE="$ENV_DIR/server.env"
UNIT_DEST="$P/etc/systemd/system/$SVC.service"
DROPIN_DIR="$UNIT_DEST.d"
DROPIN_FILE="$DROPIN_DIR/data-dir.conf"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

CMD=install BINARY="" PORT="" LISTEN="" DATA_DIR="" NAME="" ADMIN=""
NO_START=0 PURGE=0 YES=0 IMPORT_DIR=""

usage() {
  cat <<USAGE
Usage: install-server.sh [install|upgrade|uninstall|renew-cert|import-cores DIR|status] [options]

  install (default)  Install the server binary, config and systemd service.
  upgrade            Replace the binary and restart the service (data/config untouched).
  uninstall          Remove service and binary; keep config and data unless --purge.
  renew-cert         Renew the self-generated TLS certificate as the service user in the
                     service's data dir, then restart the service. Prints the new fingerprint.
  import-cores DIR   Offline import of libretro buildbot core zips from DIR as the service user
                     into the service's data dir: DIR/<platform>/<core>_libretro.<suffix>.zip
                     (platform linux-x64 or windows-x64), optionally DIR/info.zip and
                     DIR/<platform>/.index-extended. The service keeps running.
  status             Show service status and URL.

Options:
  --binary PATH      Server binary (default: uzume-server-linux-<arch> next to this
                     script, then in the current directory)
  --port N           Listen port (default 8443). Port in use? Use e.g. --port 8444
  --listen ADDR      Full listen address, e.g. 127.0.0.1:8444 (instead of --port)
  --data-dir DIR     Data directory (default $DEFAULT_DATA_DIR)
  --name NAME        Server name (used on the first start only)
  --admin USER       Create the admin account (password from prompt or one stdin line)
  --no-start         Do not enable/start the service
  --purge            uninstall: also delete /etc/uzume and the data dir
  --yes              uninstall --purge: skip the confirmation
  -h, --help         This help

Environment: UZUME_INSTALL_ROOT=DIR prefixes all paths and skips root-only
actions (for tests).
USAGE
}

die()  { echo "error: $*" >&2; exit 1; }
warn() { echo "warning: $*" >&2; }
info() { echo "$*"; }
testmode() { [ -n "$P" ]; }

# sys CMD...: run a system-modifying command, or only print it in test mode.
sys() {
  if testmode; then echo "[dry-run] $*"; else "$@"; fi
}

while [ $# -gt 0 ]; do
  case "$1" in
    install|upgrade|uninstall|renew-cert|status) CMD="$1" ;;
    import-cores) CMD="$1" ;;
    --binary)   [ $# -ge 2 ] || die "--binary needs a value"; BINARY="$2"; shift ;;
    --port)     [ $# -ge 2 ] || die "--port needs a value"; PORT="$2"; shift ;;
    --listen)   [ $# -ge 2 ] || die "--listen needs a value"; LISTEN="$2"; shift ;;
    --data-dir) [ $# -ge 2 ] || die "--data-dir needs a value"; DATA_DIR="$2"; shift ;;
    --name)     [ $# -ge 2 ] || die "--name needs a value"; NAME="$2"; shift ;;
    --admin)    [ $# -ge 2 ] || die "--admin needs a value"; ADMIN="$2"; shift ;;
    --no-start) NO_START=1 ;;
    --purge)    PURGE=1 ;;
    --yes)      YES=1 ;;
    -h|--help)  usage; exit 0 ;;
    *)
      if [ "$CMD" = import-cores ] && [ -z "$IMPORT_DIR" ] && [ "${1#-}" = "$1" ]; then
        IMPORT_DIR="$1"
      else
        usage >&2; die "unknown argument: $1"
      fi ;;
  esac
  shift
done

if [ -n "$PORT" ] && [ -n "$LISTEN" ]; then die "use either --port or --listen, not both"; fi
if [ -n "$PORT" ]; then
  case "$PORT" in ''|*[!0-9]*) die "invalid --port: $PORT" ;; esac
  { [ "$PORT" -ge 1 ] && [ "$PORT" -le 65535 ]; } || die "--port must be 1-65535"
  LISTEN=":$PORT"
fi
if [ -n "$LISTEN" ]; then
  case "${LISTEN##*:}" in ''|*[!0-9]*) die "invalid --listen (expected [host]:port): $LISTEN" ;; esac
  case "$LISTEN" in *[[:space:]\"\'\\]*) die "invalid --listen: $LISTEN" ;; esac
fi
if [ -n "$DATA_DIR" ]; then
  case "$DATA_DIR" in /*) ;; *) die "--data-dir must be an absolute path" ;; esac
  case "$DATA_DIR" in *[[:space:]\"\'\\]*) die "--data-dir must not contain spaces, quotes or backslashes" ;; esac
  DATA_DIR="${DATA_DIR%/}"
  [ -n "$DATA_DIR" ] || die "--data-dir must not be /"
fi
if [ -n "$ADMIN" ]; then
  case "$ADMIN" in *[[:space:]]*) die "--admin must not contain spaces" ;; esac
fi

if [ "$CMD" != status ] && ! testmode && [ "$(id -u)" -ne 0 ]; then
  die "must run as root (try: sudo $0 $CMD ...)"
fi

# --- server.env helpers -------------------------------------------------------

# Keys are UZUME_<X>.

env_get() { # KEY: value from server.env (surrounding quotes stripped), empty if absent
  [ -f "$ENV_FILE" ] || return 0
  local v
  v="$(grep -m1 "^$1=" "$ENV_FILE" | cut -d= -f2- || true)"
  v="${v#\"}"; v="${v%\"}"
  printf '%s' "$v"
}

env_set() { # KEY VALUE: replace the line or append it; keeps file mode/owner
  local key="$1" val="$2" tmp
  tmp="$(mktemp)"
  if grep -q "^$key=" "$ENV_FILE"; then
    V="$val" awk -v k="$key" 'index($0, k "=") == 1 { print k "=" ENVIRON["V"]; next } { print }' \
      "$ENV_FILE" >"$tmp"
  else
    cat "$ENV_FILE" >"$tmp"
    printf '%s=%s\n' "$key" "$val" >>"$tmp"
  fi
  cat "$tmp" >"$ENV_FILE"
  rm -f "$tmp"
}

quote_name() { # NAME -> "NAME" with \ and " escaped (systemd EnvironmentFile syntax)
  local n="${1//\\/\\\\}"
  printf '"%s"' "${n//\"/\\\"}"
}

# validate_data_dir DIR: refuse shared/system directories (chown -R / rm -rf targets).
validate_data_dir() {
  local d
  d="$(printf '%s' "$1" | tr -s /)"
  d="${d%/}"
  case "$d" in /*) ;; *) die "data dir must be an absolute path: '$1'" ;; esac
  case "$d" in */..|*/../*) die "data dir must not contain '..': '$1'" ;; esac
  case "$d" in
    /home|/home/*|/root|/root/*)
      die "data dir '$1' is under /home or /root, which is not supported (the uzume user cannot traverse private home directories). Install with the default data dir and copy: sudo cp -a <old dir>/. /var/lib/uzume/" ;;
    ""|/bin|/boot|/dev|/etc|/lib|/lib64|/opt|/proc|/run|/sbin|/srv|/sys|/tmp|/usr|/var|/mnt|/media|\
    /usr/local|/usr/lib|/usr/share|/var/lib|/var/log|/var/cache|/var/tmp|/var/spool)
      die "data dir '$1' is a shared system directory; use a dedicated directory such as /var/lib/uzume or /srv/uzume" ;;
  esac
}

effective_listen()   { local v; v="$(env_get UZUME_LISTEN)";   echo "${v:-$DEFAULT_LISTEN}"; }
effective_data_dir() { local v; v="$(env_get UZUME_DATA_DIR)"; echo "${v:-$DEFAULT_DATA_DIR}"; }

# --- shared helpers --------------------------------------------------------

detect_arch() {
  case "$(uname -m)" in
    aarch64|arm64) echo arm64 ;;
    x86_64|amd64)  echo amd64 ;;
    *) die "unsupported architecture: $(uname -m) (supported: aarch64, x86_64)" ;;
  esac
}

find_binary() {
  if [ -n "$BINARY" ]; then
    [ -f "$BINARY" ] || die "binary not found: $BINARY"
    return
  fi
  local name; name="uzume-server-linux-$(detect_arch)"
  if [ -f "$SCRIPT_DIR/$name" ]; then BINARY="$SCRIPT_DIR/$name"
  elif [ -f "./$name" ]; then BINARY="./$name"
  else die "no binary given and $name not found next to the script or in the current directory (use --binary PATH)"
  fi
}

binary_version() { "$1" -version 2>&1 | head -n1 || true; }

install_binary() { # SRC: atomic copy to temp + mv
  local tmp
  install -d -m 0755 "$(dirname "$BIN_DEST")"
  tmp="$(mktemp "$BIN_DEST.XXXXXX")"
  if ! install -m 0755 "$1" "$tmp"; then rm -f "$tmp"; die "could not copy binary"; fi
  mv -f "$tmp" "$BIN_DEST"
}

# License and third-party notices: best effort, from next to the script (flat copy) or from a repository checkout.
install_notices() {
  local f src found=0 dest="$P/usr/local/share/doc/uzume-server"
  for f in THIRD-PARTY-NOTICES.txt LICENSE; do
    src=""
    for c in "$SCRIPT_DIR/$f" "$SCRIPT_DIR/../../server/$f" "$SCRIPT_DIR/../../$f"; do
      [ -f "$c" ] && { src="$c"; break; }
    done
    [ -n "$src" ] || continue
    install -d -m 0755 "$dest"
    install -m 0644 "$src" "$dest/$f"
    found=1
  done
  [ "$found" -eq 1 ] || info "Note: LICENSE and THIRD-PARTY-NOTICES.txt not found next to the script; copy them to /usr/local/share/doc/uzume-server/ (GPL-3.0-or-later)."
}

check_binary_runs() {
  [ -x "$BINARY" ] || chmod +x "$BINARY" 2>/dev/null || true
  "$BINARY" -version >/dev/null 2>&1 || die "$BINARY does not run on this machine (wrong architecture?)"
}

service_active() { ! testmode && systemctl is-active --quiet "$SVC"; }

host_ip() {
  local ip=""
  ip="$(hostname -I 2>/dev/null | awk '{print $1}' || true)"
  echo "${ip:-localhost}"
}

print_url() { # [LISTEN]
  local listen="${1:-$(effective_listen)}"
  local host="${listen%:*}"
  case "$host" in
    ""|0.0.0.0|"[::]"|"::") host="$(host_ip)" ;;
    *:*) case "$host" in "["*) ;; *) host="[$host]" ;; esac ;;
  esac
  info "URL:         https://$host:${listen##*:}/"
}

warn_if_port_busy() { # PORT
  command -v ss >/dev/null 2>&1 || return 0
  if ss -ltn 2>/dev/null | awk 'NR>1 {print $4}' | grep -Eq ":$1\$"; then
    warn "port $1 is already in use by another service. Choose a free one with --port, e.g. --port 8444"
  fi
}

print_fingerprint() {
  command -v journalctl >/dev/null 2>&1 || return 0
  local fp=""
  for _ in 1 2 3 4 5; do
    fp="$(journalctl -u "$SVC" --no-pager -n 200 -o cat 2>/dev/null | grep -i fingerprint | tail -n1 || true)"
    [ -z "$fp" ] || break
    sleep 1
  done
  if [ -n "$fp" ]; then info "TLS:         $fp"
  else info "TLS:         fingerprint not found yet: journalctl -u $SVC | grep -i fingerprint"
  fi
}

write_dropin() { # LISTEN DATA_DIR
  local port="${1##*:}" dir="$2" body=""
  if [ "$dir" != "$DEFAULT_DATA_DIR" ]; then
    body+="ReadWritePaths=$dir"$'\n'
  fi
  if [ "$port" -lt 1024 ]; then
    body+="AmbientCapabilities=CAP_NET_BIND_SERVICE"$'\n'
  fi
  if [ -n "$body" ]; then
    install -d -m 0755 "$DROPIN_DIR"
    printf '# Written by install-server.sh\n[Service]\n%s' "$body" >"$DROPIN_FILE"
    chmod 0644 "$DROPIN_FILE"
  else
    rm -f "$DROPIN_FILE"
    rmdir "$DROPIN_DIR" 2>/dev/null || true
  fi
}

read_admin_password() {
  local p1 p2
  if [ -t 0 ]; then
    read -r -s -p "Admin password: " p1; echo >&2
    read -r -s -p "Repeat password: " p2; echo >&2
    [ "$p1" = "$p2" ] || die "passwords do not match"
  else
    IFS= read -r p1 || true
  fi
  [ -n "$p1" ] || die "empty admin password"
  ADMIN_PW="$p1"
}

create_admin() { # DATA_DIR; sets ADMIN_FAILED=1 on a real failure
  local dir="$1" out
  if testmode; then
    info "[dry-run] runuser -u $SVC_USER -- uzume-server setup-admin -username $ADMIN (data dir $dir)"
    return 0
  fi
  if out="$(printf '%s\n' "$ADMIN_PW" | runuser -u "$SVC_USER" -- env "UZUME_DATA_DIR=$dir" \
        "$BIN_DEST" setup-admin -username "$ADMIN" 2>&1)"; then
    info "Admin '$ADMIN' created."
  elif grep -qiE 'already|exists' <<<"$out"; then
    info "Note: an admin already exists, skipped --admin."
  else
    warn "setup-admin failed: $out"
    ADMIN_FAILED=1
  fi
}
ADMIN_PW="" ADMIN_FAILED=0

warn_port_check() { service_active || warn_if_port_busy "$1"; }

# --- commands --------------------------------------------------------------

cmd_install() {
  find_binary
  check_binary_runs
  [ -z "$ADMIN" ] || read_admin_password

  # Effective config: explicit options win, then existing server.env, then defaults.
  local listen data_dir
  listen="${LISTEN:-$(effective_listen)}"
  data_dir="${DATA_DIR:-$(effective_data_dir)}"
  validate_data_dir "$data_dir"
  [ -z "$NAME" ] || ! [ -f "$ENV_FILE" ] || [ -z "$(env_get UZUME_NAME)" ] || \
    warn "UZUME_NAME is only used on the first start; an existing server keeps its name"

  # System user and group.
  if ! getent group "$SVC_USER" >/dev/null 2>&1; then
    sys groupadd --system "$SVC_USER"
  fi
  if ! id -u "$SVC_USER" >/dev/null 2>&1; then
    sys useradd --system --gid "$SVC_USER" --no-create-home --home-dir /nonexistent \
      --shell /usr/sbin/nologin "$SVC_USER"
  fi

  install_binary "$BINARY"
  info "Installed:   /usr/local/bin/uzume-server ($(binary_version "$BIN_DEST"))"

  # Config: keep an existing file, only touch explicitly given keys.
  install -d -m 0750 "$ENV_DIR"
  sys chown "root:$SVC_USER" "$ENV_DIR"
  if [ ! -f "$ENV_FILE" ]; then
    ( umask 027
      {
        echo "# Uzume server configuration (read by systemd, see uzume-server.service)."
        echo "# UZUME_NAME is only used on the first start."
        echo "UZUME_LISTEN=$listen"
        echo "UZUME_DATA_DIR=$data_dir"
        echo "# Sessions over the internet (relay server, see the README): uncomment and set your DNS name."
        echo "#UZUME_TURN=1"
        echo "#UZUME_PUBLIC_HOST=hub.example.org"
        [ -z "$NAME" ] || echo "UZUME_NAME=$(quote_name "$NAME")"
      } >"$ENV_FILE" )
    info "Config:      created /etc/uzume/server.env"
  else
    [ -z "$LISTEN" ]   || env_set UZUME_LISTEN "$LISTEN"
    [ -z "$DATA_DIR" ] || env_set UZUME_DATA_DIR "$DATA_DIR"
    [ -z "$NAME" ]     || env_set UZUME_NAME "$(quote_name "$NAME")"
    info "Config:      kept /etc/uzume/server.env (only explicitly given options updated)"
  fi
  chmod 0640 "$ENV_FILE"
  sys chown "root:$SVC_USER" "$ENV_FILE"

  # Data dir: created if missing, ownership fixed, content never touched otherwise.
  install -d -m 0750 "$P$data_dir"
  sys chown -R "$SVC_USER:$SVC_USER" "$P$data_dir"
  info "Data dir:    $data_dir"

  # Unit + drop-in.
  local unit_src="$SCRIPT_DIR/$SVC.service"
  [ -f "$unit_src" ] || unit_src="./$SVC.service"
  [ -f "$unit_src" ] || die "$SVC.service not found next to the script"
  install -d -m 0755 "$(dirname "$UNIT_DEST")"
  install -m 0644 "$unit_src" "$UNIT_DEST"
  write_dropin "$listen" "$data_dir"
  install_notices

  [ -z "$ADMIN" ] || create_admin "$data_dir"

  sys systemctl daemon-reload
  if [ "$NO_START" -eq 1 ] || testmode; then
    info "Service installed but not started (--no-start). Start: systemctl enable --now $SVC"
  else
    warn_port_check "${listen##*:}"
    systemctl enable --now "$SVC"
    for _ in 1 2 3 4 5 6 7 8 9 10; do
      sleep 1
      systemctl is-active --quiet "$SVC" && break
    done
    if systemctl is-active --quiet "$SVC"; then info "Status:      active"
    else
      echo "Status:      NOT active, see: journalctl -u $SVC -e" >&2
      exit 1
    fi
    print_url "$listen"
    print_fingerprint
  fi
  if [ "$NO_START" -eq 1 ] || testmode; then warn_port_check "${listen##*:}"; fi
  [ "$ADMIN_FAILED" -eq 0 ] || exit 1
}

cmd_upgrade() {
  [ -n "$BINARY" ] || die "upgrade needs --binary PATH"
  find_binary
  check_binary_runs
  local old new
  old="none"
  [ ! -x "$BIN_DEST" ] || old="$(binary_version "$BIN_DEST")"
  install_binary "$BINARY"
  new="$(binary_version "$BIN_DEST")"
  if testmode; then
    info "[dry-run] systemctl restart $SVC"
  elif systemctl is-active --quiet "$SVC"; then
    systemctl restart "$SVC"
  else
    info "Service is not running; not restarted."
  fi
  info "Version:     $old -> $new"
}

cmd_uninstall() {
  local data_dir
  data_dir="$(effective_data_dir)"
  [ "$PURGE" -ne 1 ] || validate_data_dir "$data_dir"
  if testmode; then
    echo "[dry-run] systemctl disable --now $SVC"
  else
    systemctl disable --now "$SVC" >/dev/null 2>&1 || true
  fi
  rm -f "$UNIT_DEST" "$DROPIN_FILE" "$BIN_DEST"
  rm -rf "$P/usr/local/share/doc/uzume-server"
  rmdir "$DROPIN_DIR" 2>/dev/null || true
  sys systemctl daemon-reload
  info "Removed:     service unit and /usr/local/bin/uzume-server"
  if [ "$PURGE" -eq 1 ]; then
    if [ "$YES" -ne 1 ]; then
      echo "This permanently deletes /etc/uzume and the data dir $data_dir (database, saves, TLS keys)."
      local ans=""
      read -r -p "Type 'yes' to continue: " ans || true
      [ "$ans" = yes ] || die "aborted, nothing purged"
    fi
    rm -rf "$P$data_dir" "$ENV_DIR"
    info "Purged:      /etc/uzume and $data_dir"
    info "Kept:        system user/group '$SVC_USER' (remove with: userdel $SVC_USER)"
  else
    info "Kept:        /etc/uzume and $data_dir (delete with: uninstall --purge)"
    info "Kept:        system user/group '$SVC_USER'"
  fi
}

cmd_renew_cert() {
  local dir
  [ -f "$ENV_FILE" ] || testmode || die "$ENV_FILE not found; is the server installed?"
  if [ -n "$(env_get UZUME_TLS_CERT)" ] || [ -n "$(env_get UZUME_TLS_KEY)" ]; then
    die "an own TLS certificate/key is configured in $ENV_FILE; the server never renews it, renew it where it comes from"
  fi
  dir="$(effective_data_dir)"
  if testmode; then
    info "[dry-run] runuser -u $SVC_USER -- env UZUME_DATA_DIR=$dir $BIN_DEST renew-cert"
    info "[dry-run] systemctl restart $SVC"
    return 0
  fi
  [ -x "$BIN_DEST" ] || die "$BIN_DEST not found; is the server installed?"
  runuser -u "$SVC_USER" -- env "UZUME_DATA_DIR=$dir" "$BIN_DEST" renew-cert \
    || die "renew-cert failed; the service was not restarted"
  systemctl restart "$SVC"
  info "Service restarted. Players must confirm the new fingerprint."
}

cmd_import_cores() {
  local dir tmp
  [ -n "$IMPORT_DIR" ] || die "import-cores needs a directory: install-server.sh import-cores DIR"
  [ -d "$IMPORT_DIR" ] || die "not a directory: $IMPORT_DIR"
  [ -f "$ENV_FILE" ] || testmode || die "$ENV_FILE not found; is the server installed?"
  dir="$(effective_data_dir)"
  if testmode; then
    info "[dry-run] cp -a $IMPORT_DIR/. <tmp> (readable by $SVC_USER)"
    info "[dry-run] runuser -u $SVC_USER -- env UZUME_DATA_DIR=$dir $BIN_DEST import-cores <tmp>"
    return 0
  fi
  [ -x "$BIN_DEST" ] || die "$BIN_DEST not found; is the server installed?"
  # The service user may not be able to read DIR (e.g. under /root): work on a world-readable copy.
  tmp="$(mktemp -d)" || die "cannot create a temporary directory"
  chmod 755 "$tmp"
  cp -a "$IMPORT_DIR"/. "$tmp"/ || { rm -rf "$tmp"; die "cannot copy $IMPORT_DIR"; }
  chmod -R a+rX "$tmp"
  local rc=0
  runuser -u "$SVC_USER" -- env "UZUME_DATA_DIR=$dir" "$BIN_DEST" import-cores "$tmp" || rc=$?
  rm -rf "$tmp"
  [ "$rc" = 0 ] || die "import-cores failed"
}

cmd_status() {
  if testmode; then
    echo "[dry-run] systemctl status $SVC"
  else
    systemctl --no-pager --lines=5 status "$SVC" || true
  fi
  if [ -f "$ENV_FILE" ] && [ ! -r "$ENV_FILE" ]; then
    info "URL:         unknown, run with sudo to show the configured URL"
  else
    print_url
  fi
}

case "$CMD" in
  install)   cmd_install ;;
  upgrade)   cmd_upgrade ;;
  uninstall) cmd_uninstall ;;
  renew-cert) cmd_renew_cert ;;
  import-cores) cmd_import_cores ;;
  status)    cmd_status ;;
esac
