package hub_test

import (
	"bytes"
	"database/sql"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/phabioo/uzume/server/internal/backup"
	"github.com/phabioo/uzume/server/internal/hub"
	"github.com/phabioo/uzume/server/internal/hub/hubtest"
)

// Admin storage overview and clean-ups (0.19 package 7). All data is synthetic.

type storageEnv struct {
	t          *testing.T
	svc        *hub.Service
	clk        *hubtest.Clock
	db         *sql.DB
	dir        string
	user       hub.User
	game       hub.Game
	devA, devB string
}

func newStorageEnv(t *testing.T) *storageEnv {
	t.Helper()
	dir := t.TempDir()
	svc, clk, db := hubtest.OpenIn(t, dir, nil)
	u, err := svc.CreateAdmin(ctx, "fabio", "secret-12345")
	if err != nil {
		t.Fatal(err)
	}
	g, err := svc.AddROM(ctx, bytes.NewReader(randomROM(2000)), "demo.nds", "", "", u.ID)
	if err != nil {
		t.Fatal(err)
	}
	return &storageEnv{t: t, svc: svc, clk: clk, db: db, dir: dir, user: u, game: g,
		devA: pairDevice(t, svc, u.ID, "Desktop"), devB: pairDevice(t, svc, u.ID, "Laptop")}
}

func (e *storageEnv) put(slot, dev string, base int, data []byte) hub.PutSaveResult {
	e.t.Helper()
	r, err := e.svc.PutSave(ctx, hub.PutSaveInput{UserID: e.user.ID, DeviceID: dev, GameID: e.game.ID, Slot: slot,
		BaseRevision: base, SHA256: hexSHA(data), Reason: hub.SyncCheckpoint, Body: bytes.NewReader(data)})
	if err != nil {
		e.t.Fatalf("put: %v", err)
	}
	return r
}

func (e *storageEnv) blob(slot string, data []byte) string {
	return filepath.Join(e.dir, "saves", e.user.ID, e.game.ID, slot, hexSHA(data))
}

func (e *storageEnv) write(rel string, n int) {
	e.t.Helper()
	p := filepath.Join(e.dir, rel)
	if err := os.MkdirAll(filepath.Dir(p), 0o750); err != nil {
		e.t.Fatal(err)
	}
	if err := os.WriteFile(p, bytes.Repeat([]byte("x"), n), 0o640); err != nil {
		e.t.Fatal(err)
	}
}

func (e *storageEnv) overview() hub.StorageOverview {
	e.t.Helper()
	ov, err := e.svc.StorageOverview(ctx)
	if err != nil {
		e.t.Fatal(err)
	}
	return ov
}

func (e *storageEnv) kind(ov hub.StorageOverview, id string) (hub.StorageKind, bool) {
	for _, k := range ov.Kinds {
		if k.ID == id {
			return k, true
		}
	}
	return hub.StorageKind{}, false
}

func (e *storageEnv) cleanup(ov hub.StorageOverview, id string) hub.StorageCleanup {
	e.t.Helper()
	for _, c := range ov.Cleanup {
		if c.ID == id {
			return c
		}
	}
	e.t.Fatalf("no clean-up %s", id)
	return hub.StorageCleanup{}
}

func (e *storageEnv) backdate(days int) {
	e.t.Helper()
	old := e.clk.Now().Add(-time.Duration(days) * 24 * time.Hour).Unix()
	if _, err := e.db.Exec(`UPDATE save_history SET created_at = ?`, old); err != nil {
		e.t.Fatal(err)
	}
}

