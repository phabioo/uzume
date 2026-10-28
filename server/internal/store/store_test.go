package store

import (
	"database/sql"
	"os"
	"path/filepath"
	"reflect"
	"strconv"
	"strings"
	"testing"
	"time"
)

func TestOpenMigrateIdempotent(t *testing.T) {
	p := filepath.Join(t.TempDir(), "db.sqlite")
	db, err := Open(p)
	if err != nil {
		t.Fatal(err)
	}
	v, _ := SchemaVersion(db)
	if v != 30 {
		t.Fatalf("version %d", v)
	}
	var fk int
	db.QueryRow(`PRAGMA foreign_keys`).Scan(&fk)
	if fk != 1 {
		t.Fatal("foreign keys off")
	}
	db.Close()
	db, err = Open(p) // second start does not migrate again
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	if v, _ := SchemaVersion(db); v != 30 {
		t.Fatalf("version after restart %d", v)
	}
	if _, err := db.Exec(`INSERT INTO devices(id,user_id,name,platform,arch,player_version,credential_hash,status,created_at) VALUES('d','nouser','n','p','a','v','h','trusted',1)`); err == nil {
		t.Fatal("FK violation must fail")
	}
}

func rawDB(t *testing.T, path string) *sql.DB {
	t.Helper()
	db, err := sql.Open("sqlite", "file:"+path+"?_pragma=foreign_keys(1)")
	if err != nil {
		t.Fatal(err)
	}
	db.SetMaxOpenConns(1)
	return db
}

func backupFiles(t *testing.T, dir string) []string {
	t.Helper()
	es, _ := os.ReadDir(dir)
	var out []string
	for _, e := range es {
		out = append(out, e.Name())
	}
	return out
}

func TestBackupBeforeMigration(t *testing.T) {
	old := nowFunc
	nowFunc = func() time.Time { return time.Date(2026, 10, 7, 12, 30, 45, 0, time.UTC) }
	t.Cleanup(func() { nowFunc = old })
	dir := t.TempDir()
	p := filepath.Join(dir, "uzume.db")

	// A fresh database is not backed up.
	db, err := Open(p)
	if err != nil {
		t.Fatal(err)
	}
	db.Close()
	if _, err := os.Stat(filepath.Join(dir, "backups")); err == nil {
		t.Fatalf("fresh database must not be backed up: %v", backupFiles(t, filepath.Join(dir, "backups")))
	}
	// An up-to-date database is not backed up either.
	db, _ = Open(p)
	db.Close()
	if _, err := os.Stat(filepath.Join(dir, "backups")); err == nil {
		t.Fatal("no pending migration, no backup")
	}

	// An existing database at schema 3 gets a backup before 4, 5 and 6 are applied.
	p2 := filepath.Join(dir, "old.db")
	raw := rawDB(t, p2)
	if err := migrate(raw, "", 3); err != nil {
		t.Fatal(err)
	}
	raw.Close()
	db, err = Open(p2)
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	if v, _ := SchemaVersion(db); v != 30 {
		t.Fatalf("version %d", v)
	}
	bdir := filepath.Join(dir, "backups")
	got := backupFiles(t, bdir)
	if len(got) != 1 || got[0] != "hub-3-to-30-20261007T123045Z.db" {
		t.Fatalf("backups: %v", got)
	}
	bk := rawDB(t, filepath.Join(bdir, got[0]))
	defer bk.Close()
	if v, err := SchemaVersion(bk); err != nil || v != 3 {
		t.Fatalf("backup holds schema %d, %v", v, err)
	}
}

func TestBackupsPrunedToFive(t *testing.T) {
	dir := t.TempDir()
	for _, n := range []string{"hub-1-to-2-20260101T000000Z.db", "hub-2-to-3-20260102T000000Z.db", "hub-3-to-4-20260103T000000Z.db",
		"hub-4-to-5-20260104T000000Z.db", "hub-4-to-5-20260105T000000Z.db", "hub-4-to-5-20260106T000000Z.db", "notes.txt"} {
		os.WriteFile(filepath.Join(dir, n), nil, 0o600)
	}
	if err := pruneBackups(dir, BackupsKept); err != nil {
		t.Fatal(err)
	}
	got := backupFiles(t, dir)
	want := []string{"hub-2-to-3-20260102T000000Z.db", "hub-3-to-4-20260103T000000Z.db", "hub-4-to-5-20260104T000000Z.db",
		"hub-4-to-5-20260105T000000Z.db", "hub-4-to-5-20260106T000000Z.db", "notes.txt"}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("got %v", got)
	}
}

