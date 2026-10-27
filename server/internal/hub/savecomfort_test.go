package hub_test

import (
	"bytes"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/phabioo/uzume/server/internal/hub"
	"github.com/phabioo/uzume/server/internal/hub/hubtest"
)

func newComfortEnv(t *testing.T, mod func(*hub.Options)) (*savesEnv, *hubtest.Clock) {
	t.Helper()
	svc, clk := hubtest.New(t, mod)
	u, err := svc.CreateAdmin(ctx, "fabio", "secret-12345")
	if err != nil {
		t.Fatal(err)
	}
	g, err := svc.AddROM(ctx, bytes.NewReader(randomROM(2000)), "demo.nds", "", "", u.ID)
	if err != nil {
		t.Fatal(err)
	}
	return &savesEnv{t: t, svc: svc, user: u, game: g,
		devA: pairDevice(t, svc, u.ID, "Desktop"), devB: pairDevice(t, svc, u.ID, "Laptop")}, clk
}

func (e *savesEnv) restore(dev string, version, expected int) (hub.SaveSlot, error) {
	return e.svc.RestoreSaveVersion(ctx, hub.RestoreInput{UserID: e.user.ID, DeviceID: dev, GameID: e.game.ID, Slot: "default",
		Version: version, ExpectedRevision: expected})
}

func (e *savesEnv) fileExists(data []byte) bool {
	_, err := os.Stat(filepath.Join(e.svc.DataDir(), "saves", e.user.ID, e.game.ID, "default", hexSHA(data)))
	return err == nil
}

func (e *savesEnv) reasons() []string {
	var out []string
	for _, v := range e.history() {
		out = append(out, v.Reason)
	}
	return out
}

func TestRestoreSecuresCurrentAndCreatesNewCheckpoint(t *testing.T) {
	e, _ := newComfortEnv(t, nil)
	e.put(e.devA, 0, []byte("one"), hub.SyncFinalSessionEnd) // Rev 1, v1 session_end
	e.put(e.devA, 1, []byte("two"), hub.SyncCheckpoint)      // Rev 2, same device: not in history
	sl, err := e.restore(e.devB, 1, 2)
	if err != nil {
		t.Fatal(err)
	}
	c := sl.Current
	if c.Revision != 3 || c.Reason != hub.SyncRestore || c.DeviceID != e.devB || c.DeviceName != "Laptop" || c.SHA256 != hexSHA([]byte("one")) {
		t.Fatalf("%+v", c)
	}
	if string(e.content()) != "one" {
		t.Fatalf("content %q", e.content())
	}
	h := e.history() // newest first
	if len(h) != 2 || h[0].Reason != hub.HistoryBeforeRestore || h[0].Revision != 2 || h[0].SHA256 != hexSHA([]byte("two")) {
		t.Fatalf("%+v", h)
	}
	// Restoring the secured version again: Rev 3 is already captured by nothing yet (restore content "one" = v1), so it
	// gets a before_restore; after a manual snapshot no second capture is made.
	if _, err := e.svc.CreateSaveSnapshot(ctx, e.user.ID, e.game.ID, "default", nil); err != nil {
		t.Fatal(err)
	}
	n := len(e.history())
	sl, err = e.restore(e.devA, h[0].Version, 3)
	if err != nil || sl.Current.Revision != 4 || string(e.content()) != "two" || sl.Current.DeviceID != e.devA {
		t.Fatalf("%+v %v", sl.Current, err)
	}
	if len(e.history()) != n {
		t.Fatalf("current checkpoint was captured twice: %v", e.reasons())
	}
	// An upload based on the restored revision is accepted; one based on the old revision conflicts.
	if r := e.put(e.devA, 4, []byte("three"), hub.SyncCheckpoint); r.Conflict != nil {
		t.Fatal("unexpected conflict")
	}
	if r := e.put(e.devB, 3, []byte("four"), hub.SyncCheckpoint); r.Conflict == nil {
		t.Fatal("expected a conflict")
	}
}