func TestStorageKindsSumSyntheticFiles(t *testing.T) {
	e := newStorageEnv(t)
	cur, old, shared := []byte("current-save-bytes"), []byte("older-save"), []byte("shared-blob")
	e.put("main", e.devA, 0, old)
	e.put("main", e.devB, 1, shared) // v1 = old (device change)
	if _, err := e.svc.CreateSaveSnapshot(ctx, e.user.ID, e.game.ID, "main", nil); err != nil {
		t.Fatal(err) // v2 = shared blob, still the current checkpoint's
	}
	e.put("main", e.devB, 2, cur) // same device: no history; current is now cur, the snapshot keeps `shared`
	e.write("states/u/g/state1", 30)
	e.write("states/u/g/state2", 12)
	e.write("firmware/nds/bios.bin", 77)
	e.write("cores/foo/1/linux/foo_libretro.so", 500)

	ov := e.overview()
	order := []string{"games", "saves", "snapshots", "states", "screenshots", "cores", "firmware"}
	if len(ov.Kinds) != len(order) {
		t.Fatalf("%+v", ov.Kinds)
	}
	for i, id := range order {
		if ov.Kinds[i].ID != id {
			t.Fatalf("kinds out of order: %+v", ov.Kinds)
		}
	}
	want := map[string]hub.StorageKind{
		"games":     {Bytes: 2000, Count: 1},
		"saves":     {Bytes: int64(len(cur)), Count: 1},
		"snapshots": {Bytes: int64(len(old) + len(shared)), Count: 2},
		"states":    {Bytes: 42, Count: 2},
		"firmware":  {Bytes: 77, Count: 1},
		"cores":     {Bytes: 500, Count: 1},
	}
	for id, w := range want {
		k, _ := e.kind(ov, id)
		if k.Bytes != w.Bytes || k.Count != w.Count {
			t.Errorf("%s = %+v, want %+v", id, k, w)
		}
	}
	if k, _ := e.kind(ov, "screenshots"); k.Bytes != 0 || k.Count != 0 {
		t.Errorf("screenshots %+v", k)
	}
	if _, ok := e.kind(ov, "backups"); ok {
		t.Fatal("backups present without a backup folder")
	}
	if len(ov.Cleanup) != 4 || ov.Cleanup[0].ID != "old_snapshots" || ov.Cleanup[1].ID != "upload_leftovers" || ov.Cleanup[2].ID != "unused_cores" || ov.Cleanup[3].ID != "deleted_game_states" {
		t.Fatalf("cleanup %+v", ov.Cleanup)
	}
	for _, c := range ov.Cleanup {
		if c.Count != 0 || c.Bytes != 0 {
			t.Errorf("fresh data offers %+v", c)
		}
	}

	// The snapshot of the current content is shared: it counts as a save, not as a snapshot.
	if _, err := e.svc.CreateSaveSnapshot(ctx, e.user.ID, e.game.ID, "main", nil); err != nil {
		t.Fatal(err)
	}
	ov = e.overview()
	if k, _ := e.kind(ov, "saves"); k.Bytes != int64(len(cur)) || k.Count != 1 {
		t.Errorf("saves after shared snapshot %+v", k)
	}
	if k, _ := e.kind(ov, "snapshots"); k.Count != 2 {
		t.Errorf("snapshots after shared snapshot %+v", k)
	}

	// Backups appear once a folder is set: the sum of the backup files in it.
	folder := t.TempDir()
	if err := e.svc.SetBackupSettings(ctx, hub.BackupSettingsUpdate{Folder: &folder}); err != nil {
		t.Fatal(err)
	}
	name := backup.FileName("Test-Hub", e.clk.Now(), false)
	if err := os.WriteFile(filepath.Join(folder, name), bytes.Repeat([]byte("b"), 123), 0o640); err != nil {
		t.Fatal(err)
	}
	ov = e.overview()
	if k, ok := e.kind(ov, "backups"); !ok || k.Bytes != 123 || k.Count != 1 || ov.Kinds[len(ov.Kinds)-1].ID != "backups" {
		t.Errorf("backups %+v %v", k, ok)
	}
}

