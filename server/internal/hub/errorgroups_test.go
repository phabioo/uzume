package hub_test

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/phabioo/uzume/server/internal/hub"
	"github.com/phabioo/uzume/server/internal/updates"
)

// A failed self-update is recorded once as an update_failure event of the server, however often the status is read.
func TestFailedUpdateIsRecordedOnce(t *testing.T) {
	svc, _, _ := newAdmin(t)
	dir := filepath.Join(svc.DataDir(), updates.UpdatesDir)
	if err := os.MkdirAll(dir, 0o750); err != nil {
		t.Fatal(err)
	}
	b, _ := json.Marshal(updates.Result{Version: "0.24.1", Time: time.Now().UTC(), OK: false, Message: "dpkg exited 1"})
	if err := os.WriteFile(filepath.Join(dir, updates.LastResultName), b, 0o640); err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 3; i++ {
		if _, err := svc.UpdateStatus(ctx); err != nil {
			t.Fatal(err)
		}
	}
	gs, unseen, err := svc.ListErrorGroups(ctx)
	if err != nil || len(gs) != 1 || unseen != 1 {
		t.Fatalf("%+v %d %v", gs, unseen, err)
	}
	if g := gs[0]; g.Kind != hub.ErrKindUpdateFailure || g.Component != "server" || g.Count != 1 || g.Title != "Update to 0.24.1 failed" {
		t.Fatalf("%+v", g)
	}
	d, err := svc.GetErrorGroup(ctx, gs[0].Fingerprint)
	if err != nil || len(d.Events) != 1 || d.Events[0].DeviceName != "Uzume server" {
		t.Fatalf("%+v %v", d, err)
	}
}

func TestErrorGroupUpdateAndDeleteErrors(t *testing.T) {
	svc, _, _ := newAdmin(t)
	if _, err := svc.GetErrorGroup(ctx, "uz-000000000000"); err != hub.ErrNotFound {
		t.Fatalf("get: %v", err)
	}
	if _, err := svc.UpdateErrorGroup(ctx, "uz-000000000000", hub.ErrorGroupUpdate{Seen: true}); err != hub.ErrNotFound {
		t.Fatalf("update: %v", err)
	}
	if err := svc.DeleteErrorGroup(ctx, "uz-000000000000"); err != hub.ErrNotFound {
		t.Fatalf("delete: %v", err)
	}
	if _, err := svc.UpdateErrorGroup(ctx, "uz-000000000000", hub.ErrorGroupUpdate{}); err == nil {
		t.Fatal("an empty update must be refused")
	}
}
