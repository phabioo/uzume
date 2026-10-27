package backup

import (
	"archive/tar"
	"bytes"
	"context"
	"errors"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

var bg = context.Background()

func write(t *testing.T, p, content string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(p), 0o750); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(p, []byte(content), 0o640); err != nil {
		t.Fatal(err)
	}
}

func read(t *testing.T, p string) string {
	t.Helper()
	b, err := os.ReadFile(p)
	if err != nil {
		t.Fatal(err)
	}
	return string(b)
}

// dataDir builds a synthetic data directory (no real game data).
func dataDir(t *testing.T) string {
	d := t.TempDir()
	write(t, filepath.Join(d, "uzume.db"), "DB-v1")
	write(t, filepath.Join(d, "saves", "u1", "g1", "main", "aaaa"), "save-one")
	write(t, filepath.Join(d, "saves", "u1", "g1", "main", "bbbb"), "save-two")
	write(t, filepath.Join(d, "plugins", "p", "1.0.0", "main.lua"), "-- plugin")
	write(t, filepath.Join(d, "firmware", "nds", "fw.bin"), "fw")
	write(t, filepath.Join(d, "screenshots", "u1", "g1", "s1.png"), "png-bytes")
	write(t, filepath.Join(d, "states", "u1", "g1", "main", "1.state"), "state-bytes")
	write(t, filepath.Join(d, "states", "u1", "g1", "main", "1.png"), "state-preview")
	write(t, filepath.Join(d, "tls", "cert.pem"), "CERT")
	write(t, filepath.Join(d, "tls", "key.pem"), "KEY")
	write(t, filepath.Join(d, "roms", "ab", "abcd"), "rom-bytes")
	write(t, filepath.Join(d, "cores", "x", "core.so"), "core") // never in a backup
	write(t, filepath.Join(d, "logs", "hub.log"), "log")        // never in a backup
	return d
}

func opts(t *testing.T, data, target string) Options {
	return Options{DataDir: data, TargetDir: target, HubName: "Home Hub", HubID: "hid", HubVersion: "1.2.3", SchemaVersion: 7,
		Snapshot: func(dest string) error {
			return os.WriteFile(dest, []byte(read(t, filepath.Join(data, "uzume.db"))), 0o600)
		},
		Now:  func() time.Time { return time.Date(2026, 10, 14, 1, 0, 0, 0, time.UTC) },
		Free: func(string) (uint64, bool) { return 1 << 40, true }}
}

func TestRoundTrip(t *testing.T) {
	data, target := dataDir(t), t.TempDir()
	res, err := Create(bg, opts(t, data, target))
	if err != nil {
		t.Fatal(err)
	}
	if res.Name != "uzume-server-home-hub-20261014T010000Z.tar" {
		t.Fatalf("name %q", res.Name)
	}
	m := res.Manifest
	if m.IncludesROMs || m.HasGroup(GroupROMs) {
		t.Fatalf("ROMs are off by default: %+v", m.Groups)
	}
	for _, n := range m.Members {
		if strings.Contains(n.Name, "cores/") || strings.Contains(n.Name, "logs/") || strings.Contains(n.Name, "roms/") {
			t.Fatalf("unexpected member %s", n.Name)
		}
	}
	if got := m.ContentsText(); got != "Database · saves · states · plugins · firmware · settings · screenshots" {
		t.Fatalf("contents %q", got)
	}
	// Nothing but the finished archive is left in the target.
	ents, _ := os.ReadDir(target)
	if len(ents) != 1 {
		t.Fatalf("target holds %d entries", len(ents))
	}
	rm, err := ReadManifest(res.Path)
	if err != nil || rm.SchemaVersion != 7 || rm.HubVersion != "1.2.3" || len(rm.Members) != len(m.Members) {
		t.Fatalf("manifest %+v %v", rm, err)
	}

	stage := filepath.Join(data, StagingDir)
	_, items, err := Extract(bg, res.Path, stage, ExtractOptions{MaxSchema: 7})
	if err != nil {
		t.Fatal(err)
	}
	if strings.Join(items, ",") != "firmware,plugins,saves,screenshots,states,tls,uzume.db" {
		t.Fatalf("items %v", items)
	}
	if read(t, filepath.Join(stage, "screenshots", "u1", "g1", "s1.png")) != "png-bytes" {
		t.Fatal("screenshots group missing")
	}
	if read(t, filepath.Join(stage, "states", "u1", "g1", "main", "1.state")) != "state-bytes" || read(t, filepath.Join(stage, "states", "u1", "g1", "main", "1.png")) != "state-preview" {
		t.Fatal("states group missing")
	}
	if read(t, filepath.Join(stage, "saves", "u1", "g1", "main", "bbbb")) != "save-two" || read(t, filepath.Join(stage, "uzume.db")) != "DB-v1" {
		t.Fatal("content differs")
	}
}