func TestBackupFailureAbortsMigration(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "old.db")
	raw := rawDB(t, p)
	if err := migrate(raw, "", 3); err != nil {
		t.Fatal(err)
	}
	// The backup directory cannot be created: a regular file is in the way.
	blocker := filepath.Join(dir, "backups")
	os.WriteFile(blocker, []byte("x"), 0o600)
	err := MigrateWithBackup(raw, blocker)
	if err == nil || !strings.Contains(err.Error(), "backup") {
		t.Fatalf("want a clear backup error, got %v", err)
	}
	if v, _ := SchemaVersion(raw); v != 3 {
		t.Fatalf("migration ran despite the failed backup: schema %d", v)
	}
	raw.Close()
	// Open fails the same way (startup aborts).
	os.Remove(blocker)
	os.WriteFile(blocker, []byte("x"), 0o600)
	if _, err := Open(p); err == nil || !strings.Contains(err.Error(), "backup") {
		t.Fatalf("Open: %v", err)
	}
}

func TestMigration0006KeepsSavesAndAllowsNewReasons(t *testing.T) {
	db := rawDB(t, filepath.Join(t.TempDir(), "s.db"))
	defer db.Close()
	if err := migrate(db, "", 5); err != nil {
		t.Fatal(err)
	}
	for _, q := range []string{
		`INSERT INTO users(id, username, display_name, role, created_at) VALUES ('u1','a','A','admin',1)`,
		`INSERT INTO save_slots VALUES ('u1','g1','default',2,'aa',3,'d1','final',10)`,
		`INSERT INTO save_history VALUES ('u1','g1','default',1,1,'bb',3,'d1','checkpoint','session_end',NULL,9)`,
		`INSERT INTO save_history VALUES ('u1','g1','default',2,1,'cc',3,'d2','checkpoint','conflict_upload',1,9)`,
	} {
		if _, err := db.Exec(q); err != nil {
			t.Fatal(q, err)
		}
	}
	if err := migrate(db, "", 0); err != nil {
		t.Fatal(err)
	}
	var n int
	var label sql.NullString
	if err := db.QueryRow(`SELECT COUNT(*), MAX(label) FROM save_history WHERE slot = 'default'`).Scan(&n, &label); err != nil || n != 2 || label.Valid {
		t.Fatalf("history rows %d label %v: %v", n, label, err)
	}
	var base sql.NullInt64
	if err := db.QueryRow(`SELECT base_revision FROM save_history WHERE version = 2`).Scan(&base); err != nil || base.Int64 != 1 {
		t.Fatalf("base_revision: %v %v", base, err)
	}
	for _, q := range []string{
		`UPDATE save_slots SET reason = 'restore'`,
		`INSERT INTO save_history VALUES ('u1','g1','default',3,2,'aa',3,'d1','restore','before_restore',NULL,11,'my label',0)`,
	} {
		if _, err := db.Exec(q); err != nil {
			t.Fatal(q, err)
		}
	}
	if _, err := db.Exec(`UPDATE save_slots SET reason = 'bogus'`); err == nil {
		t.Fatal("reason constraint missing")
	}
	// The index of the history table exists again.
	if err := db.QueryRow(`SELECT COUNT(*) FROM sqlite_master WHERE name = 'save_history_sha'`).Scan(&n); err != nil || n != 1 {
		t.Fatalf("index: %d %v", n, err)
	}
}