func TestStorageCleanupOldSnapshots(t *testing.T) {
	e := newStorageEnv(t)
	a, b, c, d := []byte("save-A"), []byte("save-B"), []byte("save-C"), []byte("save-D")
	e.put("main", e.devA, 0, a)
	e.put("main", e.devB, 1, b) // v1 = A
	e.put("main", e.devA, 2, c) // v2 = B
	e.put("main", e.devB, 3, d) // v3 = C
	snap, err := e.svc.CreateSaveSnapshot(ctx, e.user.ID, e.game.ID, "main", nil)
	if err != nil { // v4 = D, manual, shares its blob with the current checkpoint
		t.Fatal(err)
	}
	if len(histOf(t, e, "main")) != 4 {
		t.Fatalf("history %+v", histOf(t, e, "main"))
	}

	// A slot with an open conflict: its old versions stay.
	e.put("other", e.devA, 0, []byte("o1"))
	e.put("other", e.devB, 1, []byte("o2")) // v1 = o1
	if r := e.put("other", e.devA, 1, []byte("o3")); r.Conflict == nil {
		t.Fatal("no conflict")
	}
	// A slot whose highest versions go: the numbers must not come back.
	x, y, z := []byte("third-x"), []byte("third-y"), []byte("third-z")
	e.put("third", e.devA, 0, x)
	e.put("third", e.devB, 1, y) // v1 = x
	e.put("third", e.devA, 2, z) // v2 = y
	e.backdate(100)
	// A recent automatic version stays too.
	f := []byte("save-F")
	e.put("main", e.devA, 4, f) // current D is already in the history (v4): nothing captured
	g := []byte("save-G")
	e.put("main", e.devB, 5, g) // v5 = F, recent

	want := int64(len(a) + len(b) + len(c) + len(x) + len(y))
	oc := e.cleanup(e.overview(), "old_snapshots")
	if oc.Count != 5 || oc.Bytes != want {
		t.Fatalf("offer %+v", oc)
	}
	res, err := e.svc.StorageCleanup(ctx, "old_snapshots")
	if err != nil {
		t.Fatal(err)
	}
	if res.ID != "old_snapshots" || res.Removed != 5 || res.FreedBytes != want {
		t.Fatalf("result %+v", res)
	}
	for _, v := range [][]byte{a, b, c} {
		mustExist(t, e.blob("main", v), false)
	}
	for _, v := range [][]byte{x, y} {
		mustExist(t, e.blob("third", v), false)
	}
	mustExist(t, e.blob("main", d), true) // manual snapshot
	mustExist(t, e.blob("main", f), true) // recent version
	mustExist(t, e.blob("main", g), true) // current checkpoint
	mustExist(t, e.blob("third", z), true)
	mustExist(t, e.blob("other", []byte("o1")), true) // open conflict
	h := histOf(t, e, "main")
	if len(h) != 2 {
		t.Fatalf("history %+v", h)
	}
	sawSnap := false
	for _, v := range h {
		if v.Version == snap.Version && v.Reason == hub.HistoryManualSnapshot {
			sawSnap = true
		}
	}
	if !sawSnap {
		t.Fatalf("manual snapshot gone: %+v", h)
	}
	if cur, err := e.svc.GetSaveSlot(ctx, e.user.ID, e.game.ID, "main"); err != nil || cur.Current.SHA256 != hexSHA(g) {
		t.Fatalf("checkpoint changed: %+v %v", cur, err)
	}
	if n := len(histOf(t, e, "other")); n != 2 {
		t.Fatalf("history of the slot with the conflict: %d versions", n)
	}
	if left := e.cleanup(e.overview(), "old_snapshots"); left.Count != 0 || left.Bytes != 0 {
		t.Fatalf("still offered: %+v", left)
	}
	// Version numbers are never reused: z moves to the history as v3, not v1.
	e.put("third", e.devB, 3, []byte("third-w"))
	if h := histOf(t, e, "third"); len(h) != 1 || h[0].Version != 3 || h[0].SHA256 != hexSHA(z) {
		t.Fatalf("third %+v", h)
	}
	if _, err := e.svc.StorageCleanup(ctx, "nope"); err == nil {
		t.Fatal("unknown id accepted")
	}
}

func histOf(t *testing.T, e *storageEnv, slot string) []hub.SaveVersion {
	t.Helper()
	h, err := e.svc.ListSaveHistory(ctx, e.user.ID, e.game.ID, slot)
	if err != nil {
		t.Fatal(err)
	}
	return h
}

func TestStorageCleanupUploadLeftovers(t *testing.T) {
	e := newStorageEnv(t)
	mk := func(name string, n int) hub.UploadSession {
		t.Helper()
		u, err := e.svc.CreateUpload(ctx, e.user.ID, hub.UploadInput{Filename: name, Size: int64(n) + 1000})
		if err != nil {
			t.Fatal(err)
		}
		e.write(filepath.Join("tmp", "uploads", u.ID), n)
		return u
	}
	oldUp := mk("old.nds", 64)
	e.clk.Advance(8 * 24 * time.Hour)
	newUp := mk("new.nds", 32)

	oc := e.cleanup(e.overview(), "upload_leftovers")
	if oc.Count != 1 || oc.Bytes != 64 {
		t.Fatalf("offer %+v", oc)
	}
	res, err := e.svc.StorageCleanup(ctx, "upload_leftovers")
	if err != nil || res.Removed != 1 || res.FreedBytes != 64 {
		t.Fatalf("result %+v %v", res, err)
	}
	mustExist(t, filepath.Join(e.dir, "tmp", "uploads", oldUp.ID), false)
	mustExist(t, filepath.Join(e.dir, "tmp", "uploads", newUp.ID), true)
	if _, err := e.svc.GetUpload(ctx, e.user.ID, newUp.ID); err != nil {
		t.Fatalf("recent upload gone: %v", err)
	}
	if again, _ := e.svc.StorageCleanup(ctx, "upload_leftovers"); again.Removed != 0 || again.FreedBytes != 0 {
		t.Fatalf("second run %+v", again)
	}
}