func TestIncludeROMsAndExtras(t *testing.T) {
	data, target := dataDir(t), t.TempDir()
	o := opts(t, data, target)
	o.IncludeROMs = true
	ext := filepath.Join(t.TempDir(), "server.env")
	write(t, ext, "SECRET=1")
	o.Extras = []Extra{{"server.env", ext}, {"missing.pem", filepath.Join(t.TempDir(), "nope")}}
	res, err := Create(bg, o)
	if err != nil {
		t.Fatal(err)
	}
	if !res.Manifest.HasGroup(GroupROMs) || !strings.HasSuffix(res.Manifest.ContentsText(), "ROMs") {
		t.Fatalf("%+v", res.Manifest.Groups)
	}
	stage := filepath.Join(data, StagingDir)
	if _, _, err := Extract(bg, res.Path, stage, ExtractOptions{}); err != nil {
		t.Fatal(err)
	}
	if read(t, filepath.Join(stage, "roms", "ab", "abcd")) != "rom-bytes" || read(t, filepath.Join(stage, "restored-config", "server.env")) != "SECRET=1" {
		t.Fatal("ROM or extra missing")
	}
	if _, err := os.Stat(filepath.Join(stage, "restored-config", "missing.pem")); err == nil {
		t.Fatal("absent extra must be left out")
	}
}

// corrupt flips one byte of the payload of member name inside the tar file.
func corrupt(t *testing.T, file, name string) {
	t.Helper()
	b, err := os.ReadFile(file)
	if err != nil {
		t.Fatal(err)
	}
	// Simple and reliable: locate the member's content in the file and flip a byte.
	tr := tar.NewReader(bytes.NewReader(b))
	var want []byte
	for {
		h, err := tr.Next()
		if err != nil {
			t.Fatal(err)
		}
		if h.Name == name {
			want, _ = io.ReadAll(tr)
			break
		}
	}
	i := bytes.Index(b, want)
	if i < 0 || len(want) == 0 {
		t.Fatal("content not found")
	}
	b[i] ^= 0xff
	if err := os.WriteFile(file, b, 0o600); err != nil {
		t.Fatal(err)
	}
}

func TestChecksumMismatchChangesNothing(t *testing.T) {
	data, target := dataDir(t), t.TempDir()
	res, err := Create(bg, opts(t, data, target))
	if err != nil {
		t.Fatal(err)
	}
	corrupt(t, res.Path, "data/saves/u1/g1/main/aaaa")
	stage := filepath.Join(data, StagingDir)
	_, _, err = Extract(bg, res.Path, stage, ExtractOptions{})
	var ce *ChecksumError
	if !errors.As(err, &ce) || ce.Group != GroupSaves {
		t.Fatalf("want checksum error in saves, got %v", err)
	}
	if !strings.Contains(err.Error(), "checksum mismatch in saves") {
		t.Fatal(err)
	}
	if _, err := os.Stat(stage); err == nil {
		t.Fatal("staging must be removed")
	}
	if read(t, filepath.Join(data, "uzume.db")) != "DB-v1" || read(t, filepath.Join(data, "saves", "u1", "g1", "main", "aaaa")) != "save-one" {
		t.Fatal("live data changed")
	}
	if HasPending(data) {
		t.Fatal("no restore may be pending")
	}
	// Verify-only finds it too.
	if _, _, err := Extract(bg, res.Path, "", ExtractOptions{VerifyOnly: true}); !errors.As(err, &ce) {
		t.Fatalf("verify: %v", err)
	}
}

