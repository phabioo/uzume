package main

import (
	"bytes"
	"crypto/ed25519"
	"encoding/base64"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/phabioo/uzume/server/internal/pluginpkg"
)

// A published id+version is immutable: plugin-add refuses a different sha256 unless -replace is given.
func TestPluginAddRefusesDifferentFileForSameVersion(t *testing.T) {
	dir := t.TempDir()
	now := func() time.Time { return time.Date(2026, 10, 12, 8, 0, 0, 0, time.UTC) }
	seed := base64.StdEncoding.EncodeToString(make([]byte, ed25519.SeedSize))
	env := map[string]string{signingKeyEnv: seed}
	var out bytes.Buffer
	runE := func(args ...string) error {
		out.Reset()
		return run(args, func(k string) string { return env[k] }, &out, now)
	}
	signZip := func(name string, extra map[string]string) string {
		raw, signed := filepath.Join(dir, name+".raw"), filepath.Join(dir, name+".uzplugin")
		os.WriteFile(raw, pluginZip(t, "1.2.0", extra), 0o644)
		if err := runE("plugin-sign", "-in", raw, "-out", signed); err != nil {
			t.Fatal(err)
		}
		return signed
	}
	idxFile := filepath.Join(dir, "plugin-index.json")
	url := "https://example.org/tracker-1.2.0.uzplugin"
	a, b := signZip("a", nil), signZip("b", map[string]string{"extra.txt": "different"})
	if err := runE("plugin-add", "-index", idxFile, "-plugin", a, "-url", url); err != nil {
		t.Fatal(err)
	}
	before, _ := os.ReadFile(idxFile)
	// The same file again is fine (idempotent re-run).
	if err := runE("plugin-add", "-index", idxFile, "-plugin", a, "-url", url); err != nil {
		t.Fatal(err)
	}
	// A different file under the same version is refused and the index stays untouched.
	err := runE("plugin-add", "-index", idxFile, "-plugin", b, "-url", url)
	if err == nil || !strings.Contains(err.Error(), "immutable") {
		t.Fatalf("different sha256 accepted: %v", err)
	}
	if after, _ := os.ReadFile(idxFile); !bytes.Equal(before, after) {
		t.Fatal("index changed by a refused plugin-add")
	}
	// -replace allows it with a warning.
	prev := warnOut
	var warn bytes.Buffer
	warnOut = &warn
	defer func() { warnOut = prev }()
	if err := runE("plugin-add", "-index", idxFile, "-plugin", b, "-url", url, "-replace"); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(warn.String(), "replacing") {
		t.Fatalf("no warning: %q", warn.String())
	}
	raw, _ := os.ReadFile(idxFile)
	idx, _ := pluginpkg.ParseIndex(raw, false)
	bd, _ := os.ReadFile(b)
	if len(idx.Plugins) != 1 || idx.Plugins[0].SHA256 != pluginpkg.SHA256Hex(bd) {
		t.Fatalf("index %+v", idx.Plugins)
	}
}
