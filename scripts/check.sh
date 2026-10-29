#!/usr/bin/env bash
# Quiet check steps: output buffered, on failure only the last 40 lines.
# Usage: scripts/check.sh hub-fmt|hub-vet|hub-staticcheck|hub-test|hub-race|hub-codegen|hub-notices|script-tests|hub-build|hub-deb|app-appimage|generate|client|packaging|hub|all
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export CGO_ENABLED=0 GOTOOLCHAIN=local
export VCPKG_ROOT="${VCPKG_ROOT:-$HOME/.cache/uzume/vcpkg}"
status=0

# step <label> <command...>: runs it, reports "ok  label" or "FAIL label" + log tail.
step() {
  local label="$1"; shift
  local log; log="$(mktemp)"
  if "$@" >"$log" 2>&1; then
    echo "ok  $label"
  else
    echo "FAIL $label"
    # Go test failures and race reports can sit far above the tail (log noise of later tests): show them first.
    grep -E -A12 -- '^(--- FAIL|WARNING: DATA RACE|panic:)' "$log" | head -n 120
    tail -n "${STEP_TAIL:-40}" "$log"
    status=1
  fi
  rm -f "$log"
}

hub_fmt() {
  local out; out="$(cd "$ROOT/server" && gofmt -l .)" || return 1
  [ -z "$out" ] || { echo "not gofmt-formatted:"; echo "$out"; return 1; }
}
hub_vet()  { (cd "$ROOT/server" && go vet ./...); }
STATICCHECK_VERSION="2025.1.1"
# Pinned staticcheck (installed into GOBIN/GOPATH on demand); findings in server/internal/api/api.gen.go are excluded.
hub_staticcheck() {
  local bin
  bin="$(go env GOBIN)"; [ -n "$bin" ] || bin="$(go env GOPATH)/bin"
  if ! "$bin/staticcheck" -version 2>/dev/null | grep -q "$STATICCHECK_VERSION"; then
    go install "honnef.co/go/tools/cmd/staticcheck@$STATICCHECK_VERSION" || return 1
  fi
  local out rc=0
  out="$(cd "$ROOT/server" && "$bin/staticcheck" ./... 2>&1)" || rc=$?
  # rc 1 = findings; drop those in the generated api.gen.go (no "Code generated" header). Other output (rc >= 2, build errors) stays.
  out="$(printf '%s\n' "$out" | grep -v 'api\.gen\.go:' || true)"
  if [ "$rc" -ge 2 ] || [ -n "$(printf '%s' "$out" | tr -d '[:space:]')" ]; then printf '%s\n' "$out"; return 1; fi
}
hub_test() { (cd "$ROOT/server" && go test ./...); }
# hub-race: the whole Hub test suite under the race detector (needs cgo and a C compiler; the Linux CI job runs it, CI-4).
# Not part of `make check-hub` (slower, needs gcc); run it with `make check-hub-race`.
hub_race() { (cd "$ROOT/server" && CGO_ENABLED=1 go test -race ./...); }
generate() { (cd "$ROOT/server" && go generate ./...); }
hub_codegen() {
  local before after
  before="$(cd "$ROOT/server/internal/api" && cat ./*.gen.go | sha256sum)"
  generate || return 1
  after="$(cd "$ROOT/server/internal/api" && cat ./*.gen.go | sha256sum)"
  [ "$before" = "$after" ] || { echo "Generated code is stale: run 'make generate' and commit"; return 1; }
  (cd "$ROOT/server" && go mod tidy -diff) || { echo "go.mod/go.sum not tidy: run 'go mod tidy'"; return 1; }
}
# hub-notices: server/THIRD-PARTY-NOTICES.txt matches the compiled-in Go modules (regenerate: make notices).
hub_notices() { "$ROOT/scripts/gen-hub-notices.sh" --check; }
# script-tests: unit tests of the Python helper scripts (scripts/tests, stdlib unittest; changelog.py).
script_tests() { (cd "$ROOT" && python3 -I -m unittest discover -s scripts/tests); }
hub_build() {
  local target goos arch ext
  mkdir -p "$ROOT/server/dist"
  # CGO is off (pure-Go sqlite), so the Windows binary cross-compiles on Linux.
  for target in linux/amd64 linux/arm64 windows/amd64; do
    goos="${target%/*}"; arch="${target#*/}"; ext=""
    [ "$goos" = windows ] && ext=".exe"
    (cd "$ROOT/server" && GOOS="$goos" GOARCH="$arch" go build -trimpath \
      -ldflags "-s -w -X github.com/phabioo/uzume/server/internal/version.Version=${HUB_VERSION:-dev} -X github.com/phabioo/uzume/server/internal/version.Channel=${HUB_CHANNEL:-dev} -X github.com/phabioo/uzume/server/internal/version.Commit=${HUB_COMMIT:-}" \
      -o "dist/uzume-server-$goos-$arch$ext" ./cmd/uzume-server) || return 1
  done
}
# hub-deb: .deb packages from the binaries in server/dist (make build-hub first), version = HUB_VERSION.
hub_deb() {
  local arch v="${HUB_VERSION:-}"
  [ -n "$v" ] || { echo "HUB_VERSION is required (X.Y.Z[-pre])"; return 1; }
  for arch in amd64 arm64; do
    "$ROOT/packaging/linux/build-deb.sh" --binary "$ROOT/server/dist/uzume-server-linux-$arch" \
      --arch "$arch" --version "$v" --out "$ROOT/server/dist" || return 1
  done
}
# app-appimage: the app AppImage from a finished linux-release client build (APP_VERSION required; see the Makefile).
app_appimage() {
  local v="${APP_VERSION:-}"
  [ -n "$v" ] || { echo "APP_VERSION is required (X.Y.Z[-pre])"; return 1; }
  local ddc sdl
  ddc="$("$ROOT/scripts/fetch-libdatachannel.sh" 2>/dev/null)" || return 1
  sdl="$("$ROOT/scripts/fetch-sdl3.sh" 2>/dev/null)" || return 1
  UZUME_EXTRA_LIB_DIRS="$ddc/lib:$sdl/lib:$ddc/lib64:$sdl/lib64" \
    "$ROOT/packaging/linux/appimage/build-appimage.sh" \
      --build-dir "${APP_BUILD_DIR:-$ROOT/client/build/linux-release}" \
      --server "${APP_SERVER:-$ROOT/server/dist/uzume-server-linux-amd64}" \
      --version "$v" --out "${APP_OUT:-$ROOT/client/build/appimage}"
}
CORE_PATH=""
client_core() { CORE_PATH="$("$ROOT/scripts/fetch-buildbot-core.sh" melondsds)" && [ -f "$CORE_PATH" ]; }
DESMUME_PATH=""
client_desmume() { DESMUME_PATH="$("$ROOT/scripts/fetch-buildbot-core.sh" desmume)" && [ -f "$DESMUME_PATH" ]; }
DDC_PATH=""
client_libdatachannel() { DDC_PATH="$("$ROOT/scripts/fetch-libdatachannel.sh")" && [ -d "$DDC_PATH" ]; }
SDL3_PATH=""
client_sdl3() { SDL3_PATH="$("$ROOT/scripts/fetch-sdl3.sh")" && [ -d "$SDL3_PATH" ]; }
# client_diag FILE: failed tests and the QtTest assertions of a ctest run.
client_diag() {
  grep -E '^\s*[0-9]+/[0-9]+ Test\s+#[0-9]+: .*\*\*\*|tests failed|The following tests FAILED|^\s+[0-9]+ - ' "$1" | head -n 20
  echo "--- failed test output (FAIL!/QFATAL/crash, context) ---"
  grep -E -B2 -A4 'FAIL!|QFATAL|Exception|Segmentation' "$1" | head -n 80
  echo "--- warnings (QWARN, context) ---"
  grep -E -B2 -A4 'QWARN' "$1" | head -n 60
}
# UZUME_CTEST_REGEX (optional): ctest -R filter. ctest with the output of failed tests (QtTest FAIL!/QWARN/QFATAL with context) so the assertion is not lost.
# Tests run in parallel (CTEST_PARALLEL_LEVEL, default: number of CPUs); every test uses its own temporary directories and
# fake Hub port, CTEST_PARALLEL_LEVEL=1 restores the serial run.
# UZUME_CTEST_RETRY=1 (CI): a failed run is repeated once for the failed tests only (serial). Tests that pass on the retry are
# reported as flaky (GitHub warning annotation, names appended to UZUME_FLAKY_FILE) and do not fail the check; a second failure does.
client_test() {
  local p="$1" out rc=0
  out="$(mktemp)"
  local -a sel=()
  [ -z "${UZUME_CTEST_REGEX:-}" ] || sel=(-R "$UZUME_CTEST_REGEX")  # nightly: only the real-core tests
  local jobs="${CTEST_PARALLEL_LEVEL:-$(nproc 2>/dev/null || echo 2)}"
  (cd "$ROOT/client" && ctest --preset "$p" --output-on-failure -j "$jobs" "${sel[@]}" >"$out" 2>&1) || rc=$?
  if [ "$rc" -ne 0 ]; then
    client_diag "$out"
  fi
  if [ "$rc" -ne 0 ] && [ -n "${UZUME_CTEST_RETRY:-}" ]; then
    local retry names name rrc=0
    names="$(sed -nE 's/^[[:space:]]*[0-9]+\/[0-9]+ Test[[:space:]]+#[0-9]+: ([^ ]+) \.+\*\*\*(Failed|Exception|Timeout).*/\1/p' "$out" | sort -u)"
    if [ -n "$names" ]; then
      echo "--- retrying failed tests once: $(echo "$names" | tr '\n' ' ')---"
      retry="$(mktemp)"
      (cd "$ROOT/client" && ctest --preset "$p" --output-on-failure --rerun-failed >"$retry" 2>&1) || rrc=$?
      if [ "$rrc" -eq 0 ]; then
        echo "flaky (failed once, passed on retry): $(echo "$names" | tr '\n' ' ')"
        for name in $names; do
          [ "${GITHUB_ACTIONS:-}" != true ] || echo "::warning title=Flaky test::$name failed once and passed on retry"
          [ -z "${UZUME_FLAKY_FILE:-}" ] || echo "$name" >>"$UZUME_FLAKY_FILE"
        done
        rc=0
      else
        echo "--- retry failed too ---"
        client_diag "$retry"
      fi
      rm -f "$retry"
    fi
  fi
  # Nightly (UZUME_REQUIRE_CORES): the real-core tests must run, a skip counts as a failure.
  if [ "$rc" -eq 0 ] && [ -n "${UZUME_REQUIRE_CORES:-}" ] \
     && grep -E 'Test\s+#[0-9]+: emulation_core(_desmume)?\s.*(Skipped|Not Run)' "$out"; then
    echo "core test skipped although the cores are required (UZUME_REQUIRE_CORES)"; rc=1
  fi
  # Merge CI (UZUME_REQUIRE_TESTCORE): the in-repo test core tests must run (emulation_core_gl_testcore needs GL, which xvfb provides).
  if [ "$rc" -eq 0 ] && [ -n "${UZUME_REQUIRE_TESTCORE:-}" ] \
     && grep -E 'Test\s+#[0-9]+: (emulation_test_core\S*|\S+_testcore)\s.*(Skipped|Not Run)' "$out"; then
    echo "test-core test skipped (UZUME_REQUIRE_TESTCORE)"; rc=1
  fi
  rm -f "$out"
  return "$rc"
}
client() {
  local p="${CLIENT_PRESET:-linux-debug}"
  local -a core_arg=()
  # libdatachannel (pinned, cached under ~/.cache/uzume/deps): mandatory, built on first use.
  step "client: libdatachannel" client_libdatachannel
  [ -n "$DDC_PATH" ] && [ -d "$DDC_PATH" ] || return 1
  # SDL3 (pinned, gamepad only, cached under ~/.cache/uzume/deps): mandatory, built on first use.
  step "client: sdl3" client_sdl3
  [ -n "$SDL3_PATH" ] && [ -d "$SDL3_PATH" ] || return 1
  core_arg=("-DCMAKE_PREFIX_PATH=$DDC_PATH" "-DSDL3_ROOT=$SDL3_PATH")
  # Version and channel of the build (CI computes them once, spec 0.3 S1); unset: CMake defaults (<VERSION>-dev).
  [ -z "${UZUME_VERSION:-}" ] || core_arg+=("-DUZUME_VERSION=$UZUME_VERSION")
  [ -z "${UZUME_CHANNEL:-}" ] || core_arg+=("-DUZUME_CHANNEL=$UZUME_CHANNEL")
  [ -z "${UZUME_COMMIT:-}" ] || core_arg+=("-DUZUME_COMMIT=$UZUME_COMMIT")
  # Real cores come from the libretro buildbot (ADR 0020 D10). UZUME_SKIP_CORES=1 (merge CI) fetches none: the *_testcore
  # tests use the in-repo test core and the real-core tests skip. Otherwise (local, nightly) a failed fetch is a warning and the
  # real-core tests skip; UZUME_REQUIRE_CORES=1 (nightly) turns a failed fetch into a failure.
  if [ -z "${UZUME_SKIP_CORES:-}" ]; then
    if client_core 2>/dev/null; then
      core_arg+=("-DUZUME_MELONDS_DS_CORE=$CORE_PATH"); echo "ok  client: melonDS DS core"
    elif [ -n "${UZUME_REQUIRE_CORES:-}" ]; then
      echo "FAIL client: melonDS DS core (buildbot fetch failed; UZUME_REQUIRE_CORES is set)"; "$ROOT/scripts/fetch-buildbot-core.sh" melondsds 2>&1 | tail -n 5; return 1
    else
      echo "warn client: melonDS DS core not fetched (buildbot unreachable?), its tests will be skipped"
    fi
    if client_desmume 2>/dev/null; then
      core_arg+=("-DUZUME_DESMUME_CORE=$DESMUME_PATH"); echo "ok  client: desmume core"
    elif [ -n "${UZUME_REQUIRE_CORES:-}" ]; then
      echo "FAIL client: desmume core (buildbot fetch failed; UZUME_REQUIRE_CORES is set)"; "$ROOT/scripts/fetch-buildbot-core.sh" desmume 2>&1 | tail -n 5; return 1
    else
      echo "warn client: desmume core not fetched (buildbot unreachable?), DeSmuME tests will be skipped"
    fi
  else
    core_arg+=("-DUZUME_MELONDS_DS_CORE=" "-DUZUME_DESMUME_CORE=")  # clear paths cached by an earlier configure
    echo "ok  client: real cores skipped (UZUME_SKIP_CORES), test core only"
  fi
  step "client: configure" bash -c "cd '$ROOT/client' && cmake --preset $p ${core_arg[*]:-}"
  step "client: build"     bash -c "cd '$ROOT/client' && cmake --build --preset $p"
  STEP_TAIL=150 step "client: test" client_test "$p"
}

