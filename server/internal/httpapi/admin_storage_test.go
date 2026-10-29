package httpapi

import (
	"bytes"
	"context"
	"os"
	"path/filepath"
	"testing"

	"github.com/google/uuid"

	"github.com/phabioo/uzume/server/internal/diag"
)

// Admin storage overview and clean-up over REST (0.19 package 7). Requests and responses are checked against the spec.

type storageT struct {
	MeasuredAt string `json:"measured_at"`
	Disk       *struct {
		TotalBytes int64 `json:"total_bytes"`
		FreeBytes  int64 `json:"free_bytes"`
	} `json:"disk"`
	BackupDisk *struct {
		Folder string `json:"folder"`
	} `json:"backup_disk"`
	Kinds []struct {
		ID    string `json:"id"`
		Bytes int64  `json:"bytes"`
		Count int    `json:"count"`
	} `json:"kinds"`
	Screenshots []map[string]any `json:"screenshots"`
	Cleanup     []struct {
		ID    string `json:"id"`
		Bytes int64  `json:"bytes"`
		Count int    `json:"count"`
	} `json:"cleanup"`
}

func TestAdminStorageOverviewAndCleanup(t *testing.T) {
	e, tok := adminEnv(t, diag.NewErrorRing(5), diag.NewRing(5, 1024))
	dir := e.svc.DataDir()
	if err := os.MkdirAll(filepath.Join(dir, "firmware", "nds"), 0o750); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "firmware", "nds", "bios.bin"), make([]byte, 77), 0o640); err != nil {
		t.Fatal(err)
	}

	rec := e.do("GET", "/api/v1/admin/storage", nil, opt{token: tok})
	wantStatus(t, rec, 200, "")
	got := decode[storageT](t, rec)
	ids := []string{"games", "saves", "snapshots", "states", "screenshots", "cores", "firmware"}
	if len(got.Kinds) != len(ids) || got.MeasuredAt == "" || got.Screenshots == nil || got.BackupDisk != nil {
		t.Fatalf("%s", rec.Body.String())
	}
	for i, k := range got.Kinds {
		if k.ID != ids[i] || (k.ID == "firmware" && (k.Bytes != 77 || k.Count != 1)) {
			t.Fatalf("kind %d: %+v", i, k)
		}
	}
	if len(got.Cleanup) != 4 || got.Cleanup[0].ID != "old_snapshots" || got.Cleanup[1].ID != "upload_leftovers" || got.Cleanup[2].ID != "unused_cores" || got.Cleanup[3].ID != "deleted_game_states" {
		t.Fatalf("cleanup %+v", got.Cleanup)
	}

	// Clean-up needs "Confirm it's you" (all ids), the overview does not.
	for _, id := range []string{"old_snapshots", "upload_leftovers", "unused_cores", "deleted_game_states"} {
		rec = e.do("POST", "/api/v1/admin/storage/cleanup", map[string]any{"id": id}, opt{token: tok})
		wantStatus(t, rec, 403, "confirmation_required")
	}
	e.confirm(tok)
	for _, id := range []string{"old_snapshots", "upload_leftovers", "unused_cores", "deleted_game_states"} {
		rec = e.do("POST", "/api/v1/admin/storage/cleanup", map[string]any{"id": id}, opt{token: tok})
		wantStatus(t, rec, 200, "")
		res := decode[struct {
			ID         string `json:"id"`
			FreedBytes int64  `json:"freed_bytes"`
			Removed    int    `json:"removed"`
		}](t, rec)
		if res.ID != id || res.FreedBytes != 0 || res.Removed != 0 {
			t.Fatalf("%s: %+v", id, res)
		}
	}

	rec = e.do("POST", "/api/v1/admin/storage/cleanup", map[string]any{"id": "saves"}, opt{token: tok, raw: true})
	wantStatus(t, rec, 400, "")
}