func TestTruncatedAndGarbageAreDamaged(t *testing.T) {
	data, target := dataDir(t), t.TempDir()
	res, _ := Create(bg, opts(t, data, target))
	b, _ := os.ReadFile(res.Path)
	cut := filepath.Join(t.TempDir(), "cut.tar")
	os.WriteFile(cut, b[:len(b)/2], 0o600)
	var de *DamagedError
	if _, err := ReadManifest(cut); !errors.As(err, &de) {
		t.Fatalf("truncated: %v", err)
	}
	junk := filepath.Join(t.TempDir(), "junk.tar")
	os.WriteFile(junk, []byte("this is not a tar file at all"), 0o600)
	if _, _, err := Extract(bg, junk, filepath.Join(data, StagingDir), ExtractOptions{}); !errors.As(err, &de) {
		t.Fatalf("junk: %v", err)
	}
	infos, _ := List(filepath.Dir(res.Path))
	if len(infos) != 1 || infos[0].Err != "" {
		t.Fatalf("%+v", infos)
	}
}

func TestNewerSchemaRejectedBeforeAnything(t *testing.T) {
	data, target := dataDir(t), t.TempDir()
	o := opts(t, data, target)
	o.SchemaVersion = 9
	res, err := Create(bg, o)
	if err != nil {
		t.Fatal(err)
	}
	stage := filepath.Join(data, StagingDir)
	_, _, err = Extract(bg, res.Path, stage, ExtractOptions{MaxSchema: 8})
	var ne *NewerError
	if !errors.As(err, &ne) || ne.Schema != 9 || ne.Known != 8 {
		t.Fatalf("%v", err)
	}
	if _, err := os.Stat(stage); err == nil {
		t.Fatal("nothing may be unpacked")
	}
}

func TestNoSpaceFailsAndDeletesNothing(t *testing.T) {
	data, target := dataDir(t), t.TempDir()
	old := filepath.Join(target, FileName("Home Hub", time.Date(2026, 10, 1, 1, 0, 0, 0, time.UTC), false))
	write(t, old, "old backup")
	o := opts(t, data, target)
	o.Free = func(string) (uint64, bool) { return 100, true }
	_, err := Create(bg, o)
	var ns *NoSpaceError
	if !errors.As(err, &ns) || ns.Free != 100 {
		t.Fatalf("%v", err)
	}
	if !strings.HasPrefix(err.Error(), "Not enough space on "+target+": needs ") {
		t.Fatal(err.Error())
	}
	ents, _ := os.ReadDir(target)
	if len(ents) != 1 || read(t, old) != "old backup" {
		t.Fatalf("the old backup must stay and no partial may remain: %v", ents)
	}
}

func TestCancelRemovesPartial(t *testing.T) {
	data, target := dataDir(t), t.TempDir()
	ctx, cancel := context.WithCancel(bg)
	o := opts(t, data, target)
	o.Progress = func(p Progress) {
		if p.Phase == "plugins" {
			cancel()
		}
	}
	_, err := Create(ctx, o)
	if !errors.Is(err, context.Canceled) {
		t.Fatalf("%v", err)
	}
	if ents, _ := os.ReadDir(target); len(ents) != 0 {
		t.Fatalf("left over: %v", ents)
	}
}

func TestNotWritable(t *testing.T) {
	data := dataDir(t)
	blocker := filepath.Join(t.TempDir(), "file")
	write(t, blocker, "x")
	_, err := Create(bg, opts(t, data, filepath.Join(blocker, "sub"))) // a folder below a file
	var nw *NotWritableError
	if !errors.As(err, &nw) {
		t.Fatalf("%v", err)
	}
}

func TestPruneKeepsSafetyAndNewest(t *testing.T) {
	dir := t.TempDir()
	base := time.Date(2026, 10, 1, 3, 0, 0, 0, time.UTC)
	for i := 0; i < 5; i++ {
		write(t, filepath.Join(dir, FileName("Hub", base.AddDate(0, 0, i), false)), "x")
	}
	safety := FileName("Hub", base, true)
	write(t, filepath.Join(dir, safety), "x")
	write(t, filepath.Join(dir, "notes.txt"), "keep me")
	del, err := Prune(dir, 2)
	if err != nil || len(del) != 3 {
		t.Fatalf("%v %v", del, err)
	}
	left, _ := os.ReadDir(dir)
	names := map[string]bool{}
	for _, e := range left {
		names[e.Name()] = true
	}
	if !names[safety] || !names["notes.txt"] || !names[FileName("Hub", base.AddDate(0, 0, 4), false)] || !names[FileName("Hub", base.AddDate(0, 0, 3), false)] || len(names) != 4 {
		t.Fatalf("%v", names)
	}
}

