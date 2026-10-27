package main

import (
	"encoding/base64"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/phabioo/uzume/server/internal/hub/hubtest"
)

// plain.fbplugin is the legacy extension, still accepted as input.
func TestImportPlugins(t *testing.T) {
	in, data := t.TempDir(), t.TempDir()
	os.WriteFile(filepath.Join(in, "signed.uzplugin"), hubtest.SignPlugin(t, hubtest.PluginZip(t, "net.example.signed", "1.0.0", []string{"ui"}, nil)), 0o644)
	os.WriteFile(filepath.Join(in, "plain.fbplugin"), hubtest.PluginZip(t, "net.example.plain", "1.0.0", nil, nil), 0o644)
	trust := base64.StdEncoding.EncodeToString(hubtest.PluginKeys()[0])

	var out strings.Builder
	// The directory may come before or after the flags; the extra trusted key makes the signed file official.
	if err := runImportPlugins([]string{in, "-data-dir", data, "-core-trust-key", trust}, &out); err != nil {
		t.Fatalf("%v %q", err, out.String())
	}
	if !strings.Contains(out.String(), "2 added, 0 unchanged, 0 problem(s)") {
		t.Fatalf("summary %q", out.String())
	}
	for _, f := range []string{"net.example.signed/1.0.0.uzplugin", "net.example.plain/1.0.0.uzplugin"} {
		if _, err := os.Stat(filepath.Join(data, "plugins", filepath.FromSlash(f))); err != nil {
			t.Fatal(err)
		}
	}
	// Again: nothing changes. Without the extra key the signed file is imported as unverified (already offered: unchanged).
	out.Reset()
	if err := runImportPlugins([]string{"-data-dir", data, in}, &out); err != nil || !strings.Contains(out.String(), "0 added, 2 unchanged") {
		t.Fatalf("%v %q", err, out.String())
	}
	// A broken file is reported and makes the command fail; the others are still imported.
	os.WriteFile(filepath.Join(in, "broken.uzplugin"), []byte("nope"), 0o644)
	os.WriteFile(filepath.Join(in, "new.uzplugin"), hubtest.PluginZip(t, "net.example.new", "2.0.0", nil, nil), 0o644)
	out.Reset()
	err := runImportPlugins([]string{in, "-data-dir", data}, &out)
	if err == nil || !strings.Contains(out.String(), "1 added, 2 unchanged, 1 problem(s)") || !strings.Contains(out.String(), "Rejected: broken.uzplugin") {
		t.Fatalf("err %v out %q", err, out.String())
	}
	if err := runImportPlugins([]string{"-data-dir", data}, &out); err == nil {
		t.Fatal("missing directory accepted")
	}
	if err := runImportPlugins([]string{filepath.Join(in, "nope"), "-data-dir", data}, &out); err == nil {
		t.Fatal("unreadable directory accepted")
	}
	if err := runImportPlugins([]string{in, "-data-dir", data, "-plugin-index-url", "http://x/index.json"}, &out); err == nil {
		t.Fatal("http plugin index url accepted")
	}
}
