package main

import (
	"archive/zip"
	"bytes"
	"crypto/ed25519"
	"encoding/base64"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/phabioo/uzume/server/internal/corepkg"
	"github.com/phabioo/uzume/server/internal/pluginpkg"
)

func pluginZip(t *testing.T, version string, extra map[string]string) []byte {
	t.Helper()
	var b bytes.Buffer
	zw := zip.NewWriter(&b)
	files := map[string]string{
		"manifest.json": `{"api":1,"id":"net.example.tracker","name":"Tracker","version":"` + version + `","entry":"main.lua","permissions":["ui"],"description":"Tracks"}`,
		"main.lua":      "return {}",
	}
	for k, v := range extra {
		files[k] = v
	}
	for _, n := range []string{"manifest.json", "main.lua"} {
		w, _ := zw.Create(n)
		w.Write([]byte(files[n]))
		delete(files, n)
	}
	for n, c := range files {
		w, _ := zw.Create(n)
		w.Write([]byte(c))
	}
	zw.Close()
	return b.Bytes()
}

func TestPluginSignAddAndIndexSign(t *testing.T) {
	dir := t.TempDir()
	fixed := time.Date(2026, 10, 12, 8, 0, 0, 0, time.UTC)
	now := func() time.Time { return fixed }
	var out bytes.Buffer
	seed := base64.StdEncoding.EncodeToString(make([]byte, ed25519.SeedSize))
	env := map[string]string{signingKeyEnv: seed}
	runE := func(env map[string]string, args ...string) error {
		out.Reset()
		return run(args, func(k string) string { return env[k] }, &out, now)
	}
	raw, signed := filepath.Join(dir, "raw.uzplugin"), filepath.Join(dir, "signed.uzplugin")
	os.WriteFile(raw, pluginZip(t, "1.2.0", nil), 0o644)

	if err := runE(nil, "plugin-sign", "-in", raw, "-out", signed); err == nil {
		t.Fatal("plugin-sign without a key succeeded")
	}
	if err := runE(env, "plugin-sign", "-in", raw, "-out", signed); err != nil {
		t.Fatal(err)
	}
	data, _ := os.ReadFile(signed)
	p, err := pluginpkg.Open(data)
	if err != nil {
		t.Fatal(err)
	}
	pubSeed, _ := corepkg.ParseSeed(seed)
	pub, _ := corepkg.PublicFromSeed(pubSeed)
	if !p.Verify([]ed25519.PublicKey{pub}).Official() {
		t.Fatal("signed plugin is not official")
	}
	// An invalid input is refused and no output is written.
	os.WriteFile(raw, []byte("not a zip"), 0o644)
	if err := runE(env, "plugin-sign", "-in", raw, "-out", filepath.Join(dir, "bad.uzplugin")); err == nil {
		t.Fatal("invalid plugin signed")
	}
	if _, err := os.Stat(filepath.Join(dir, "bad.uzplugin")); err == nil {
		t.Fatal("output written for an invalid plugin")
	}

	idxFile := filepath.Join(dir, "plugin-index.json")
	url := "https://example.org/tracker-1.2.0.uzplugin"
	// plugin-add refuses an unsigned plugin ...
	unsigned := filepath.Join(dir, "unsigned.uzplugin")
	os.WriteFile(unsigned, pluginZip(t, "1.2.0", nil), 0o644)
	if err := runE(env, "plugin-add", "-index", idxFile, "-plugin", unsigned, "-url", url); err == nil || !strings.Contains(err.Error(), "not signed") {
		t.Fatalf("unsigned accepted: %v", err)
	}
	// ... a plugin whose content changed after signing ...
	tampered := filepath.Join(dir, "tampered.uzplugin")
	os.WriteFile(tampered, tamper(t, data), 0o644)
	if err := runE(env, "plugin-add", "-index", idxFile, "-plugin", tampered, "-url", url); err == nil || !strings.Contains(err.Error(), "invalid") {
		t.Fatalf("tampered accepted: %v", err)
	}
	// ... a signature of a key that is not trusted (no env key, not the release key) ...
	if err := runE(nil, "plugin-add", "-index", idxFile, "-plugin", signed, "-url", url); err == nil {
		t.Fatal("untrusted signature accepted")
	}
	// ... and a non-https URL.
	if err := runE(env, "plugin-add", "-index", idxFile, "-plugin", signed, "-url", "http://example.org/x"); err == nil {
		t.Fatal("http url accepted")
	}
	if _, err := os.Stat(idxFile); err == nil {
		t.Fatal("index written by failed plugin-add")
	}
	if err := runE(env, "plugin-add", "-index", idxFile, "-plugin", signed, "-url", url); err != nil {
		t.Fatal(err)
	}
	// A second version is added, the first stays; the same version again replaces its entry.
	signed2 := filepath.Join(dir, "signed2.uzplugin")
	os.WriteFile(raw, pluginZip(t, "1.10.0", nil), 0o644)
	if err := runE(env, "plugin-sign", "-in", raw, "-out", signed2); err != nil {
		t.Fatal(err)
	}
	if err := runE(env, "plugin-add", "-index", idxFile, "-plugin", signed2, "-url", "https://example.org/tracker-1.10.0.uzplugin"); err != nil {
		t.Fatal(err)
	}
	if err := runE(env, "plugin-add", "-index", idxFile, "-plugin", signed, "-url", "https://example.org/moved.uzplugin"); err != nil {
		t.Fatal(err)
	}
	b1, _ := os.ReadFile(idxFile)
	idx, errs := pluginpkg.ParseIndex(b1, false)
	if len(errs) != 0 || len(idx.Plugins) != 2 || idx.Plugins[0].Version != "1.2.0" || idx.Plugins[0].URL != "https://example.org/moved.uzplugin" ||
		idx.Plugins[1].Version != "1.10.0" || !idx.GeneratedAt.Equal(fixed) {
		t.Fatalf("index %+v %v", idx, errs)
	}
	e := idx.Plugins[0]
	if e.SHA256 != pluginpkg.SHA256Hex(data) || e.Size != int64(len(data)) || e.Name != "Tracker" || e.API != 1 || len(e.Permissions) != 1 || e.Description != "Tracks" {
		t.Fatalf("entry %+v", e)
	}
	// The existing sign command writes the detached signature of the plugin index.
	if err := runE(env, "sign", "-index", idxFile); err != nil {
		t.Fatal(err)
	}
	pubB64 := base64.StdEncoding.EncodeToString(pub)
	if err := runE(nil, "verify", "-index", idxFile, "-sig", idxFile+".sig", "-pub", pubB64); err != nil {
		t.Fatalf("verify: %v", err)
	}
	// A plugin index with an invalid entry is not signed.
	os.WriteFile(idxFile, bytes.Replace(b1, []byte(`"api": 1`), []byte(`"api": 2`), 1), 0o644)
	if err := runE(env, "sign", "-index", idxFile); err == nil {
		t.Fatal("invalid plugin index signed")
	}
}

func tamper(t *testing.T, data []byte) []byte {
	t.Helper()
	zr, _ := zip.NewReader(bytes.NewReader(data), int64(len(data)))
	var b bytes.Buffer
	zw := zip.NewWriter(&b)
	for _, f := range zr.File {
		rc, _ := f.Open()
		var c bytes.Buffer
		c.ReadFrom(rc)
		rc.Close()
		body := c.Bytes()
		if f.Name == "main.lua" {
			body = []byte("return {evil=true}")
		}
		w, _ := zw.Create(f.Name)
		w.Write(body)
	}
	zw.Close()
	return b.Bytes()
}
