package httpapi

import (
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
