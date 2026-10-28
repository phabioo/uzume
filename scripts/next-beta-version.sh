#!/usr/bin/env bash
# Next beta version for a push to main (ADR 0016, amended by ADR 0036: release line plus running build number).
#   next-beta-version.sh <VERSION-file-content> <commit-sha> < "git ls-remote --tags" output
# Versions are R.B.0: R = release line, B = build number within the line, third field always 0 (reserved).
# VERSION (R.0.0) is the floor and names the line. The result is R.max(1, VERSION minor, highest plain tag vR.B.0 + 1).0.
# If a plain tag vR.B.0 of that line already points at <commit-sha> (re-run of the same commit), that version is reused.
# Suffixed tags (v21.1.0-beta.3), tags of other lines and tags with a third field other than 0 are ignored.
# B is limited to 255 (the MSI ProductVersion minor field); more builds in one line fail with an error: start a new line.
# Prints R.B.0.
#   next-beta-version.sh --self-test
set -euo pipefail

compute() {
  local floor="$1" sha="$2" major minor patch out
  [[ "$floor" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)$ ]] || { echo "VERSION must be R.B.0 (for example 21.0.0), found '$floor'" >&2; return 1; }
  major="${BASH_REMATCH[1]}"; minor="${BASH_REMATCH[2]}"; patch="${BASH_REMATCH[3]}"
  [ "$patch" = 0 ] || { echo "VERSION must end in .0 (third field is reserved), found '$floor'" >&2; return 1; }
  [[ "$sha" =~ ^[0-9a-f]{40}$ ]] || { echo "commit must be a 40 digit sha, found '$sha'" >&2; return 1; }
  out="$(awk -v major="$major" -v floor="$minor" -v sha="$sha" '
    { ref = $2; sub(/\^\{\}$/, "", ref); sub(/^refs\/tags\//, "", ref); tag[ref] = $1 }  # peeled (^{}) lines come later and win
    END {
      max = 0; reuse = -1
      for (t in tag) {
        n = split(t, p, ".")
        if (n != 3 || p[1] != "v" major || p[2] !~ /^[0-9]+$/ || p[3] != "0") continue
        if (p[2] + 0 > max) max = p[2] + 0
        if (tag[t] == sha && p[2] + 0 > reuse) reuse = p[2] + 0
      }
      build = max + 1
      if (floor + 0 > build) build = floor + 0
      if (reuse >= 0) build = reuse
      printf "%s.%d.0\n", major, build
    }')"
  build="${out#*.}"; build="${build%.0}"
  if [ "$build" -gt 255 ]; then
    echo "release line $major has reached build $build (limit 255, MSI ProductVersion): start a new line by raising VERSION to $((major + 1)).0.0" >&2
    return 1
  fi
  printf '%s\n' "$out"
}

self_test() {
  local rc=0 a="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" b="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
  t() { # expected floor sha tags...
    local want="$1" floor="$2" sha="$3" got tags="" x
    shift 3
    for x in "$@"; do tags+="${x%%=*}	refs/tags/${x#*=}"$'\n'; done
    got="$(printf '%s' "$tags" | compute "$floor" "$sha")"
    if [ "$got" = "$want" ]; then echo "ok   $floor + [$*] -> $got"; else echo "FAIL $floor + [$*] -> $got (want $want)"; rc=1; fi
  }
  t 21.1.0  21.0.0 "$a"                                          # no tags: B starts at 1
  t 21.3.0  21.0.0 "$a" "$b=v21.1.0" "$b=v21.2.0"
  t 21.10.0 21.0.0 "$a" "$b=v21.9.0"                              # numeric order
  t 21.12.0 21.0.0 "$a" "$b=v21.2.0" "$b=v21.9.0" "$b=v21.11.0"
  t 21.1.0  21.0.0 "$a" "$b=v0.20.15" "$b=v0.20.3"                # old 0.20.N tags are another line
  t 21.2.0  21.0.0 "$a" "$b=v21.1.0" "$a=v21.2.0"                 # re-run of the same commit reuses its tag
  t 21.2.0  21.0.0 "$a" "$b=v21.1.0" "$b=v21.2.0" "$a=v21.2.0^{}"
  t 21.3.0  21.0.0 "$a" "$b=v21.1.0" "$b=v21.2.0"
  t 22.1.0  22.0.0 "$a" "$b=v21.1.0" "$b=v21.40.0"                # new line restarts at 1
  t 21.2.0  21.0.0 "$a" "$b=v21.1.0" "$b=v21.1.0-beta.3" "$b=v21.70.1" "$b=v21.1.0.1" "$b=v210.9.0" "$b=v22.9.0"
  e() { # floor sha tags...: must fail
    local floor="$1" sha="$2" tags="" x
    shift 2
    for x in "$@"; do tags+="${x%%=*}	refs/tags/${x#*=}"$'\n'; done
    if printf '%s' "$tags" | compute "$floor" "$sha" >/dev/null 2>&1; then echo "FAIL $floor + [$*] should be an error"; rc=1; else echo "ok   $floor + [$*] -> error"; fi
  }
  e 21.0.0 "$a" "$b=v21.255.0"                                    # build 256 does not fit the MSI
  t 21.255.0 21.0.0 "$a" "$b=v21.254.0"                          # 255 is still allowed
  e 21.1.5 "$a"                                                   # third field must be 0
  e 21.0   "$a"
  return "$rc"
}

if [ "${1:-}" = "--self-test" ]; then self_test; exit $?; fi
[ $# -eq 2 ] || { echo "usage: $0 <VERSION> <sha>  (tags on stdin)  |  --self-test" >&2; exit 2; }
compute "$(printf '%s' "$1" | tr -d '[:space:]')" "$2"
