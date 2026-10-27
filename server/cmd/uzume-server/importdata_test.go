package main

import (
	"database/sql"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"github.com/phabioo/uzume/server/internal/backup"
	"github.com/phabioo/uzume/server/internal/config"
)

// importSource is a stopped server's data folder with an admin, settings that point into it and some files.
func importSource(t *testing.T) string {
	t.Helper()
	dir := newServerData(t, true)
	svc, closeFn := testService(t, dir)
	if err := svc.SetNetOverride(t.Context(), config.NetImportDir, filepath.Join(dir, "imports")); err != nil {
		t.Fatal(err)
	}
	closeFn()
	elsewhere := filepath.Join(t.TempDir(), "roms")
	setSetting(t, dir, "backup_dir", elsewhere)
	setSetting(t, dir, "tricky", dir+"-other") // shares the prefix, is not inside
	for rel, content := range map[string]string{
		"saves/u/g/main/aaaa": "synthetic save", "roms/ab/abcd.nds": "dummy rom", "plugins/p/1.uzplugin": "plugin",
		"logs/hub.log": "log", "backups/old.db": "old", "hub.lock": "lock", "imports/a.txt": "x",
	} {
		p := filepath.Join(dir, rel)
		os.MkdirAll(filepath.Dir(p), 0o750)
		if err := os.WriteFile(p, []byte(content), 0o640); err != nil {
			t.Fatal(err)
		}
	}
	return dir
}

func setSetting(t *testing.T, dir, key, val string) {
	t.Helper()
	db, err := sql.Open("sqlite", "file:"+filepath.ToSlash(filepath.Join(dir, "uzume.db")))
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	if _, err := db.Exec(`INSERT INTO settings(key, value) VALUES(?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value`, key, val); err != nil {
		t.Fatal(err)
	}
}

func getSetting(t *testing.T, dir, key string) string {
	t.Helper()
	db, err := sql.Open("sqlite", "file:"+filepath.ToSlash(filepath.Join(dir, "uzume.db")))
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	var v string
	if err := db.QueryRow(`SELECT value FROM settings WHERE key = ?`, key).Scan(&v); err != nil {
		t.Fatal(err)
	}
	return v
}

func exitCode(err error) int {
	var xe *exitError
	if errors.As(err, &xe) {
		return xe.code
	}
	return -1
}

func TestImportData(t *testing.T) {
	from := importSource(t)
	dest := filepath.Join(t.TempDir(), "ProgramData", "Uzume", "Server") // parents do not exist yet
	var out strings.Builder
	if err := runImportData([]string{"--from", from, "-data-dir", dest}, &out); err != nil {
		t.Fatal(err)
	}
	if strings.TrimSpace(out.String()) != dest {
		t.Fatalf("stdout %q", out.String())
	}
	for _, rel := range []string{"uzume.db", "saves/u/g/main/aaaa", "roms/ab/abcd.nds", "plugins/p/1.uzplugin", "logs/hub.log", "backups/old.db", "imports/a.txt"} {
		if !exists(filepath.Join(dest, rel)) {
			t.Errorf("%s not copied", rel)
		}
	}
	if exists(filepath.Join(dest, "hub.lock")) {
		t.Error("lock file copied")
	}
	// The copy is a working server database with the admin; stored paths moved with it.
	svc, closeFn := testService(t, dest)
	has, err := svc.HasAdmin(t.Context())
	closeFn()
	if err != nil || !has {
		t.Fatalf("admin: %v %v", has, err)
	}
	if got := getSetting(t, dest, "net."+config.NetImportDir); got != filepath.Join(dest, "imports") {
		t.Errorf("import dir %q", got)
	}
	if got := getSetting(t, dest, "tricky"); got != from+"-other" {
		t.Errorf("sibling path changed: %q", got)
	}
	if got := getSetting(t, dest, "backup_dir"); strings.HasPrefix(got, dest) {
		t.Errorf("outside path changed: %q", got)
	}
	// The source stays, plus the note.
	if !hasDatabase(from) || !exists(filepath.Join(from, "saves/u/g/main/aaaa")) {
		t.Fatal("source was touched")
	}
	if got := getSetting(t, from, "net."+config.NetImportDir); got != filepath.Join(from, "imports") {
		t.Errorf("source setting changed: %q", got)
	}
	b, err := os.ReadFile(filepath.Join(from, "moved.json"))
	if err != nil {
		t.Fatal(err)
	}
	var m struct{ To, At string }
	if err := json.Unmarshal(b, &m); err != nil || m.To != dest || m.At == "" {
		t.Fatalf("moved.json %s %v", b, err)
	}
}

