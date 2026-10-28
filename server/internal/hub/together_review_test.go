package hub_test

import (
	"errors"
	"fmt"
	"strings"
	"testing"
	"time"

	"github.com/phabioo/uzume/server/internal/hub"
	"github.com/phabioo/uzume/server/internal/hub/hubtest"
)

// Fixes from the 0.23 Together review.

func TestNoReadyCheckOrPauseOnInactiveRun(t *testing.T) {
	for _, to := range []string{hub.RunPaused, hub.RunFinished} {
		t.Run(to, func(t *testing.T) {
			f := newTogetherFx(t, nil)
			if _, err := f.svc.SetRunStatus(ctx, f.admin.ID, f.run.ID, to); err != nil {
				t.Fatal(err)
			}
			got(f.cAdmin)
			got(f.cAnna)
			f.start(t, f.cAdmin)
			send(t, f.cAdmin, "pause_request", fmt.Sprintf(`{"scope":"run","id":%q,"paused":true}`, f.run.ID))
			cs, ps, errs := got(f.cAdmin)
			if len(errs) != 2 || errs[0] != "conflict" || errs[1] != "conflict" || len(cs) != 0 || len(ps) != 0 {
				t.Fatalf("starts must be refused: %+v %+v %v", cs, ps, errs)
			}
			if cs, ps, _ := got(f.cAnna); len(cs) != 0 || len(ps) != 0 {
				t.Fatalf("members saw a refused start: %+v %+v", cs, ps)
			}
			if f.svc.PauseForTest("run", f.run.ID) {
				t.Fatal("a pause was registered")
			}
			again := connect(t, f.svc, f.pAnna)
			if cs, ps, _ := got(again); len(cs) != 0 || len(ps) != 0 {
				t.Fatalf("catch-up after the refused start: %+v %+v", cs, ps)
			}
			// chat stays possible
			chatSend(t, f.cAdmin, "run", f.run.ID, "c1", "still talking")
			if ms, errs := chats(again); len(errs) != 0 || len(ms) != 1 {
				t.Fatalf("chat on an inactive run: %+v %v", ms, errs)
			}
		})
	}
}

func TestDeleteRunAnnouncesEndOfSharedPause(t *testing.T) {
	f := newTogetherFx(t, nil)
	send(t, f.cAnna, "pause_request", fmt.Sprintf(`{"scope":"run","id":%q,"paused":true}`, f.run.ID))
	got(f.cAdmin)
	got(f.cAnna)
	if err := f.svc.DeleteRun(ctx, f.run.ID); err != nil {
		t.Fatal(err)
	}
	for _, c := range []*hub.Client{f.cAdmin, f.cAnna} {
		if _, ps, _ := got(c); len(ps) != 1 || ps[0]["paused"] != false || ps[0]["id"] != f.run.ID {
			t.Fatalf("members must learn that the pause ended: %+v", ps)
		}
	}
	if _, ps, _ := got(f.cBob); len(ps) != 0 {
		t.Fatal("non-member told about the pause")
	}
	if f.svc.PauseForTest("run", f.run.ID) {
		t.Fatal("pause survived the delete")
	}
}

type sessFx struct {
	svc         *hub.Service
	admin, anna hub.User
	bob         hub.User
	po, pa, pb  hub.Principal
	co, ca      *hub.Client
	sessionID   string
}

func newSessFx(t *testing.T) *sessFx {
	t.Helper()
	svc, _ := hubtest.New(t, nil)
	admin, _ := svc.CreateAdmin(ctx, "admin", "secret-12345")
	anna, _ := svc.CreateUser(ctx, "anna", "Anna")
	bob, _ := svc.CreateUser(ctx, "bob", "Bob")
	g, err := svc.AddROM(ctx, strings.NewReader("homebrew-rom-dummy"), "demo.nds", "Demo", "", admin.ID)
	if err != nil {
		t.Fatal(err)
	}
	po, pa, pb := principal(t, svc, admin.ID, "Desktop"), principal(t, svc, anna.ID, "Anna-PC"), principal(t, svc, bob.ID, "Bob-PC")
	co, ca := connect(t, svc, po), connect(t, svc, pa)
	s, err := svc.PublishSession(ctx, po, g.ID, hub.VisibilityHubUsers)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := svc.JoinSession(ctx, pa, s.SessionID); err != nil {
		t.Fatal(err)
	}
	got(co)
	got(ca)
	return &sessFx{svc: svc, admin: admin, anna: anna, bob: bob, po: po, pa: pa, pb: pb, co: co, ca: ca, sessionID: s.SessionID}
}

func TestEndSessionAnnouncesEndOfSharedPause(t *testing.T) {
	f := newSessFx(t)
	send(t, f.ca, "pause_request", fmt.Sprintf(`{"scope":"session","id":%q,"paused":true}`, f.sessionID))
	got(f.co)
	got(f.ca)
	if err := f.svc.EndSession(ctx, f.po, f.sessionID); err != nil {
		t.Fatal(err)
	}
	for _, c := range []*hub.Client{f.co, f.ca} {
		if _, ps, _ := got(c); len(ps) != 1 || ps[0]["paused"] != false || ps[0]["id"] != f.sessionID {
			t.Fatalf("members must learn that the pause ended: %+v", ps)
		}
	}
}