func TestNames(t *testing.T) {
	if Slug("Home Hub!") != "home-hub" || Slug("  ") != "hub" || Slug("Wohnzimmer-Äpfel") != "wohnzimmer-pfel" {
		t.Fatal("slug")
	}
	n := FileName("Home Hub", time.Date(2026, 10, 14, 18, 52, 3, 0, time.UTC), true)
	if n != "uzume-server-home-hub-20261014T185203Z-safety.tar" {
		t.Fatal(n)
	}
	ts, safety, ok := ParseName(n)
	if !ok || !safety || ts.Hour() != 18 {
		t.Fatal("parse")
	}
	if _, safety, ok := ParseName("uzume-server-home-hub-20261014T185203Z-safety.tar"); !ok || !safety {
		t.Fatal("archives of versions before the rename must still parse")
	}
	for _, bad := range []string{"x.tar", "../uzume-server-a-20261014T185203Z.tar", "uzume-server-a-20261014T185203Z.tar.gz"} {
		if _, _, ok := ParseName(bad); ok {
			t.Fatal(bad)
		}
	}
	if _, err := Find(t.TempDir(), "../x"); err == nil {
		t.Fatal("find must reject")
	}
}

func TestPathTraversalMemberRejected(t *testing.T) {
	var buf bytes.Buffer
	tw := tar.NewWriter(&buf)
	tw.WriteHeader(&tar.Header{Name: "data/saves/../../evil", Mode: 0o600, Size: 1, Typeflag: tar.TypeReg})
	tw.Write([]byte("x"))
	js := `{"format":1,"schema_version":1,"groups":["database"],"members":[{"name":"data/saves/../../evil","size":1,"sha256":"00","group":"saves"}]}`
	tw.WriteHeader(&tar.Header{Name: ManifestName, Mode: 0o600, Size: int64(len(js)), Typeflag: tar.TypeReg})
	tw.Write([]byte(js))
	tw.Close()
	f := filepath.Join(t.TempDir(), "evil.tar")
	os.WriteFile(f, buf.Bytes(), 0o600)
	data := t.TempDir()
	_, _, err := Extract(bg, f, filepath.Join(data, StagingDir), ExtractOptions{})
	var de *DamagedError
	if !errors.As(err, &de) {
		t.Fatalf("%v", err)
	}
}

// ---- restore swap ----

func prepare(t *testing.T, data string) (res Result, stage string, items []string) {
	t.Helper()
	// Backup of the "old" state, then change the live data.
	res, err := Create(bg, opts(t, data, t.TempDir()))
	if err != nil {
		t.Fatal(err)
	}
	write(t, filepath.Join(data, "uzume.db"), "DB-v2")
	write(t, filepath.Join(data, "uzume.db-wal"), "wal")
	write(t, filepath.Join(data, "saves", "u1", "g1", "main", "cccc"), "newer-save")
	stage = filepath.Join(data, StagingDir)
	_, items, err = Extract(bg, res.Path, stage, ExtractOptions{})
	if err != nil {
		t.Fatal(err)
	}
	return
}