func TestAdminStorageMemberForbidden(t *testing.T) {
	e, _ := adminEnv(t, diag.NewErrorRing(5), diag.NewRing(5, 1024))
	anna, err := e.svc.CreateUser(context.Background(), "anna", "Anna")
	if err != nil {
		t.Fatal(err)
	}
	admin := e.admin
	e.admin = anna
	userTok := e.login(uuid.NewString())
	e.admin = admin
	rec := e.do("GET", "/api/v1/admin/storage", nil, opt{token: userTok})
	wantStatus(t, rec, 403, "")
	rec = e.do("POST", "/api/v1/admin/storage/cleanup", map[string]any{"id": "old_snapshots"}, opt{token: userTok})
	wantStatus(t, rec, 403, "")
}

func TestAdminStorageScreenshots(t *testing.T) {
	e, tok := adminEnv(t, diag.NewErrorRing(5), diag.NewRing(5, 1024))
	ctx := context.Background()
	rec := e.do("GET", "/api/v1/admin/storage/screenshots", nil, opt{token: tok})
	wantStatus(t, rec, 200, "")
	type resT struct {
		TotalCount int   `json:"total_count"`
		TotalBytes int64 `json:"total_bytes"`
		Items      []struct {
			UserID   string `json:"user_id"`
			UserName string `json:"user_name"`
			GameID   string `json:"game_id"`
			GameName string `json:"game_name"`
			Bytes    int64  `json:"bytes"`
			Count    int    `json:"count"`
		} `json:"items"`
	}
	if r := decode[resT](t, rec); r.Items == nil || len(r.Items) != 0 || r.TotalCount != 0 {
		t.Fatalf("empty: %s", rec.Body.String())
	}

	anna, err := e.svc.CreateUser(ctx, "anna", "Anna")
	if err != nil {
		t.Fatal(err)
	}
	g1, err := e.svc.AddROM(ctx, bytes.NewReader([]byte("rom-one-dummy")), "one.nds", "One", "", e.admin.ID)
	if err != nil {
		t.Fatal(err)
	}
	g2, err := e.svc.AddROM(ctx, bytes.NewReader([]byte("rom-two-dummy")), "two.nds", "Two", "", e.admin.ID)
	if err != nil {
		t.Fatal(err)
	}
	small, big := testPNG(t, 16, 16), testPNG(t, 256, 192)
	add := func(user, game string, data []byte) {
		t.Helper()
		if _, err := e.svc.AddScreenshot(ctx, user, game, "", bytes.NewReader(data)); err != nil {
			t.Fatal(err)
		}
	}
	add(e.admin.ID, g1.ID, small)
	add(e.admin.ID, g1.ID, small)
	add(anna.ID, g1.ID, big)
	add(anna.ID, g2.ID, small)

	rec = e.do("GET", "/api/v1/admin/storage/screenshots", nil, opt{token: tok})
	wantStatus(t, rec, 200, "")
	r := decode[resT](t, rec)
	var sum int64
	for _, it := range r.Items {
		sum += it.Bytes
	}
	if len(r.Items) != 3 || r.TotalCount != 4 || r.TotalBytes != sum || r.TotalBytes <= 0 {
		t.Fatalf("rows: %s", rec.Body.String())
	}
	if first := r.Items[0]; first.UserID != anna.ID || first.GameID != g1.ID || first.Count != 1 || first.UserName != "Anna" || first.GameName != "One" {
		t.Fatalf("largest first: %+v", first)
	}
	for i := 1; i < len(r.Items); i++ {
		if r.Items[i].Bytes > r.Items[i-1].Bytes {
			t.Fatalf("not sorted: %s", rec.Body.String())
		}
	}

	rec = e.do("GET", "/api/v1/admin/storage/screenshots?user_id="+e.admin.ID, nil, opt{token: tok})
	wantStatus(t, rec, 200, "")
	if r := decode[resT](t, rec); len(r.Items) != 1 || r.TotalCount != 2 || r.Items[0].UserID != e.admin.ID {
		t.Fatalf("user filter: %s", rec.Body.String())
	}
	rec = e.do("GET", "/api/v1/admin/storage/screenshots?game_id="+g2.ID+"&user_id="+anna.ID, nil, opt{token: tok})
	wantStatus(t, rec, 200, "")
	if r := decode[resT](t, rec); len(r.Items) != 1 || r.TotalCount != 1 || r.Items[0].GameID != g2.ID {
		t.Fatalf("game filter: %s", rec.Body.String())
	}
}
