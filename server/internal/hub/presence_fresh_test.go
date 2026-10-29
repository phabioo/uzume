package hub_test

import (
	"fmt"
	"strings"
	"testing"

	"github.com/phabioo/uzume/server/internal/hub"
	"github.com/phabioo/uzume/server/internal/hub/hubtest"
)

// A receiver who registers the ROM of a local-only game after the sender reported presence gets the game title
// without the sender reporting again (and loses it again when the registration is removed).
func TestPresenceTitleFollowsRomRegistration(t *testing.T) {
	svc, _ := hubtest.New(t, nil)
	admin, _ := svc.CreateAdmin(ctx, "fabio", "secret-12345")
	anna, _ := svc.CreateUser(ctx, "anna", "Anna")
	sha := strings.Repeat("ab", 32)
	in := hub.RegisterInput{SHA256: sha, Filename: "demo.nds", System: "nds", Title: "Local Demo", Size: 1000}
	g, _, err := svc.RegisterROM(ctx, admin.ID, in)
	if err != nil {
		t.Fatal(err)
	}
	owner := principal(t, svc, admin.ID, "Desktop")
	co := connect(t, svc, owner)
	ca := connect(t, svc, principal(t, svc, anna.ID, "Anna-PC"))
	drain(co)
	drain(ca)
	if err := co.Handle([]byte(fmt.Sprintf(`{"type":"presence_update","payload":{"state":"in_game","game_id":%q}}`, g.ID))); err != nil {
		t.Fatal(err)
	}
	if p := lastPresence(ca, owner.Device.ID); p == nil || p["game_id"] != g.ID || p["game_title"] != "" {
		t.Fatalf("a non-owner must not see the title yet: %+v", p)
	}
	if _, _, err := svc.RegisterROM(ctx, anna.ID, in); err != nil {
		t.Fatal(err)
	}
	if p := lastPresence(ca, owner.Device.ID); p == nil || p["game_title"] != "Local Demo" || p["system"] == "" {
		t.Fatalf("registering the ROM must deliver the title: %+v", p)
	}
	if err := svc.UnregisterROM(ctx, anna.ID, g.ID); err != nil {
		t.Fatal(err)
	}
	if p := lastPresence(ca, owner.Device.ID); p == nil || p["game_id"] != g.ID || p["game_title"] != "" {
		t.Fatalf("removing the registration must hide the title again: %+v", p)
	}
}

// The presence update that removes session_id after an ACL revocation must reach a receiver whose queue is busy.
func TestPresenceSessionRevocationSurvivesBusyQueue(t *testing.T) {
	svc, _ := hubtest.New(t, nil)
	admin, _ := svc.CreateAdmin(ctx, "fabio", "secret-12345")
	anna, _ := svc.CreateUser(ctx, "anna", "Anna")
	g, err := svc.AddROM(ctx, strings.NewReader("homebrew-rom-dummy"), "demo.nds", "Demo", "", admin.ID)
	if err != nil {
		t.Fatal(err)
	}
	owner := principal(t, svc, admin.ID, "Desktop")
	co := connect(t, svc, owner)
	ca := connect(t, svc, principal(t, svc, anna.ID, "Anna-PC"))
	play := fmt.Sprintf(`{"type":"presence_update","payload":{"state":"in_game","game_id":%q}}`, g.ID)
	if err := co.Handle([]byte(play)); err != nil {
		t.Fatal(err)
	}
	s, err := svc.PublishSession(ctx, owner, g.ID, hub.VisibilityHubUsers)
	if err != nil {
		t.Fatal(err)
	}
	if p := lastPresence(ca, owner.Device.ID); p == nil || p["session_id"] != s.SessionID {
		t.Fatalf("anna must see the session first: %+v", p)
	}
	for i := 0; i < 100; i++ { // fill anna's queue up to the droppable limit
		if err := co.Handle([]byte(play)); err != nil {
			t.Fatal(err)
		}
	}
	if _, err := svc.SetSessionVisibility(ctx, owner, s.SessionID, hub.VisibilityPrivate); err != nil {
		t.Fatal(err)
	}
	if p := lastPresence(ca, owner.Device.ID); p == nil || p["session_id"] != "" {
		t.Fatalf("the revocation must arrive although the queue is busy: %+v", p)
	}
}

// A check registered with a member list read while the run was still active is dropped when the run has left the
// active state by the time it is registered.
func TestReadyCheckDroppedWhenRunLeavesActiveBeforeRegistration(t *testing.T) {
	f := newTogetherFx(t, nil)
	if _, err := f.svc.SetRunStatus(ctx, f.admin.ID, f.run.ID, hub.RunFinished); err != nil {
		t.Fatal(err)
	}
	got(f.cAdmin)
	got(f.cAnna)
	f.svc.RegisterStaleCheckForTest("run", f.run.ID, f.admin.ID, []string{f.admin.ID, f.anna.ID})
	if cs := got2(f.cAnna); len(cs) == 0 || cs[len(cs)-1].Status != "cancelled" {
		t.Fatalf("the check on a finished run must be cancelled: %+v", cs)
	}
	f.svc.RegisterStaleCheckForTest("run", f.run.ID, f.admin.ID, []string{f.admin.ID})
	if f.svc.OpenCheckForTest("run", f.run.ID) {
		t.Fatal("a ready check survived on a finished run")
	}
}