func TestMigration0009AdoptsLegacyCoreAsDefault(t *testing.T) {
	setup := func(t *testing.T, cached bool) *sql.DB {
		db := rawDB(t, filepath.Join(t.TempDir(), "s.db"))
		t.Cleanup(func() { db.Close() })
		if err := migrate(db, "", 8); err != nil {
			t.Fatal(err)
		}
		cachedAt := "NULL"
		if cached {
			cachedAt = "5"
		}
		for _, q := range []string{
			`INSERT INTO core_packages VALUES ('melonds_ds','1.3.0','linux-x64','GPL-3.0','u','r','Uzume CI',1)`,
			`INSERT INTO core_packages VALUES ('melonds_ds','1.4.0','linux-x64','GPL-3.0','u','r','Uzume CI',2)`,
			`INSERT INTO core_package_files VALUES ('melonds_ds','1.3.0','linux-x64','a.so','library',1,'aa','u',5)`,
			`INSERT INTO core_package_files VALUES ('melonds_ds','1.4.0','linux-x64','a.so','library',1,'bb','u',` + cachedAt + `)`,
		} {
			if _, err := db.Exec(q); err != nil {
				t.Fatal(q, err)
			}
		}
		if err := migrate(db, "", 0); err != nil {
			t.Fatal(err)
		}
		return db
	}
	// The expected version (1.4.0 in the seed) is cached: it becomes the installed default core.
	db := setup(t, true)
	var ver, origin, def, lib string
	if err := db.QueryRow(`SELECT version, origin FROM system_cores WHERE system_id = 'nds' AND core_id = 'melonds_ds'`).Scan(&ver, &origin); err != nil ||
		ver != "1.4.0" || origin != "legacy-mirror" {
		t.Fatalf("%q %q %v", ver, origin, err)
	}
	db.QueryRow(`SELECT default_core_id, libretro_ids FROM systems WHERE id = 'nds'`).Scan(&def, &lib)
	if def != "melonds_ds" || lib != "nds" {
		t.Fatalf("default %q libretro %q", def, lib)
	}
	db.QueryRow(`SELECT origin FROM core_packages WHERE version = '1.3.0'`).Scan(&origin)
	if origin != "legacy-mirror" {
		t.Fatalf("package origin %q", origin)
	}
	var n int
	if db.QueryRow(`SELECT COUNT(*) FROM profiled_cores WHERE core_id IN ('melondsds','desmume')`).Scan(&n); n != 2 {
		t.Fatalf("profiled cores %d", n)
	}
	// 1.4.0 is not cached: the cached 1.3.0 is adopted instead.
	db = setup(t, false)
	if err := db.QueryRow(`SELECT version FROM system_cores WHERE core_id = 'melonds_ds'`).Scan(&ver); err != nil || ver != "1.3.0" {
		t.Fatalf("%q %v", ver, err)
	}
}

func TestMigration0009WithoutLegacyPackagesInstallsNothing(t *testing.T) {
	db := rawDB(t, filepath.Join(t.TempDir(), "s.db"))
	defer db.Close()
	if err := migrate(db, "", 0); err != nil {
		t.Fatal(err)
	}
	var n int
	var def sql.NullString
	db.QueryRow(`SELECT COUNT(*) FROM system_cores`).Scan(&n)
	db.QueryRow(`SELECT default_core_id FROM systems WHERE id = 'nds'`).Scan(&def)
	if n != 0 || def.Valid {
		t.Fatalf("%d %v", n, def)
	}
}

// Migration 0015 (local ROMs): every existing game becomes a stored file of itself; the version column stays NULL.
func TestMigration0015ExistingGames(t *testing.T) {
	p := filepath.Join(t.TempDir(), "uzume.db")
	raw := rawDB(t, p)
	if err := migrate(raw, "", 14); err != nil {
		t.Fatal(err)
	}
	for _, q := range []string{
		`INSERT INTO users(id, username, display_name, role, created_at) VALUES ('u1', 'admin', 'admin', 'admin', 1)`,
		`INSERT INTO games(id, title, system, rom_sha256, rom_size, filename, uploaded_by, added_at, game_code, game_code_kind)
			VALUES ('g1', 'One', 'nds', '` + strings.Repeat("a", 64) + `', 10, 'one.nds', 'u1', 5, 'ABCD', 'nds-gamecode')`,
		`INSERT INTO games(id, title, system, rom_sha256, rom_size, filename, uploaded_by, added_at, game_code, game_code_kind)
			VALUES ('g2', 'Two', 'nds', '` + strings.Repeat("b", 64) + `', 20, 'two.nds', 'u1', 6, '', '')`,
	} {
		if _, err := raw.Exec(q); err != nil {
			t.Fatalf("%v: %s", err, q)
		}
	}
	raw.Close()

	db, err := Open(p)
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	rows, err := db.Query(`SELECT game_id, sha256, size, filename, stored, added_by, added_at FROM game_roms ORDER BY game_id`)
	if err != nil {
		t.Fatal(err)
	}
	defer rows.Close()
	var got []string
	for rows.Next() {
		var g, sha, fn, by string
		var size, stored, at int64
		if err := rows.Scan(&g, &sha, &size, &fn, &stored, &by, &at); err != nil {
			t.Fatal(err)
		}
		got = append(got, strings.Join([]string{g, sha[:2], fn, by}, "/")+"/"+itoa(size)+"/"+itoa(stored)+"/"+itoa(at))
	}
	want := []string{"g1/aa/one.nds/u1/10/1/5", "g2/bb/two.nds/u1/20/1/6"}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("game_roms %v, want %v", got, want)
	}
	var nullVersions int
	db.QueryRow(`SELECT COUNT(*) FROM games WHERE game_version IS NULL`).Scan(&nullVersions)
	if nullVersions != 2 {
		t.Fatalf("game_version must stay NULL for existing games, got %d NULL", nullVersions)
	}
	var owners int
	db.QueryRow(`SELECT COUNT(*) FROM rom_owners`).Scan(&owners)
	if owners != 0 {
		t.Fatalf("rom_owners %d", owners)
	}
}