# packaging_deb TMP: builds the server .deb for both architectures from a dummy binary and checks control and contents;
# the server/update units are verified when systemd-analyze exists.
packaging_deb() {
  local tmp="$1" dir="$ROOT/packaging/linux" arch deb rc=0 listing info
  for arch in amd64 arm64; do
    deb="$("$dir/build-deb.sh" --binary "$tmp/dummy-server" --arch "$arch" --version 0.3.0-beta.7 --out "$tmp/deb")" \
      || { echo "build-deb.sh failed for $arch"; return 1; }
    [ "$(basename "$deb")" = "uzume-server_0.3.0-beta.7_${arch}.deb" ] || { echo "unexpected deb name: $deb"; rc=1; }
    info="$(dpkg-deb -f "$deb")"
    grep -qx 'Package: uzume-server' <<<"$info" || { echo "$arch: control lacks Package"; rc=1; }
    grep -qx 'Version: 0.3.0~beta.7' <<<"$info" || { echo "$arch: control Version is not 0.3.0~beta.7"; rc=1; }
    grep -qx "Architecture: $arch" <<<"$info" || { echo "$arch: control Architecture wrong"; rc=1; }
    listing="$(dpkg-deb -c "$deb")"
    local p
    for p in ./usr/bin/uzume-server ./lib/systemd/system/uzume-server.service \
             ./lib/systemd/system/uzume-server-update.path ./lib/systemd/system/uzume-server-update.service \
             ./usr/share/doc/uzume-server/copyright ./usr/share/doc/uzume-server/THIRD-PARTY-NOTICES.txt; do
      grep -q " $p\$" <<<"$listing" || { echo "$arch: $p missing from the package"; rc=1; }
    done
    # Nothing outside usr/bin, usr/share/doc/uzume-server and the unit directory: no data, no config.
    if grep -qiE '_libretro\.|/cores(/|$)' <<<"$listing"; then echo "$arch: emulator cores must not be part of the package"; rc=1; fi
    if grep -E ' \./' <<<"$listing" | sed 's/ -> .*//' | grep -vE ' \./(usr/(bin/(uzume-server)?|share/(doc/(uzume-server/(copyright|THIRD-PARTY-NOTICES\.txt)?)?)?)?|lib/(systemd/(system/(uzume-server[a-z.-]*)?)?)?)?$' | grep -q .; then
      echo "$arch: unexpected paths in the package"; rc=1
    fi
    local x="$tmp/x-$arch"
    mkdir -p "$x/ctrl"; dpkg-deb -R "$deb" "$x/pkg" >/dev/null
    grep -qx 'ExecStart=/usr/bin/uzume-server' "$x/pkg/lib/systemd/system/uzume-server.service" || { echo "$arch: unit ExecStart"; rc=1; }
    grep -qx 'PathExists=/run/uzume/update-request' "$x/pkg/lib/systemd/system/uzume-server-update.path" || { echo "$arch: path unit"; rc=1; }
    grep -qx 'ExecStart=/usr/bin/uzume-server update apply-staged' "$x/pkg/lib/systemd/system/uzume-server-update.service" || { echo "$arch: update unit"; rc=1; }
    for p in postinst prerm postrm; do [ -x "$x/pkg/DEBIAN/$p" ] || { echo "$arch: $p not executable"; rc=1; }; done
  done
  if command -v actionlint >/dev/null 2>&1; then
    actionlint "$ROOT"/.github/workflows/*.yml || rc=1
  else
    echo "skip actionlint (not installed)"
  fi
  if command -v systemd-analyze >/dev/null 2>&1; then
    local u="$tmp/units"; mkdir -p "$u"
    dpkg-deb -x "$deb" "$tmp/xunits"
    cp "$tmp"/xunits/lib/systemd/system/*.* "$u/"
    sed -i 's#^ExecStart=.*#ExecStart=/bin/true#; s#^User=.*#User=root#; s#^Group=.*#Group=root#; /^EnvironmentFile=/d' "$u"/*.service
    systemd-analyze verify "$u"/uzume-server.service "$u"/uzume-server-update.service "$u"/uzume-server-update.path >"$tmp/verify2.log" 2>&1 || true
    if grep -E "$u/" "$tmp/verify2.log" | grep -viE 'not-found|Failed to (connect|create|open)|No such file' | grep -q .; then
      grep -E "$u/" "$tmp/verify2.log" | head -n 5; rc=1
    fi
  fi
  return "$rc"
}

# packaging: syntax, shellcheck, unit verification and an install/uninstall smoke test.
packaging() {
  local dir="$ROOT/packaging/linux" tmp rc=0
  tmp="$(mktemp -d)"
  local f
  for f in "$dir/install-server.sh" "$dir/build-deb.sh" "$dir"/deb/* "$ROOT/scripts/gen-hub-notices.sh" "$ROOT/scripts/check-trusted-keys.sh" "$ROOT/scripts/e2e-hub-update.sh" "$ROOT/scripts/next-beta-version.sh" "$ROOT/scripts/update-index.sh" "$ROOT/scripts/fetch-buildbot-core.sh" "$ROOT/scripts/check-vcpkg-pin.sh" "$ROOT/scripts/bootstrap-vcpkg.sh"; do
    bash -n "$f" || rc=1
  done
  # Beta version numbering of the CI version job (ADR 0011).
  "$ROOT/scripts/next-beta-version.sh" --self-test >"$tmp/beta-version.log" 2>&1 || { cat "$tmp/beta-version.log"; rc=1; }
  if command -v shellcheck >/dev/null 2>&1; then
    shellcheck "$dir/install-server.sh" "$dir/build-deb.sh" "$dir/appimage/build-appimage.sh" "$ROOT/scripts/gen-hub-notices.sh" "$ROOT/scripts/check-trusted-keys.sh" "$ROOT/scripts/e2e-hub-update.sh" || rc=1
    shellcheck -s sh "$dir"/deb/* || rc=1
    # Remaining scripts: warnings and errors only (the e2e scripts use A && ok || die on purpose).
    shellcheck -S warning "$ROOT"/scripts/*.sh "$ROOT/.claude/hooks/session-start.sh" || rc=1
  else
    echo "skip shellcheck (not installed)"
  fi
  if command -v systemd-analyze >/dev/null 2>&1; then
    sed 's#^ExecStart=.*#ExecStart=/bin/true#; s#^User=.*#User=root#; s#^Group=.*#Group=root#' \
      "$dir/uzume-server.service" >"$tmp/uzume-server.service"
    # Complaints naming the temp unit are real and fail; anything else (e.g. no
    # systemd manager in a container) is tolerated.
    if ! systemd-analyze verify "$tmp/uzume-server.service" >"$tmp/verify.log" 2>&1 \
       && grep -q "$tmp/uzume-server.service" "$tmp/verify.log"; then
      grep "$tmp/uzume-server.service" "$tmp/verify.log" | head -n 5
      rc=1
    fi
  else
    echo "skip systemd-analyze (not installed)"
  fi
  printf '#!/bin/sh\necho "uzume-server 0.0.0-smoke"\n' >"$tmp/dummy-server"
  chmod +x "$tmp/dummy-server"
  packaging_deb "$tmp" || rc=1
  "$ROOT/scripts/check-trusted-keys.sh" || rc=1
  "$ROOT/scripts/check-vcpkg-pin.sh" >/dev/null || rc=1
  local root="$tmp/root"
  UZUME_INSTALL_ROOT="$root" "$dir/install-server.sh" install --binary "$tmp/dummy-server" \
    --port 8444 --no-start || rc=1
  local bad
  for bad in /etc /var/lib; do
    if UZUME_INSTALL_ROOT="$tmp/bad" "$dir/install-server.sh" install --binary "$tmp/dummy-server" \
        --data-dir "$bad" --no-start >/dev/null 2>&1; then
      echo "install --data-dir $bad should have failed"; rc=1
    fi
  done
  grep -qx 'UZUME_LISTEN=:8444' "$root/etc/uzume/server.env" \
    || { echo "server.env lacks UZUME_LISTEN=:8444"; rc=1; }
  [ -x "$root/usr/local/bin/uzume-server" ] || { echo "binary not installed"; rc=1; }
  [ -f "$root/etc/systemd/system/uzume-server.service" ] || { echo "unit not installed"; rc=1; }
  [ -f "$root/usr/local/share/doc/uzume-server/THIRD-PARTY-NOTICES.txt" ] || { echo "third-party notices not installed"; rc=1; }
  local rc_out
  rc_out="$(UZUME_INSTALL_ROOT="$root" "$dir/install-server.sh" renew-cert 2>&1)" || { echo "renew-cert dry-run failed"; rc=1; }
  grep -q 'UZUME_DATA_DIR=/var/lib/uzume .*renew-cert' <<<"$rc_out" \
    || { echo "renew-cert dry-run lacks the runuser line"; rc=1; }
  mkdir -p "$tmp/cores-in/linux-x64"; : >"$tmp/cores-in/linux-x64/dummy_libretro.so.zip"
  rc_out="$(UZUME_INSTALL_ROOT="$root" "$dir/install-server.sh" import-cores "$tmp/cores-in" 2>&1)" || { echo "import-cores dry-run failed"; rc=1; }
  grep -q 'UZUME_DATA_DIR=/var/lib/uzume .*import-cores' <<<"$rc_out" \
    || { echo "import-cores dry-run lacks the runuser line"; rc=1; }
  if UZUME_INSTALL_ROOT="$root" "$dir/install-server.sh" import-cores >/dev/null 2>&1; then
    echo "import-cores without a directory must fail"; rc=1
  fi
  printf 'UZUME_TLS_CERT=/x/c.pem\n' >>"$root/etc/uzume/server.env"
  if UZUME_INSTALL_ROOT="$root" "$dir/install-server.sh" renew-cert >/dev/null 2>&1; then
    echo "renew-cert must refuse with UZUME_TLS_CERT set"; rc=1
  fi
  UZUME_INSTALL_ROOT="$root" "$dir/install-server.sh" uninstall --purge --yes || rc=1
  [ ! -e "$root/usr/local/bin/uzume-server" ] && [ ! -e "$root/etc/uzume" ] \
    || { echo "uninstall --purge left files behind"; rc=1; }
  rm -rf "$tmp"
  return "$rc"
}

case "${1:-all}" in
  hub-fmt)     step "hub: fmt" hub_fmt ;;
  hub-vet)     step "hub: vet" hub_vet ;;
  hub-staticcheck) step "hub: staticcheck" hub_staticcheck ;;
  hub-test)    step "hub: test" hub_test ;;
  hub-race)    step "hub: test -race" hub_race ;;
  hub-codegen) step "hub: codegen" hub_codegen ;;
  hub-notices) step "hub: notices" hub_notices ;;
  script-tests) step "scripts: python tests" script_tests ;;
  notices)     "$ROOT/scripts/gen-hub-notices.sh" ;;
  packaging) step "packaging" packaging ;;
  hub)   step "hub: fmt" hub_fmt; step "hub: vet" hub_vet; step "hub: staticcheck" hub_staticcheck; step "hub: test" hub_test; step "hub: codegen" hub_codegen; step "hub: notices" hub_notices; step "scripts: python tests" script_tests; step "packaging" packaging ;;
  hub-deb) step "hub: deb packages" hub_deb ;;
  app-appimage) STEP_TAIL=60 step "app: AppImage" app_appimage ;;
  hub-build) step "hub: build linux/amd64+arm64, windows/amd64" hub_build ;;
  generate)  step "hub: generate" generate ;;
  client)    client ;;
  all)       "$0" hub || status=1; "$0" client || status=1 ;;
  *) echo "unknown step: $1" >&2; exit 2 ;;
esac
exit "$status"