func TestApplyPendingSwaps(t *testing.T) {
	data := dataDir(t)
	res, stage, items := prepare(t, data)
	if err := WritePending(data, Pending{Staging: stage, Items: items, Backup: res.Name, BackupCreated: res.Manifest.Created, Safety: "s.tar"}); err != nil {
		t.Fatal(err)
	}
	r, err := ApplyPending(data, time.Now())
	if err != nil || r == nil || !r.OK {
		t.Fatalf("%+v %v", r, err)
	}
	if read(t, filepath.Join(data, "uzume.db")) != "DB-v1" {
		t.Fatal("database not restored")
	}
	if _, err := os.Stat(filepath.Join(data, "uzume.db-wal")); err == nil {
		t.Fatal("wal must go away with the old database")
	}
	if _, err := os.Stat(filepath.Join(data, "saves", "u1", "g1", "main", "cccc")); err == nil {
		t.Fatal("newer save must be gone")
	}
	if read(t, filepath.Join(data, "roms", "ab", "abcd")) != "rom-bytes" || read(t, filepath.Join(data, "cores", "x", "core.so")) != "core" {
		t.Fatal("ROMs (not in the backup) and cores must stay")
	}
	for _, d := range []string{StagingDir, oldDir} {
		if _, err := os.Stat(filepath.Join(data, d)); err == nil {
			t.Fatalf("%s left", d)
		}
	}
	if HasPending(data) {
		t.Fatal("still pending")
	}
	if got := ReadResult(data); got == nil || !got.OK || got.Safety != "s.tar" {
		t.Fatalf("%+v", got)
	}
	if again, err := ApplyPending(data, time.Now()); again != nil || err != nil {
		t.Fatal("nothing pending any more")
	}
}

func TestApplyPendingRollsBackOnFailure(t *testing.T) {
	data := dataDir(t)
	res, stage, items := prepare(t, data)
	// The staged plugins directory vanishes: the swap fails after the database and saves were already replaced.
	if err := os.RemoveAll(filepath.Join(stage, "plugins")); err != nil {
		t.Fatal(err)
	}
	WritePending(data, Pending{Staging: stage, Items: items, Backup: res.Name, BackupCreated: res.Manifest.Created})
	r, err := ApplyPending(data, time.Now())
	if err == nil || r == nil || r.OK {
		t.Fatalf("want failure: %+v %v", r, err)
	}
	if read(t, filepath.Join(data, "uzume.db")) != "DB-v2" || read(t, filepath.Join(data, "uzume.db-wal")) != "wal" {
		t.Fatal("database must be as before")
	}
	if read(t, filepath.Join(data, "saves", "u1", "g1", "main", "cccc")) != "newer-save" || read(t, filepath.Join(data, "plugins", "p", "1.0.0", "main.lua")) != "-- plugin" {
		t.Fatal("data must be as before")
	}
	if got := ReadResult(data); got == nil || got.OK || got.Error == "" {
		t.Fatalf("%+v", got)
	}
}

func TestApplyPendingResumesAfterCrash(t *testing.T) {
	data := dataDir(t)
	res, stage, items := prepare(t, data)
	WritePending(data, Pending{Staging: stage, Items: items, Backup: res.Name, BackupCreated: res.Manifest.Created})
	// Simulate a crash after the database and saves were swapped but before COMMITTED.
	oldRoot := filepath.Join(data, oldDir)
	os.MkdirAll(oldRoot, 0o750)
	for _, w := range walFiles {
		if exists(filepath.Join(data, w)) {
			os.Rename(filepath.Join(data, w), filepath.Join(oldRoot, w))
		}
	}
	for _, it := range []string{"uzume.db", "saves"} {
		os.Rename(filepath.Join(data, it), filepath.Join(oldRoot, it))
		os.Rename(filepath.Join(stage, it), filepath.Join(data, it))
	}
	r, err := ApplyPending(data, time.Now())
	if err != nil || !r.OK {
		t.Fatalf("%+v %v", r, err)
	}
	if read(t, filepath.Join(data, "uzume.db")) != "DB-v1" || read(t, filepath.Join(data, "tls", "key.pem")) != "KEY" {
		t.Fatal("not fully restored")
	}
	if _, err := os.Stat(filepath.Join(data, "saves", "u1", "g1", "main", "cccc")); err == nil {
		t.Fatal("newer save must be gone")
	}
}