func TestEndedSessionKeepsNoRingOrPause(t *testing.T) {
	f := newSessFx(t)
	if !f.svc.AppendSessionChatForTest(f.sessionID, f.admin.ID, "before") || !f.svc.SessionChatRingForTest(f.sessionID) {
		t.Fatal("running session must keep its ring")
	}
	if err := f.svc.EndSession(ctx, f.po, f.sessionID); err != nil {
		t.Fatal(err)
	}
	if f.svc.SessionChatRingForTest(f.sessionID) {
		t.Fatal("ring survived the end")
	}
	// an append and a pause that lost the race against the end (they run after the cleanup)
	if f.svc.AppendSessionChatForTest(f.sessionID, f.admin.ID, "late") {
		t.Fatal("append to an ended session reported as kept")
	}
	if f.svc.SessionChatRingForTest(f.sessionID) {
		t.Fatal("a late append recreated the ring of an ended session")
	}
	if f.svc.SetPauseForTest("session", f.sessionID, f.admin.ID) || f.svc.PauseForTest("session", f.sessionID) {
		t.Fatal("a late pause survived on an ended session")
	}
}

func TestReadyCheckResyncsMembersAfterRegistering(t *testing.T) {
	f := newTogetherFx(t, nil)
	// registered with a list read before anna joined and with bob, who is no member
	f.svc.RegisterStaleCheckForTest("run", f.run.ID, f.admin.ID, []string{f.admin.ID, f.bob.ID})
	cs := got2(f.cAnna)
	if len(cs) != 1 || cs[0].state(f.anna.ID) != "pending" || cs[0].state(f.bob.ID) != "" || len(cs[0].Members) != 2 {
		t.Fatalf("members must be reconciled: %+v", cs)
	}
	if cs := got2(f.cBob); len(cs) == 0 || cs[len(cs)-1].state(f.bob.ID) != "" {
		t.Fatalf("bob (who left the list) must hear about the change: %+v", cs)
	}
	// a scope that is gone meanwhile drops the check
	f2 := newSessFx(t)
	if err := f2.svc.EndSession(ctx, f2.po, f2.sessionID); err != nil {
		t.Fatal(err)
	}
	got(f2.co)
	f2.svc.RegisterStaleCheckForTest("session", f2.sessionID, f2.admin.ID, []string{f2.admin.ID, f2.anna.ID})
	if cs := got2(f2.co); len(cs) == 0 || cs[len(cs)-1].Status != "cancelled" {
		t.Fatalf("check on an ended session must be cancelled: %+v", cs)
	}
}

func TestReadyCheckCancelAndCatchUpExpireLazily(t *testing.T) {
	f := newTogetherFx(t, nil)
	f.start(t, f.cAdmin)
	cs := got2(f.cAnna)
	id := cs[0].CheckID
	got(f.cAdmin)
	f.clk.Advance(31 * time.Second)
	send(t, f.cAdmin, "ready_check_cancel", fmt.Sprintf(`{"check_id":%q}`, id))
	cs, _, errs := got(f.cAdmin)
	if len(errs) != 1 || errs[0] != "not_found" || len(cs) != 1 || cs[0].Status != "expired" {
		t.Fatalf("cancel after the time ran out: %+v %v", cs, errs)
	}
	if cs := got2(f.cAnna); len(cs) != 1 || cs[0].Status != "expired" {
		t.Fatalf("members told it expired: %+v", cs)
	}

	// catch-up must not resend a check that ran out
	f.start(t, f.cAdmin)
	got(f.cAdmin)
	got(f.cAnna)
	f.clk.Advance(31 * time.Second)
	f.cAnna.Disconnect()
	again := connect(t, f.svc, f.pAnna)
	for _, c := range got2(again) {
		if c.Status == "open" {
			t.Fatalf("catch-up resent an expired check: %+v", c)
		}
	}
	if cs := got2(f.cAdmin); len(cs) == 0 || cs[len(cs)-1].Status != "expired" {
		t.Fatalf("starter told it expired: %+v", cs)
	}
}

func TestListSessionChatDoesNotRevealToNonMembers(t *testing.T) {
	f := newSessFx(t)
	code := func(err error) hub.Code {
		var he *hub.Error
		if !errors.As(err, &he) {
			t.Fatalf("not a hub error: %v", err)
		}
		return he.Code
	}
	_, errUnknown := f.svc.ListSessionChat(ctx, f.pb, "no-such-session")
	_, errStranger := f.svc.ListSessionChat(ctx, f.pb, f.sessionID)
	if code(errUnknown) != hub.CodeSessionNotFound || code(errStranger) != hub.CodeSessionNotFound {
		t.Fatalf("stranger of a running session: %v / unknown: %v", errStranger, errUnknown)
	}
	if err := f.svc.EndSession(ctx, f.po, f.sessionID); err != nil {
		t.Fatal(err)
	}
	if _, err := f.svc.ListSessionChat(ctx, f.pb, f.sessionID); code(err) != hub.CodeSessionNotFound {
		t.Fatalf("stranger of an ended session must not learn that it ended: %v", err)
	}
	if _, err := f.svc.ListSessionChat(ctx, f.po, f.sessionID); code(err) != hub.CodeSessionEnded {
		t.Fatalf("owner of an ended session: %v", err)
	}
}

func TestRunChatSameClientIDFromTwoUsers(t *testing.T) {
	f := newTogetherFx(t, nil)
	chatSend(t, f.cAdmin, "run", f.run.ID, "dup", "from admin")
	chatSend(t, f.cAnna, "run", f.run.ID, "dup", "from anna")
	for _, c := range []*hub.Client{f.cAdmin, f.cAnna} {
		ms, errs := chats(c)
		if len(errs) != 0 || len(ms) != 2 || ms[0].Text != "from admin" || ms[1].Text != "from anna" {
			t.Fatalf("both messages must be stored and delivered: %+v %v", ms, errs)
		}
	}
	stored, _, err := f.svc.ListRunChat(ctx, f.admin.ID, f.run.ID, 0, 100)
	if err != nil || len(stored) != 2 {
		t.Fatalf("stored: %v %v", stored, err)
	}
}