func TestRestoreStaleUnknownAndInvalid(t *testing.T) {
	e, _ := newComfortEnv(t, nil)
	e.put(e.devA, 0, []byte("one"), hub.SyncFinalSessionEnd)
	e.put(e.devA, 1, []byte("two"), hub.SyncCheckpoint)
	if _, err := e.restore(e.devA, 1, 1); !errors.Is(err, hub.ErrSaveConflictStale) {
		t.Fatalf("stale: %v", err)
	}
	if _, err := e.restore(e.devA, 99, 2); !errors.Is(err, hub.ErrNotFound) {
		t.Fatalf("unknown version: %v", err)
	}
	if _, err := e.restore(e.devA, 0, 2); !errors.Is(err, hub.ErrNotFound) {
		t.Fatalf("version 0: %v", err)
	}
	if _, err := e.restore(e.devA, 1, 0); !errors.Is(err, hub.ErrBadRequest) {
		t.Fatalf("expected revision 0: %v", err)
	}
	if _, err := e.svc.RestoreSaveVersion(ctx, hub.RestoreInput{UserID: e.user.ID, DeviceID: e.devA, GameID: e.game.ID, Slot: "other",
		Version: 1, ExpectedRevision: 1}); !errors.Is(err, hub.ErrNotFound) {
		t.Fatalf("unknown slot: %v", err)
	}
	// Other users cannot restore.
	other, err := e.svc.CreateUser(ctx, "anna", "Anna")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := e.svc.RestoreSaveVersion(ctx, hub.RestoreInput{UserID: other.ID, DeviceID: e.devA, GameID: e.game.ID, Slot: "default",
		Version: 1, ExpectedRevision: 2}); !errors.Is(err, hub.ErrNotFound) {
		t.Fatalf("other user: %v", err)
	}
	// Nothing changed.
	sl, _ := e.svc.GetSaveSlot(ctx, e.user.ID, e.game.ID, "default")
	if sl.Current.Revision != 2 || len(e.history()) != 1 {
		t.Fatalf("%+v %v", sl.Current, e.reasons())
	}
}

func TestSnapshotLabelAndErrors(t *testing.T) {
	e, _ := newComfortEnv(t, nil)
	snap := func(label *string) (hub.SaveVersion, error) {
		return e.svc.CreateSaveSnapshot(ctx, e.user.ID, e.game.ID, "default", label)
	}
	s := func(v string) *string { return &v }
	if _, err := snap(nil); !errors.Is(err, hub.ErrNotFound) {
		t.Fatalf("no checkpoint: %v", err)
	}
	e.put(e.devA, 0, []byte("one"), hub.SyncCheckpoint)
	v, err := snap(s("  before the boss  "))
	if err != nil || v.Reason != hub.HistoryManualSnapshot || v.Label == nil || *v.Label != "before the boss" || v.Revision != 1 ||
		v.SHA256 != hexSHA([]byte("one")) || v.DeviceName != "Desktop" {
		t.Fatalf("%+v %v", v, err)
	}
	if v, err = snap(s("   ")); err != nil || v.Label != nil {
		t.Fatalf("blank label: %+v %v", v, err)
	}
	if v, err = snap(nil); err != nil || v.Label != nil || v.Version != 3 {
		t.Fatalf("no label: %+v %v", v, err)
	}
	if v, err = snap(s(strings.Repeat("ä", 120))); err != nil || *v.Label != strings.Repeat("ä", 120) {
		t.Fatalf("120 characters: %v", err)
	}
	if _, err = snap(s(strings.Repeat("x", 121))); !errors.Is(err, hub.ErrBadRequest) {
		t.Fatalf("121 characters: %v", err)
	}
	if _, err = e.svc.CreateSaveSnapshot(ctx, e.user.ID, e.game.ID, "nope", nil); !errors.Is(err, hub.ErrNotFound) {
		t.Fatalf("unknown slot: %v", err)
	}
	if h := e.history(); len(h) != 4 || h[3].Label == nil || *h[3].Label != "before the boss" {
		t.Fatalf("history %+v", h)
	}
}

