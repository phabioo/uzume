package hubtest

import (
	"archive/zip"
	"bytes"
	"crypto/ed25519"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"testing"
	"time"

	"github.com/phabioo/uzume/server/internal/corepkg"
	"github.com/phabioo/uzume/server/internal/pluginpkg"
	"github.com/phabioo/uzume/server/internal/updates"
)

// PluginSeed is the test signing seed (public test data, never a real key).
func PluginSeed() []byte {
	s := make([]byte, ed25519.SeedSize)
	for i := range s {
		s[i] = 0x40 + byte(i)
	}
	return s
}

// PluginKeys are the public keys to put into hub.Options.CoreTrustKeys so that PluginSeed signatures are official.
func PluginKeys() []ed25519.PublicKey {
	pub, _ := corepkg.PublicFromSeed(PluginSeed())
	return []ed25519.PublicKey{pub}
}

// PluginZip builds an unsigned .uzplugin with a manifest, main.lua and the extra files (path -> content).
func PluginZip(t testing.TB, id, version string, perms []string, extra map[string]string) []byte {
	t.Helper()
	return PluginZipWith(t, id, version, perms, nil, extra)
}

// PluginZipWith is PluginZip plus extra top-level manifest fields (for example "run_settings").
func PluginZipWith(t testing.TB, id, version string, perms []string, fields map[string]any, extra map[string]string) []byte {
	t.Helper()
	m := map[string]any{"api": 1, "id": id, "name": "Plugin " + id, "version": version, "entry": "main.lua", "description": "Test plugin " + id,
		"permissions": perms, "provides": []string{"test.iface"}, "requires": map[string]string{"net.example.base": "^1.0"},
		"games": []map[string]any{{"system": "nds", "codes": []string{"ABC*"}}}}
	for k, v := range fields {
		m[k] = v
	}
	mb, _ := json.Marshal(m)
	var b bytes.Buffer
	zw := zip.NewWriter(&b)
	add := func(name, body string) {
		w, err := zw.CreateHeader(&zip.FileHeader{Name: name, Method: zip.Deflate, Modified: time.Date(2026, 10, 12, 0, 0, 0, 0, time.UTC)})
		if err != nil {
			t.Fatal(err)
		}
		w.Write([]byte(body))
	}
	add("manifest.json", string(mb))
	add("main.lua", "return {}")
	for _, n := range sortedKeys(extra) {
		add(n, extra[n])
	}
	if err := zw.Close(); err != nil {
		t.Fatal(err)
	}
	return b.Bytes()
}

func sortedKeys(m map[string]string) []string {
	var ks []string
	for k := range m {
		ks = append(ks, k)
	}
	sort.Strings(ks)
	return ks
}

// SignPlugin signs a .uzplugin with PluginSeed.
func SignPlugin(t testing.TB, data []byte) []byte {
	t.Helper()
	out, err := pluginpkg.Sign(data, PluginSeed())
	if err != nil {
		t.Fatal(err)
	}
	return out
}

// PluginIndex writes the signed files and a signed plugin-index.json into dir and returns the file:// URL of the
// index (for hub.Options.PluginIndexURL). badSig writes a signature of another key.
func PluginIndex(t testing.TB, dir string, badSig bool, signedFiles ...[]byte) string {
	t.Helper()
	idx := pluginpkg.Index{Format: pluginpkg.IndexFormat, GeneratedAt: time.Date(2026, 10, 12, 0, 0, 0, 0, time.UTC)}
	for i, data := range signedFiles {
		p, err := pluginpkg.Open(data)
		if err != nil {
			t.Fatal(err)
		}
		name := filepath.Join(dir, fmt.Sprintf("plugin-%d.uzplugin", i))
		if err := os.WriteFile(name, data, 0o644); err != nil {
			t.Fatal(err)
		}
		idx.Plugins = append(idx.Plugins, pluginpkg.EntryFor(p, updates.FileURL(name)))
	}
	pluginpkg.SortEntries(idx.Plugins)
	b, err := pluginpkg.MarshalIndex(idx)
	if err != nil {
		t.Fatal(err)
	}
	seed := PluginSeed()
	if badSig {
		seed = bytes.Repeat([]byte{7}, ed25519.SeedSize)
	}
	sig, err := corepkg.Sign(b, seed)
	if err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(dir, "plugin-index.json")
	if err := os.WriteFile(path, b, 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path+".sig", sig, 0o644); err != nil {
		t.Fatal(err)
	}
	return updates.FileURL(path)
}
