package pluginpkg

import (
	"strings"
	"testing"
	"time"
)

func goodEntry() IndexEntry {
	return IndexEntry{ID: "net.example.tracker", Name: "Tracker", Version: "1.2.0", API: 1, Description: "d", Permissions: []string{"ui"},
		URL: "https://example.org/t.uzplugin", SHA256: strings.Repeat("ab", 32), Size: 100}
}

func TestIndexRoundTripAndSkips(t *testing.T) {
	good := goodEntry()
	bad := goodEntry()
	bad.Version = "1.3.0"
	bad.URL = "http://example.org/x.uzplugin"
	idx := Index{Format: 1, GeneratedAt: time.Date(2026, 10, 12, 1, 2, 3, 0, time.UTC), Plugins: []IndexEntry{good, bad}}
	b, err := MarshalIndex(idx)
	if err != nil {
		t.Fatal(err)
	}
	got, errs := ParseIndex(b, false)
	if len(got.Plugins) != 1 || len(errs) != 1 || Fatal(errs) {
		t.Fatalf("plugins %d errs %v", len(got.Plugins), errs)
	}
	if !LooksLikeIndex(b) || LooksLikeIndex([]byte(`{"releases":[]}`)) {
		t.Fatal("LooksLikeIndex")
	}
}

func TestIndexFatal(t *testing.T) {
	for name, in := range map[string]string{
		"format":    `{"format":2,"generated_at":"2026-10-12T00:00:00Z","plugins":[]}`,
		"json":      `{`,
		"trailing":  `{"format":1,"generated_at":"2026-10-12T00:00:00Z","plugins":[]} x`,
		"duplicate": `{"format":1,"generated_at":"2026-10-12T00:00:00Z","plugins":[` + entryJSON() + `,` + entryJSON() + `]}`,
	} {
		if _, errs := ParseIndex([]byte(in), false); !Fatal(errs) {
			t.Errorf("%s: not fatal: %v", name, errs)
		}
	}
}

func entryJSON() string {
	return `{"id":"net.example.tracker","name":"T","version":"1.0.0","api":1,"description":"","permissions":[],"url":"https://e.org/a","sha256":"` + strings.Repeat("0", 64) + `","size":5}`
}

func TestValidateEntry(t *testing.T) {
	mut := func(f func(*IndexEntry)) IndexEntry { e := goodEntry(); f(&e); return e }
	for name, e := range map[string]IndexEntry{
		"id":       mut(func(e *IndexEntry) { e.ID = "Tracker" }),
		"version":  mut(func(e *IndexEntry) { e.Version = "1.2" }),
		"api":      mut(func(e *IndexEntry) { e.API = 2 }),
		"sha":      mut(func(e *IndexEntry) { e.SHA256 = strings.ToUpper(e.SHA256) }),
		"size0":    mut(func(e *IndexEntry) { e.Size = 0 }),
		"size big": mut(func(e *IndexEntry) { e.Size = MaxCompressedBytes + 1 }),
		"http":     mut(func(e *IndexEntry) { e.URL = "http://e.org/a" }),
		"file":     mut(func(e *IndexEntry) { e.URL = "file:///tmp/a.uzplugin" }),
		"user":     mut(func(e *IndexEntry) { e.URL = "https://u:p@e.org/a" }),
	} {
		if err := ValidateEntry(e, false); err == nil {
			t.Errorf("%s accepted", name)
		}
	}
	f := mut(func(e *IndexEntry) { e.URL = "file:///tmp/a.uzplugin" })
	if err := ValidateEntry(f, true); err != nil {
		t.Errorf("file url with allowFile: %v", err)
	}
	if err := ValidateEntry(goodEntry(), false); err != nil {
		t.Error(err)
	}
}

func TestSortEntries(t *testing.T) {
	es := []IndexEntry{{ID: "b.x", Version: "1.0.0"}, {ID: "a.x", Version: "1.10.0"}, {ID: "a.x", Version: "1.2.0"}, {ID: "a.x", Version: "1.2.0-rc.1"}}
	SortEntries(es)
	got := es[0].ID + es[0].Version + " " + es[1].Version + " " + es[2].Version + " " + es[3].ID
	if got != "a.x1.2.0-rc.1 1.2.0 1.10.0 b.x" {
		t.Fatal(got)
	}
}

func TestIndexReplaces(t *testing.T) {
	e := goodEntry()
	e.Replaces = []string{"net.example.old"}
	if err := ValidateEntry(e, false); err != nil {
		t.Fatal(err)
	}
	b, err := MarshalIndex(Index{Format: 1, GeneratedAt: time.Date(2026, 10, 12, 0, 0, 0, 0, time.UTC), Plugins: []IndexEntry{e, func() IndexEntry { g := goodEntry(); g.Version = "1.3.0"; return g }()}})
	if err != nil {
		t.Fatal(err)
	}
	if strings.Count(string(b), `"replaces"`) != 1 {
		t.Fatalf("replaces must be omitted when empty:\n%s", b)
	}
	got, errs := ParseIndex(b, false)
	if len(errs) != 0 || len(got.Plugins[0].Replaces) != 1 || got.Plugins[0].Replaces[0] != "net.example.old" {
		t.Fatalf("%v %+v", errs, got.Plugins)
	}
	many := []string{}
	for _, s := range "abcdefghi" {
		many = append(many, "net.example."+string(s))
	}
	for name, r := range map[string][]string{
		"invalid": {"Old"}, "self": {"net.example.tracker"}, "duplicate": {"net.example.a", "net.example.a"}, "too many": many,
	} {
		e := goodEntry()
		e.Replaces = r
		if err := ValidateEntry(e, false); err == nil {
			t.Errorf("%s accepted", name)
		}
	}
}