func TestStorageCleanupUnusedCores(t *testing.T) {
	e := newStorageEnv(t)
	add := func(core, version string, size int) {
		t.Helper()
		if _, err := e.db.Exec(`INSERT INTO core_packages(core_id, version, platform, license, source_url, source_ref, origin, synced_at)
			VALUES (?, ?, 'linux-x86_64', 'GPL', '', '', 'libretro-buildbot', 0)`, core, version); err != nil {
			t.Fatal(err)
		}
		if _, err := e.db.Exec(`INSERT INTO core_package_files(core_id, version, platform, name, role, size, sha256, url, cached_at)
			VALUES (?, ?, 'linux-x86_64', 'lib.so', 'library', ?, 'x', '', 1)`, core, version, size); err != nil {
			t.Fatal(err)
		}
		e.write(filepath.Join("cores", core, version, "linux-x86_64", "lib.so"), size)
	}
	add("foo", "1", 100) // no system uses it
	add("foo", "2", 200) // installed on nds
	add("bar", "7", 50)  // no system uses it
	if _, err := e.db.Exec(`INSERT INTO system_cores(system_id, core_id, version, installed_at, origin, display_name, license)
		VALUES ('nds', 'foo', '2', 0, 'libretro-buildbot', 'Foo', 'GPL')`); err != nil {
		t.Fatal(err)
	}
	if _, err := e.db.Exec(`UPDATE systems SET default_core_id = 'foo' WHERE id = 'nds'`); err != nil {
		t.Fatal(err)
	}
	oc := e.cleanup(e.overview(), "unused_cores")
	if oc.Count != 2 || oc.Bytes != 150 {
		t.Fatalf("offer %+v", oc)
	}
	res, err := e.svc.StorageCleanup(ctx, "unused_cores")
	if err != nil || res.Removed != 2 || res.FreedBytes != 150 {
		t.Fatalf("result %+v %v", res, err)
	}
	mustExist(t, filepath.Join(e.dir, "cores", "foo", "1"), false)
	mustExist(t, filepath.Join(e.dir, "cores", "bar"), false)
	mustExist(t, filepath.Join(e.dir, "cores", "foo", "2", "linux-x86_64", "lib.so"), true)
	var n int
	if err := e.db.QueryRow(`SELECT COUNT(*) FROM core_packages`).Scan(&n); err != nil || n != 1 {
		t.Fatalf("packages left %d %v", n, err)
	}
	if err := e.db.QueryRow(`SELECT COUNT(*) FROM system_cores`).Scan(&n); err != nil || n != 1 {
		t.Fatalf("installed cores left %d %v", n, err)
	}
	if c := e.cleanup(e.overview(), "unused_cores"); c.Count != 0 {
		t.Fatalf("still offered %+v", c)
	}
}

func TestStorageScreenshotUsageLargestFirst(t *testing.T) {
	e := newPlayEnv(t, nil)
	small := e.shot(e.admin.ID, e.g1.ID, "", pngBytes(t, 4, 4))
	big := e.shot(e.admin.ID, e.g2.ID, "", pngBytes(t, 200, 200))
	ov, err := e.svc.StorageOverview(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if len(ov.Screenshots) != 2 || ov.Screenshots[0].GameID != e.g2.ID || ov.Screenshots[0].Bytes != big.SizeBytes ||
		ov.Screenshots[1].Bytes != small.SizeBytes || ov.Screenshots[0].GameName != "Two" || ov.Screenshots[0].UserName == "" ||
		strings.TrimSpace(ov.Screenshots[0].UserID) == "" {
		t.Fatalf("%+v", ov.Screenshots)
	}
	for _, k := range ov.Kinds {
		if k.ID == "screenshots" && (k.Count != 2 || k.Bytes != big.SizeBytes+small.SizeBytes) {
			t.Fatalf("%+v", k)
		}
	}
}