func TestRetentionRules(t *testing.T) {
	e, clk := newComfortEnv(t, nil) // no retention while the history is built
	steps := []struct {
		after time.Duration // clock advance before the upload
		name  string
		snap  bool // a labelled manual snapshot right after
	}{
		{0, "c1", false},                  // 10-05 12:00
		{1 * time.Hour, "c2", true},       // 10-05 13:00 (+ snapshot of c2)
		{23 * time.Hour, "c3", false},     // 10-06 12:00
		{6 * 24 * time.Hour, "c4", false}, // 10-12 12:00
		{24 * time.Hour, "c5", false},     // 10-13 12:00
		{6 * 24 * time.Hour, "c6", false}, // 10-19 12:00
		{24 * time.Hour, "c7", false},     // 10-20 12:00
		{4 * 24 * time.Hour, "c8", false}, // 10-24 12:00
	}
	base := 0
	for _, st := range steps {
		clk.Advance(st.after)
		e.put(e.devA, base, []byte(st.name), hub.SyncFinalSessionEnd)
		base++
		if st.snap {
			l := "keep me"
			if _, err := e.svc.CreateSaveSnapshot(ctx, e.user.ID, e.game.ID, "default", &l); err != nil {
				t.Fatal(err)
			}
		}
	}
	if n := len(e.history()); n != 9 {
		t.Fatalf("history before thinning: %d", n)
	}
	clk.Advance(24 * time.Hour) // now: 10-25 12:00

	// recent 2, daily 2, weekly 2: kept are c8 and c7 (recent), c8 (day 10-24), c8 (week of 10-19) and c5 (week of 10-12);
	// the snapshot stays although it is old.
	e.svc.SetSaveRetentionForTest(2, 2, 2)
	if err := e.svc.SweepSaves(ctx); err != nil {
		t.Fatal(err)
	}
	var kept []string
	snapshots := 0
	for _, v := range e.history() {
		if v.Reason == hub.HistoryManualSnapshot {
			snapshots++
			if v.Label == nil || *v.Label != "keep me" {
				t.Fatalf("snapshot %+v", v)
			}
			continue
		}
		for _, st := range steps {
			if v.SHA256 == hexSHA([]byte(st.name)) {
				kept = append(kept, st.name)
			}
		}
	}
	if strings.Join(kept, ",") != "c8,c7,c5" || snapshots != 1 {
		t.Fatalf("kept %v, snapshots %d", kept, snapshots)
	}
	// Content files: only referenced ones remain (c2 is held by the snapshot, c8 by the checkpoint).
	for _, st := range steps {
		want := map[string]bool{"c2": true, "c5": true, "c7": true, "c8": true}[st.name]
		if got := e.fileExists([]byte(st.name)); got != want {
			t.Errorf("file %s exists=%v, want %v", st.name, got, want)
		}
	}
	if string(e.content()) != "c8" {
		t.Fatal("checkpoint content changed")
	}
	// A second sweep changes nothing.
	if err := e.svc.SweepSaves(ctx); err != nil || len(e.history()) != 4 {
		t.Fatalf("second sweep: %v %d", err, len(e.history()))
	}
}

func TestRetentionUnlimitedRules(t *testing.T) {
	e, clk := newComfortEnv(t, func(o *hub.Options) { o.SaveKeepRecent, o.SaveKeepDaily, o.SaveKeepWeekly = 1, 0, 0 })
	// Daily/weekly unlimited: the newest version of each day is kept whatever its age; same-day versions are thinned.
	e.put(e.devA, 0, []byte("d1a"), hub.SyncFinalSessionEnd)
	clk.Advance(time.Hour)
	e.put(e.devA, 1, []byte("d1b"), hub.SyncFinalSessionEnd)
	clk.Advance(48 * time.Hour)
	e.put(e.devA, 2, []byte("d3a"), hub.SyncFinalSessionEnd)
	clk.Advance(400 * 24 * time.Hour)
	e.put(e.devA, 3, []byte("late"), hub.SyncFinalSessionEnd)
	var got []string
	for _, v := range e.history() {
		got = append(got, string(v.SHA256[:4]))
	}
	want := []string{hexSHA([]byte("late"))[:4], hexSHA([]byte("d3a"))[:4], hexSHA([]byte("d1b"))[:4]}
	if strings.Join(got, ",") != strings.Join(want, ",") {
		t.Fatalf("got %v want %v", got, want)
	}
	// recent 0 means no thinning at all.
	e.svc.SetSaveRetentionForTest(0, 1, 1)
	e.put(e.devA, 4, []byte("x1"), hub.SyncFinalSessionEnd)
	e.put(e.devA, 5, []byte("x2"), hub.SyncFinalSessionEnd)
	if n := len(e.history()); n != 5 {
		t.Fatalf("history %d", n)
	}
}