func itoa(n int64) string { return strconv.FormatInt(n, 10) }

func TestMigration0017ExistingRowsGetDefaults(t *testing.T) {
	p := filepath.Join(t.TempDir(), "uzume.db")
	raw := rawDB(t, p)
	if err := migrate(raw, "", 16); err != nil {
		t.Fatal(err)
	}
	for _, q := range []string{
		`INSERT INTO users(id, username, display_name, role, created_at) VALUES ('u1', 'admin', 'admin', 'admin', 1)`,
		`INSERT INTO system_cores(system_id, core_id, version, installed_at, origin, display_name, license) VALUES ('nds', 'testcore', '1', 1, 'import', 'Test core', 'GPL')`,
		`INSERT INTO invites(id, code_hash, authorize_device, status, created_by, created_at, expires_at) VALUES ('i1', 'hash1', 1, 'active', 'u1', 1, 2)`,
	} {
		if _, err := raw.Exec(q); err != nil {
			t.Fatalf("%v: %s", err, q)
		}
	}
	raw.Close()

	db, err := Open(p)
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	var sys, core string
	var forUser sql.NullString
	if err := db.QueryRow(`SELECT COALESCE((SELECT MIN(core_auto_update) FROM systems), 'none')`).Scan(&sys); err != nil || sys != "" {
		t.Fatalf("systems.core_auto_update %q (%v): existing systems follow the server default", sys, err)
	}
	if err := db.QueryRow(`SELECT auto_update FROM system_cores WHERE core_id = 'testcore'`).Scan(&core); err != nil || core != "" {
		t.Fatalf("system_cores.auto_update %q (%v): existing cores follow the system", core, err)
	}
	if err := db.QueryRow(`SELECT for_user_id FROM invites WHERE id = 'i1'`).Scan(&forUser); err != nil || forUser.Valid {
		t.Fatalf("invites.for_user_id %v (%v): existing invites create a new user", forUser, err)
	}
}

func TestMigration0024RunRulesDefaultsAndStates(t *testing.T) {
	db := rawDB(t, filepath.Join(t.TempDir(), "s.db"))
	defer db.Close()
	if err := migrate(db, "", 23); err != nil {
		t.Fatal(err)
	}
	for _, q := range []string{
		`INSERT INTO users(id, username, display_name, role, created_at) VALUES ('u1','a','A','admin',1)`,
		`INSERT INTO runs(id, name, status, created_by, created_at, updated_at, status_by) VALUES ('r1','Old run','active','u1',1,1,'u1')`,
		`INSERT INTO save_slots(user_id, game_id, slot, revision, sha256, size, device_id, reason, created_at) VALUES ('u1','g1','default',2,'aa',3,'d1','final',10)`,
	} {
		if _, err := db.Exec(q); err != nil {
			t.Fatal(q, err)
		}
	}
	if err := migrate(db, "", 0); err != nil {
		t.Fatal(err)
	}
	// Runs that existed before get the defaults, saves are not marked.
	var rules string
	var cheats int
	if err := db.QueryRow(`SELECT rules FROM runs WHERE id = 'r1'`).Scan(&rules); err != nil || rules != `{"states":false,"rewind":false,"cheats":"off","memory_write":"off"}` {
		t.Fatalf("rules %q: %v", rules, err)
	}
	if err := db.QueryRow(`SELECT cheats_used FROM save_slots`).Scan(&cheats); err != nil || cheats != 0 {
		t.Fatalf("cheats_used %d: %v", cheats, err)
	}
	// States: the number is limited to 0..10 and the primary key is user, game, slot and number.
	ins := `INSERT INTO save_states(user_id, game_id, slot, n, core_id, core_version, size_bytes, sha256, device_id, created_at) VALUES ('u1','g1','default',?,'c','1',5,'aa','d1',1)`
	for _, n := range []int{0, 10} {
		if _, err := db.Exec(ins, n); err != nil {
			t.Fatal(n, err)
		}
	}
	if _, err := db.Exec(ins, 11); err == nil {
		t.Fatal("state number 11 accepted")
	}
	if _, err := db.Exec(ins, 10); err == nil {
		t.Fatal("duplicate state accepted")
	}
}
