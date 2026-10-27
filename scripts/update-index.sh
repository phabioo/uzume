#!/usr/bin/env bash
# Adds release descriptions to the signed updates index (release "updates-index"): download the current index, release-add,
# sign, verify (incl. trusted-key check) and upload. Shared by release.yml, promote.yml and index-refresh.yml (ADR 0011).
#   update-index.sh <uzume-sign> <hub.json> [<player.json> ...]
#   update-index.sh <uzume-sign>            (no release: only new generated_at/expires_at, content unchanged)
# Uploads updates-index.json + .sig (legacy pair, raw-byte signature, read by old Hubs/Players) and updates-index.json.signed
# (one file: domain-separated signature line + the same bytes; read by current clients, cannot be a mismatched pair;
# ADR 0016 amendment 2026-10-19). Ships the bundle first, then the legacy pair.
# Needs gh (GH_TOKEN, GH_REPO) and UZUME_SIGNING_KEY in the environment (never echoed). Workdir: $RUNNER_TEMP/index.
set -euo pipefail

[ $# -ge 1 ] || { echo "usage: $0 <uzume-sign> [<release.json>...]" >&2; exit 2; }
sign="$1"; shift
[ -n "${UZUME_SIGNING_KEY:-}" ] || { echo "::error title=Signing key missing::UZUME_SIGNING_KEY is empty"; exit 1; }
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
idx="${RUNNER_TEMP:-$(mktemp -d)}/index"
rm -rf "$idx"; mkdir -p "$idx"

# Lists the asset names of the updates-index release. Exit 0 = release exists (names on stdout), 44 = release does not
# exist (gh: "release not found", HTTP 404), 1 = any other gh error (network, rate limit, 5xx) after 3 attempts. Only a
# real "not found" may start from an empty index; every other failure must stop the script instead of replacing the index.
index_assets() {
  local i out
  for i in 1 2 3; do
    if out="$(gh release view updates-index --json assets --jq '.assets[].name' 2>"$idx/gh-err.txt")"; then
      printf '%s\n' "$out"
      return 0
    fi
    if grep -qi 'release not found' "$idx/gh-err.txt"; then
      return 44
    fi
    echo "::warning::gh release view updates-index failed (attempt $i/3): $(head -c 300 "$idx/gh-err.txt")" >&2
    [ "$i" -eq 3 ] || sleep $((i * 5))
  done
  return 1
}
rc=0
assets="$(index_assets)" || rc=$?
case "$rc" in
  0) release_exists=true ;;
  44) release_exists=false; assets="" ;;
  *) echo "::error title=Updates index::could not read the updates-index release (not a 'release not found' error); refusing to continue so the index is not replaced" >&2; exit 1 ;;
esac
if [ "$release_exists" = true ] && grep -qx updates-index.json <<<"$assets"; then
  # An existing index must be read successfully (set -e), otherwise it would be silently replaced by a fresh one.
  gh release download updates-index --pattern updates-index.json --dir "$idx"
fi
if [ $# -eq 0 ]; then
  [ -f "$idx/updates-index.json" ] || { echo "::error::refresh needs an existing updates index"; exit 1; }
  "$sign" index-refresh -index "$idx/updates-index.json"
fi
for rel in "$@"; do
  "$sign" release-add -index "$idx/updates-index.json" -release "$rel" -keep 5
done
"$sign" sign -index "$idx/updates-index.json" -out "$idx/updates-index.json.sig" -bundle "$idx/updates-index.json.signed"
pub="$("$sign" pubkey | sed -n 's/^public_key=//p' | head -n 1)"
[ -n "$pub" ] || { echo "::error::could not derive the public key"; exit 1; }
"$sign" verify -index "$idx/updates-index.json" -sig "$idx/updates-index.json.sig" -pub "$pub"
"$sign" verify -index "$idx/updates-index.json" -bundle "$idx/updates-index.json.signed" -pub "$pub"
# Same check as the Hub and Player: the key must be one of the compiled-in trusted keys.
grep -qF "\"$pub\"" "$root/server/internal/corepkg/keys.go" || { echo "::error::signing key is not in DefaultTrustedKeys"; exit 1; }
jq -r '.releases[] | "\(.product) \(.channel) \(.version)"' "$idx/updates-index.json"

if [ "$release_exists" != true ]; then
  gh release create updates-index --title "Uzume update index" \
    --notes "Signed index of Uzume server and app releases (updates-index.json + .sig + .signed). Read by Uzume servers and apps; updated by the release and promote workflows." \
    --latest=false
else
  # An existing release keeps its old title and notes ("FrameBeam update index"); set the current ones.
  gh release edit updates-index --title "Uzume update index" \
    --notes "Signed index of Uzume server and app releases (updates-index.json + .sig + .signed). Read by Uzume servers and apps; updated by the release and promote workflows."
fi
gh release upload updates-index "$idx/updates-index.json.signed" --clobber
gh release upload updates-index "$idx/updates-index.json" "$idx/updates-index.json.sig" --clobber