func TestRetentionKeepsVersionsOfOpenConflicts(t *testing.T) {
	e, _ := newComfortEnv(t, func(o *hub.Options) { o.SaveKeepRecent, o.SaveKeepDaily, o.SaveKeepWeekly = 1, 0, 0 })
	e.put(e.devA, 0, []byte("a1"), hub.SyncFinalSessionEnd)
	r := e.put(e.devB, 0, []byte("b1"), hub.SyncCheckpoint) // stale: secured as conflict upload
	if r.Conflict == nil {
		t.Fatal("expected a conflict")
	}
	e.put(e.devA, 1, []byte("a2"), hub.SyncFinalSessionEnd)
	e.put(e.devA, 2, []byte("a3"), hub.SyncFinalSessionEnd)
	// a1 and a2 are thinned, the secured upload stays together with its file.
	h := e.history()
	if len(h) != 2 || h[0].SHA256 != hexSHA([]byte("a3")) || h[1].Reason != hub.HistoryConflictUpload || !e.fileExists([]byte("b1")) {
		t.Fatalf("history %v", e.reasons())
	}
	if e.fileExists([]byte("a1")) || e.fileExists([]byte("a2")) {
		t.Fatal("thinned content files remain")
	}
	// Resolving releases the protection (the thinning runs right after the resolution).
	if _, err := e.resolve(r.Conflict.ID, hub.ResolveUseHub, 3, "user:x"); err != nil {
		t.Fatal(err)
	}
	if h := e.history(); len(h) != 1 || e.fileExists([]byte("b1")) {
		t.Fatalf("history %v, b1 exists %v", e.reasons(), e.fileExists([]byte("b1")))
	}
}

func TestRestoreFromWebRecordsWebDevice(t *testing.T) {
	e, _ := newComfortEnv(t, nil)
	e.put(e.devA, 0, []byte("one"), hub.SyncFinalSessionEnd)
	sl, err := e.svc.RestoreSaveVersion(ctx, hub.RestoreInput{UserID: e.user.ID, DeviceID: hub.AdminDeviceID(e.user.ID), GameID: e.game.ID,
		Slot: "default", Version: 1, ExpectedRevision: 1})
	if err != nil || sl.Current.DeviceName != "Server admin" {
		t.Fatalf("%+v %v", sl.Current, err)
	}
}

func (e *savesEnv) fileExistsIn(slot string, data []byte) bool {
	_, err := os.Stat(filepath.Join(e.svc.DataDir(), "saves", e.user.ID, e.game.ID, slot, hexSHA(data)))
	return err == nil
}

func TestDeleteSaveVersion(t *testing.T) {
	e, _ := newComfortEnv(t, nil)
	e.put(e.devA, 0, []byte("one"), hub.SyncFinalSessionEnd) // Rev 1, v1 session_end
	e.put(e.devB, 1, []byte("two"), hub.SyncFinalSessionEnd) // Rev 2, v2 device_change/session_end of Rev 1 is already v1
	u, g := e.user.ID, e.game.ID
	lbl := "boss"
	snap, err := e.svc.CreateSaveSnapshot(ctx, u, g, "default", &lbl) // snapshot of "two" (still the checkpoint)
	if err != nil {
		t.Fatal(err)
	}
	// Missing slot / version -> not found.
	if err := e.svc.DeleteSaveVersion(ctx, u, g, "default", 99); !errors.Is(err, hub.ErrNotFound) {
		t.Fatal(err)
	}
	if err := e.svc.DeleteSaveVersion(ctx, u, g, "nothere", 1); !errors.Is(err, hub.ErrNotFound) {
		t.Fatal(err)
	}
	if err := e.svc.DeleteSaveVersion(ctx, u, g, "Bad Slot", 1); !errors.Is(err, hub.ErrNotFound) {
		t.Fatal(err)
	}
	if err := e.svc.DeleteSaveVersion(ctx, "other-user", g, "default", snap.Version); !errors.Is(err, hub.ErrNotFound) {
		t.Fatal(err)
	}
	n := len(e.history())
	// The snapshot shares its content with the checkpoint: the file stays.
	if err := e.svc.DeleteSaveVersion(ctx, u, g, "default", snap.Version); err != nil {
		t.Fatal(err)
	}
	if len(e.history()) != n-1 || !e.fileExists([]byte("two")) {
		t.Fatalf("history %d, file %v", len(e.history()), e.fileExists([]byte("two")))
	}
	if sl, _ := e.svc.GetSaveSlot(ctx, u, g, "default"); sl.Current.Revision != 2 || string(e.content()) != "two" {
		t.Fatalf("%+v", sl.Current)
	}
	if err := e.svc.DeleteSaveVersion(ctx, u, g, "default", snap.Version); !errors.Is(err, hub.ErrNotFound) {
		t.Fatal(err)
	}
}