func TestApplyPendingRemovesItemsTheBackupDidNotHave(t *testing.T) {
	data := dataDir(t)
	os.RemoveAll(filepath.Join(data, "plugins"))
	res, err := Create(bg, opts(t, data, t.TempDir()))
	if err != nil {
		t.Fatal(err)
	}
	write(t, filepath.Join(data, "plugins", "new", "main.lua"), "later")
	stage := filepath.Join(data, StagingDir)
	_, items, err := Extract(bg, res.Path, stage, ExtractOptions{})
	if err != nil {
		t.Fatal(err)
	}
	WritePending(data, Pending{Staging: stage, Items: items, Backup: res.Name})
	if r, err := ApplyPending(data, time.Now()); err != nil || !r.OK {
		t.Fatal(err)
	}
	if ents, _ := os.ReadDir(filepath.Join(data, "plugins")); len(ents) != 0 {
		t.Fatal("plugins of later must be gone (empty directory as in the backup)")
	}
}

// ---- schedule ----

func TestNextRun(t *testing.T) {
	utc := time.UTC
	at := func(y int, m time.Month, d, h, mi int) time.Time { return time.Date(y, m, d, h, mi, 0, 0, utc) }
	if got := NextRun(at(2026, 10, 14, 1, 0), utc, ModeDaily, 0, 3, 0); !got.Equal(at(2026, 10, 14, 3, 0)) {
		t.Fatal(got)
	}
	if got := NextRun(at(2026, 10, 14, 3, 0), utc, ModeDaily, 0, 3, 0); !got.Equal(at(2026, 10, 15, 3, 0)) {
		t.Fatalf("exactly at the time is the next day: %v", got)
	}
	// 2026-10-14 is a Wednesday. Weekly on Monday: 19 Oct; on Wednesday after the time: 21 Oct.
	if got := NextRun(at(2026, 10, 14, 1, 0), utc, ModeWeekly, time.Monday, 3, 0); !got.Equal(at(2026, 10, 19, 3, 0)) {
		t.Fatal(got)
	}
	if got := NextRun(at(2026, 10, 14, 4, 0), utc, ModeWeekly, time.Wednesday, 3, 0); !got.Equal(at(2026, 10, 21, 3, 0)) {
		t.Fatal(got)
	}
	if !NextRun(at(2026, 10, 14, 1, 0), utc, ModeOff, 0, 3, 0).IsZero() {
		t.Fatal("off")
	}
}

func TestNextRunAcrossDST(t *testing.T) {
	berlin, err := time.LoadLocation("Europe/Berlin")
	if err != nil {
		t.Skip("no tzdata")
	}
	// Clocks go forward on 2026-03-29 (02:00 -> 03:00) and back on 2026-10-25 (03:00 -> 02:00).
	now := time.Date(2026, 3, 28, 4, 0, 0, 0, berlin)
	next := NextRun(now, berlin, ModeDaily, 0, 3, 0)
	if next.Hour() != 3 || next.Day() != 29 || next.Sub(now) != 22*time.Hour {
		t.Fatalf("spring forward: %v (%v from now)", next, next.Sub(now))
	}
	// The day after, the run is still at 03:00 local: 23 hours later in absolute time.
	n2 := NextRun(next, berlin, ModeDaily, 0, 3, 0)
	if n2.Hour() != 3 || n2.Day() != 30 || n2.Sub(next) != 24*time.Hour {
		t.Fatalf("after spring: %v", n2)
	}
	// A time that does not exist on the day clocks go forward still runs once that day.
	skip := NextRun(time.Date(2026, 3, 28, 12, 0, 0, 0, berlin), berlin, ModeDaily, 0, 2, 30)
	if skip.Day() != 29 || !skip.After(time.Date(2026, 3, 28, 12, 0, 0, 0, berlin)) || skip.Sub(time.Date(2026, 3, 28, 12, 0, 0, 0, berlin)) > 24*time.Hour {
		t.Fatalf("gap: %v", skip)
	}
	// Fall back: 03:00 CET on the long day is 3.5 hours after 00:30 CEST, not 2.5.
	now = time.Date(2026, 10, 25, 0, 30, 0, 0, berlin)
	next = NextRun(now, berlin, ModeDaily, 0, 3, 0)
	if next.Hour() != 3 || next.Day() != 25 || next.Sub(now) != 3*time.Hour+30*time.Minute {
		t.Fatalf("fall back: %v", next)
	}
	n2 = NextRun(next, berlin, ModeDaily, 0, 3, 0)
	if n2.Hour() != 3 || n2.Day() != 26 || n2.Sub(next) != 24*time.Hour {
		t.Fatalf("after fall back: %v", n2)
	}
}