func TestImportDataRefusals(t *testing.T) {
	from := importSource(t)
	run := func(from, dest string) error {
		return runImportData([]string{"--from", from, "-data-dir", dest}, &strings.Builder{})
	}
	// Destination has a database: refused, nothing changes there.
	dest := newServerData(t, false)
	before, _ := os.ReadFile(filepath.Join(dest, "uzume.db"))
	if err := run(from, dest); exitCode(err) != 3 || !strings.Contains(err.Error(), "already contains a server database") {
		t.Fatalf("dest with db: %v", err)
	}
	after, _ := os.ReadFile(filepath.Join(dest, "uzume.db"))
	if string(before) != string(after) || exists(filepath.Join(from, "moved.json")) {
		t.Fatal("refusal changed something")
	}
	// Source without a database, or missing.
	if err := run(t.TempDir(), filepath.Join(t.TempDir(), "d")); exitCode(err) != 3 || !strings.Contains(err.Error(), "no server database") {
		t.Fatalf("empty source: %v", err)
	}
	if err := run(filepath.Join(t.TempDir(), "none"), filepath.Join(t.TempDir(), "d")); exitCode(err) != 3 {
		t.Fatalf("missing source: %v", err)
	}
	// Same or nested folders.
	if err := run(from, from); exitCode(err) != 3 {
		t.Fatalf("same: %v", err)
	}
	if err := run(from, filepath.Join(from, "inner")); exitCode(err) != 3 {
		t.Fatalf("nested: %v", err)
	}
	// Missing --from is a usage error.
	var ue *usageError
	if err := runImportData(nil, &strings.Builder{}); !errors.As(err, &ue) {
		t.Fatalf("no --from: %v", err)
	}
}

func TestImportDataSourceInUse(t *testing.T) {
	if runtime.GOOS != "linux" && runtime.GOOS != "windows" {
		t.Skip("no data folder lock on this platform")
	}
	from := importSource(t)
	release, err := backup.LockDataDir(from)
	if err != nil {
		t.Fatal(err)
	}
	defer release()
	dest := filepath.Join(t.TempDir(), "d")
	err = runImportData([]string{"--from", from, "-data-dir", dest}, &strings.Builder{})
	if exitCode(err) != 3 || !strings.Contains(err.Error(), "still running") {
		t.Fatalf("%v", err)
	}
	if exists(dest) {
		t.Fatal("destination created")
	}
}

func TestImportDataFailureLeavesDestAsItWas(t *testing.T) {
	// A destination that exists (as the installer leaves it) with data of its own; the copy fails halfway because a
	// file of the same name is already there.
	from := importSource(t)
	dest := filepath.Join(t.TempDir(), "Server")
	os.MkdirAll(filepath.Join(dest, "update-request"), 0o750)
	os.MkdirAll(filepath.Join(dest, "saves", "u", "g", "main"), 0o750)
	keep := filepath.Join(dest, "saves", "u", "g", "main", "aaaa")
	os.WriteFile(keep, []byte("existing save"), 0o640)
	err := runImportData([]string{"--from", from, "-data-dir", dest}, &strings.Builder{})
	if err == nil || !strings.Contains(err.Error(), "never overwritten") {
		t.Fatalf("%v", err)
	}
	if b, _ := os.ReadFile(keep); string(b) != "existing save" {
		t.Fatalf("existing file changed: %q", b)
	}
	var left []string
	filepath.WalkDir(dest, func(p string, d os.DirEntry, _ error) error {
		rel, _ := filepath.Rel(dest, p)
		left = append(left, filepath.ToSlash(rel))
		return nil
	})
	if got := strings.Join(left, ","); got != ".,saves,saves/u,saves/u/g,saves/u/g/main,saves/u/g/main/aaaa,update-request" {
		t.Fatalf("destination after failure: %s", got)
	}
	if exists(filepath.Join(from, "moved.json")) {
		t.Fatal("moved.json written")
	}
}

func TestImportDataCorruptDatabaseRemovesNewDest(t *testing.T) {
	from := importSource(t)
	if err := os.WriteFile(filepath.Join(from, "uzume.db"), []byte("this is not a database, just text padding to be long enough"), 0o640); err != nil {
		t.Fatal(err)
	}
	dest := filepath.Join(t.TempDir(), "a", "b", "Server")
	err := runImportData([]string{"--from", from, "-data-dir", dest}, &strings.Builder{})
	if err == nil || exitCode(err) == 3 {
		t.Fatalf("%v", err)
	}
	if exists(dest) || exists(filepath.Dir(dest)) {
		t.Fatal("partial copy left behind")
	}
	if exists(filepath.Join(from, "moved.json")) {
		t.Fatal("moved.json written")
	}
}

func TestRebasePath(t *testing.T) {
	from, to := filepath.FromSlash("/data/old"), filepath.FromSlash("/srv/new")
	if runtime.GOOS == "windows" {
		from, to = `C:\Users\Max\old`, `C:\ProgramData\Uzume\Server`
	}
	for _, c := range []struct {
		in   string
		want string
		ok   bool
	}{
		{from, to, true},
		{filepath.Join(from, "a", "b"), filepath.Join(to, "a", "b"), true},
		{from + "-other", "", false},
		{from + "x" + string(filepath.Separator) + "a", "", false},
		{filepath.Join(filepath.Dir(from), "else"), "", false},
		{"relative/dir", "", false},
		{"", "", false},
		{"true", "", false},
	} {
		got, ok := rebasePath(c.in, from, to, false)
		if ok != c.ok || got != c.want {
			t.Errorf("%q: %q %v, want %q %v", c.in, got, ok, c.want, c.ok)
		}
	}
	if got, ok := rebasePath(strings.ToUpper(filepath.Join(from, "A")), strings.ToUpper(from), to, true); !ok || got != filepath.Join(to, "A") {
		t.Errorf("fold: %q %v", got, ok)
	}
}