func TestDeleteSaveVersionDropsUnreferencedFile(t *testing.T) {
	e, _ := newComfortEnv(t, nil)
	e.put(e.devA, 0, []byte("one"), hub.SyncCheckpoint) // Rev 1
	u, g := e.user.ID, e.game.ID
	snap, err := e.svc.CreateSaveSnapshot(ctx, u, g, "default", nil) // v1 of "one"
	if err != nil {
		t.Fatal(err)
	}
	e.put(e.devA, 1, []byte("two"), hub.SyncCheckpoint) // Rev 2: "one" only in the snapshot now
	if !e.fileExists([]byte("one")) {
		t.Fatal("snapshot content missing")
	}
	if err := e.svc.DeleteSaveVersion(ctx, u, g, "default", snap.Version); err != nil {
		t.Fatal(err)
	}
	if e.fileExists([]byte("one")) || !e.fileExists([]byte("two")) {
		t.Fatalf("one=%v two=%v", e.fileExists([]byte("one")), e.fileExists([]byte("two")))
	}
}

func TestCreateSaveSlot(t *testing.T) {
	e, _ := newComfortEnv(t, nil)
	e.put(e.devA, 0, []byte("one"), hub.SyncCheckpoint)
	u, g, web := e.user.ID, e.game.ID, hub.AdminDeviceID(e.user.ID)
	sl, err := e.svc.CreateSaveSlot(ctx, u, g, "default", "speedrun", web)
	if err != nil {
		t.Fatal(err)
	}
	c := sl.Current
	if sl.Slot != "speedrun" || c.Revision != 1 || c.Reason != hub.SyncRestore || c.DeviceID != web || c.SHA256 != hexSHA([]byte("one")) || c.Size != 3 {
		t.Fatalf("%+v", sl)
	}
	h, err := e.svc.ListSaveHistory(ctx, u, g, "speedrun")
	if err != nil || len(h) != 1 {
		t.Fatalf("%+v %v", h, err)
	}
	if v := h[0]; v.Version != 1 || v.Revision != 1 || v.Reason != hub.HistoryManualSnapshot || v.Label == nil || *v.Label != "From “default”" {
		t.Fatalf("%+v", v)
	}
	if !e.fileExistsIn("speedrun", []byte("one")) {
		t.Fatal("content not copied")
	}
	f, _, err := e.svc.OpenSaveContent(ctx, u, g, "speedrun")
	if err != nil {
		t.Fatal(err)
	}
	f.Close()
	// The source is untouched and independent: deleting the new slot's snapshot keeps both checkpoints.
	if src, _ := e.svc.GetSaveSlot(ctx, u, g, "default"); src.Current.Revision != 1 {
		t.Fatalf("%+v", src.Current)
	}
	if err := e.svc.DeleteSaveVersion(ctx, u, g, "speedrun", 1); err != nil || !e.fileExistsIn("speedrun", []byte("one")) {
		t.Fatalf("%v", err)
	}

	if _, err := e.svc.CreateSaveSlot(ctx, u, g, "default", "speedrun", web); !errors.Is(err, hub.ErrSaveSlotExists) {
		t.Fatal(err)
	}
	if _, err := e.svc.CreateSaveSlot(ctx, u, g, "default", "default", web); !errors.Is(err, hub.ErrSaveSlotExists) {
		t.Fatal(err)
	}
	for _, bad := range []string{"", "Speed Run", "UPPER", strings.Repeat("x", 33), "a/b"} {
		if _, err := e.svc.CreateSaveSlot(ctx, u, g, "default", bad, web); !errors.Is(err, hub.ErrBadRequest) {
			t.Fatalf("%q: %v", bad, err)
		}
	}
	if _, err := e.svc.CreateSaveSlot(ctx, u, g, "nothere", "other", web); !errors.Is(err, hub.ErrNotFound) {
		t.Fatal(err)
	}
	if _, err := e.svc.CreateSaveSlot(ctx, "other-user", g, "default", "other", web); !errors.Is(err, hub.ErrNotFound) {
		t.Fatal(err)
	}
}

func TestSnapshotVersionNumbersStayMonotonicAfterDelete(t *testing.T) {
	e, _ := newComfortEnv(t, nil)
	e.put(e.devA, 0, []byte("one"), hub.SyncCheckpoint)
	u, g := e.user.ID, e.game.ID
	s1, err := e.svc.CreateSaveSnapshot(ctx, u, g, "default", nil)
	if err != nil {
		t.Fatal(err)
	}
	if err := e.svc.DeleteSaveVersion(ctx, u, g, "default", s1.Version); err != nil {
		t.Fatal(err)
	}
	s2, err := e.svc.CreateSaveSnapshot(ctx, u, g, "default", nil)
	if err != nil || s2.Version != s1.Version+1 {
		t.Fatalf("v%d after deleting v%d: %v", s2.Version, s1.Version, err)
	}
}

func TestDeleteSaveVersionAnyReason(t *testing.T) {
	e, _ := newComfortEnv(t, nil)
	e.put(e.devA, 0, []byte("one"), hub.SyncFinalSessionEnd)
	e.put(e.devB, 1, []byte("two"), hub.SyncFinalSessionEnd)
	e.put(e.devA, 2, []byte("three"), hub.SyncCheckpoint)
	u, g := e.user.ID, e.game.ID
	h := e.history()
	if len(h) < 2 {
		t.Fatalf("history %+v", h)
	}
	high := 0
	for _, v := range h {
		if v.Reason == hub.HistoryManualSnapshot {
			t.Fatalf("unexpected snapshot %+v", v)
		}
		if v.Version > high {
			high = v.Version
		}
		if err := e.svc.DeleteSaveVersion(ctx, u, g, "default", v.Version); err != nil {
			t.Fatalf("v%d (%s): %v", v.Version, v.Reason, err)
		}
	}
	if len(e.history()) != 0 {
		t.Fatalf("history remains: %+v", e.history())
	}
	if sl, err := e.svc.GetSaveSlot(ctx, u, g, "default"); err != nil || sl.Current.Revision != 3 || string(e.content()) != "three" {
		t.Fatalf("%+v %v", sl.Current, err)
	}
	// Version numbers stay monotonic after deleting the newest ones.
	snap, err := e.svc.CreateSaveSnapshot(ctx, u, g, "default", nil)
	if err != nil || snap.Version <= high {
		t.Fatalf("v%d after high %d: %v", snap.Version, high, err)
	}
}

func TestDeleteSaveVersionRefusedForOpenConflict(t *testing.T) {
	e, _ := newComfortEnv(t, nil)
	id := e.makeConflict()
	u, g := e.user.ID, e.game.ID
	sl, err := e.svc.GetSaveSlot(ctx, u, g, "default")
	if err != nil || len(sl.OpenConflicts) != 1 {
		t.Fatalf("%+v %v", sl, err)
	}
	sec := sl.OpenConflicts[0].Secured.Version
	if err := e.svc.DeleteSaveVersion(ctx, u, g, "default", sec); !errors.Is(err, hub.ErrConflict) {
		t.Fatalf("secured version: %v", err)
	}
	// Other versions are deletable meanwhile; after resolving, the secured one is too.
	for _, v := range e.history() {
		if v.Version != sec {
			if err := e.svc.DeleteSaveVersion(ctx, u, g, "default", v.Version); err != nil {
				t.Fatalf("v%d: %v", v.Version, err)
			}
		}
	}
	if _, err := e.resolve(id, "use_hub", sl.Current.Revision, e.user.ID); err != nil {
		t.Fatal(err)
	}
	if err := e.svc.DeleteSaveVersion(ctx, u, g, "default", sec); err != nil {
		t.Fatal(err)
	}
}
