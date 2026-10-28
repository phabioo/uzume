package hub_test

import (
	"encoding/json"
	"fmt"
	"strings"
	"testing"
	"time"

	"github.com/phabioo/uzume/server/internal/hub"
	"github.com/phabioo/uzume/server/internal/hub/hubtest"
)

type rcMember struct {
	UserID string `json:"user_id"`
	State  string `json:"state"`
}

type rcMsg struct {
	CheckID   string     `json:"check_id"`
	Scope     string     `json:"scope"`
	ID        string     `json:"id"`
	By        string     `json:"by"`
	Status    string     `json:"status"`
	ExpiresAt string     `json:"expires_at"`
	Members   []rcMember `json:"members"`
}

func (m rcMsg) state(user string) string {
	for _, x := range m.Members {
		if x.UserID == user {
			return x.State
		}
	}
	return ""
}

// got drains c and returns the ready_check messages, the pause_state payloads and the error codes.
func got(c *hub.Client) (checks []rcMsg, pauses []map[string]any, errs []string) {
	for _, raw := range drain(c) {
		var env struct {
			Type    string          `json:"type"`
			Payload json.RawMessage `json:"payload"`
		}
		if json.Unmarshal([]byte(raw), &env) != nil {
			continue
		}
		switch env.Type {
		case "ready_check":
			var m rcMsg
			_ = json.Unmarshal(env.Payload, &m)
			checks = append(checks, m)
		case "pause_state":
			var m map[string]any
			_ = json.Unmarshal(env.Payload, &m)
			pauses = append(pauses, m)
		case "error":
			var m map[string]string
			_ = json.Unmarshal(env.Payload, &m)
			errs = append(errs, m["code"])
		}
	}
	return
}

func send(t *testing.T, c *hub.Client, typ string, payload string) {
	t.Helper()
	if err := c.Handle([]byte(fmt.Sprintf(`{"type":%q,"payload":%s}`, typ, payload))); err != nil {
		t.Fatal(err)
	}
}

type togetherFx struct {
	svc               *hub.Service
	clk               *hubtest.Clock
	admin, anna, bob  hub.User
	run               hub.Run
	cAdmin, cAnna     *hub.Client
	cBob              *hub.Client
	pAdmin, pAnna, pB hub.Principal
}

// newTogetherFx builds a run with admin and anna joined; bob exists but is no member.
func newTogetherFx(t *testing.T, mod func(*hub.Options)) *togetherFx {
	t.Helper()
	svc, clk := hubtest.New(t, mod)
	admin, err := svc.CreateAdmin(ctx, "admin", "secret-12345")
	if err != nil {
		t.Fatal(err)
	}
	anna, _ := svc.CreateUser(ctx, "anna", "Anna")
	bob, _ := svc.CreateUser(ctx, "bob", "Bob")
	g, err := svc.AddROM(ctx, strings.NewReader("homebrew-rom-dummy"), "demo.nds", "Demo", "", admin.ID)
	if err != nil {
		t.Fatal(err)
	}
	run, err := svc.CreateRun(ctx, hub.CreateRunInput{UserID: admin.ID, Name: "Run", GameID: g.ID, InviteUserIDs: []string{anna.ID}})
	if err != nil {
		t.Fatal(err)
	}
	if run, err = svc.JoinRun(ctx, anna.ID, run.ID, g.ID); err != nil {
		t.Fatal(err)
	}
	f := &togetherFx{svc: svc, clk: clk, admin: admin, anna: anna, bob: bob, run: run}
	f.pAdmin, f.pAnna, f.pB = principal(t, svc, admin.ID, "Desktop"), principal(t, svc, anna.ID, "Anna-PC"), principal(t, svc, bob.ID, "Bob-PC")
	f.cAdmin, f.cAnna, f.cBob = connect(t, svc, f.pAdmin), connect(t, svc, f.pAnna), connect(t, svc, f.pB)
	if _, err := svc.SetRunStatus(ctx, admin.ID, run.ID, hub.RunActive); err != nil { // ready checks and pauses need an active run
		t.Fatal(err)
	}
	got(f.cAdmin)
	got(f.cAnna)
	got(f.cBob)
	return f
}

func (f *togetherFx) start(t *testing.T, c *hub.Client) {
	t.Helper()
	send(t, c, "ready_check_start", fmt.Sprintf(`{"scope":"run","id":%q}`, f.run.ID))
}

func TestReadyCheckAllReady(t *testing.T) {
	f := newTogetherFx(t, nil)
	f.start(t, f.cAdmin)
	for _, c := range []*hub.Client{f.cAdmin, f.cAnna} {
		cs, _, errs := got(c)
		if len(errs) != 0 || len(cs) != 1 || cs[0].Status != "open" || cs[0].By != f.admin.ID ||
			cs[0].state(f.admin.ID) != "ready" || cs[0].state(f.anna.ID) != "pending" || cs[0].ExpiresAt == "" {
			t.Fatalf("start: %+v %v", cs, errs)
		}
	}
	if cs, _, _ := got(f.cBob); len(cs) != 0 {
		t.Fatalf("non-member got the check: %+v", cs)
	}
	// starting again while open returns the current check to the requester only
	got(f.cAnna)
	f.start(t, f.cAnna)
	again, _, errs := got(f.cAnna)
	if len(errs) != 0 || len(again) != 1 || again[0].Status != "open" || again[0].By != f.admin.ID {
		t.Fatalf("second start: %+v %v", again, errs)
	}
	if cs, _, _ := got(f.cAdmin); len(cs) != 0 {
		t.Fatalf("second start must not broadcast: %+v", cs)
	}
	id := again[0].CheckID
	send(t, f.cAnna, "ready_check_answer", fmt.Sprintf(`{"check_id":%q,"ready":true}`, id))
	cs := got2(f.cAdmin)
	if len(cs) != 1 || cs[0].Status != "all_ready" || cs[0].state(f.anna.ID) != "ready" {
		t.Fatalf("all_ready: %+v", cs)
	}
	// the check is over: answering fails, a new one can start
	send(t, f.cAnna, "ready_check_answer", fmt.Sprintf(`{"check_id":%q,"ready":true}`, id))
	if _, _, errs := got(f.cAnna); len(errs) != 1 || errs[0] != "not_found" {
		t.Fatalf("answer after end: %v", errs)
	}
	f.start(t, f.cAnna)
	if cs, _, _ := got(f.cAdmin); len(cs) != 1 || cs[0].Status != "open" || cs[0].By != f.anna.ID {
		t.Fatalf("new check: %+v", cs)
	}
}

func TestReadyCheckNotReadyStaysOpenAndChanges(t *testing.T) {
	f := newTogetherFx(t, nil)
	f.start(t, f.cAdmin)
	cs, _, _ := got(f.cAnna)
	id := cs[0].CheckID
	send(t, f.cAnna, "ready_check_answer", fmt.Sprintf(`{"check_id":%q,"ready":false}`, id))
	cs, _, _ = got(f.cAdmin)
	if len(cs) != 2 || cs[1].Status != "open" || cs[1].state(f.anna.ID) != "not_ready" {
		t.Fatalf("not_ready: %+v", cs)
	}
	send(t, f.cAnna, "ready_check_answer", fmt.Sprintf(`{"check_id":%q,"ready":true}`, id))
	if cs, _, _ = got(f.cAdmin); len(cs) != 1 || cs[0].Status != "all_ready" {
		t.Fatalf("changed mind: %+v", cs)
	}
}

func TestReadyCheckExpiryFakeClockAndTimer(t *testing.T) {
	f := newTogetherFx(t, nil)
	f.start(t, f.cAdmin)
	cs, _, _ := got(f.cAnna)
	id := cs[0].CheckID
	f.clk.Advance(31 * time.Second)
	send(t, f.cAnna, "ready_check_answer", fmt.Sprintf(`{"check_id":%q,"ready":true}`, id))
	cs, _, errs := got(f.cAnna)
	if len(errs) != 1 || errs[0] != "not_found" || len(cs) != 1 || cs[0].Status != "expired" {
		t.Fatalf("lazy expiry: %+v %v", cs, errs)
	}
	if cs, _, _ := got(f.cAdmin); len(cs) != 2 || cs[1].Status != "expired" {
		t.Fatalf("starter notified: %+v", cs)
	}

	// the real timer expires a check nobody touches
	g := newTogetherFx(t, func(o *hub.Options) { o.ReadyCheckTTL = 40 * time.Millisecond })
	g.start(t, g.cAdmin)
	got(g.cAnna)
	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		for _, c := range got2(g.cAnna) {
			if c.Status == "expired" {
				return
			}
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatal("timer did not expire the check")
}

func got2(c *hub.Client) []rcMsg { cs, _, _ := got(c); return cs }

func TestReadyCheckCancelAndMembership(t *testing.T) {
	f := newTogetherFx(t, nil)
	// non-member cannot start
	send(t, f.cBob, "ready_check_start", fmt.Sprintf(`{"scope":"run","id":%q}`, f.run.ID))
	if _, _, errs := got(f.cBob); len(errs) != 1 || errs[0] != "forbidden" {
		t.Fatalf("non-member start: %v", errs)
	}
	f.start(t, f.cAdmin)
	cs, _, _ := got(f.cAnna)
	id := cs[0].CheckID
	// non-member cannot answer or cancel; a non-starter cannot cancel
	send(t, f.cBob, "ready_check_answer", fmt.Sprintf(`{"check_id":%q,"ready":true}`, id))
	send(t, f.cBob, "ready_check_cancel", fmt.Sprintf(`{"check_id":%q}`, id))
	if _, _, errs := got(f.cBob); len(errs) != 2 || errs[0] != "forbidden" || errs[1] != "forbidden" {
		t.Fatalf("non-member answer/cancel: %v", errs)
	}
	send(t, f.cAnna, "ready_check_cancel", fmt.Sprintf(`{"check_id":%q}`, id))
	if _, _, errs := got(f.cAnna); len(errs) != 1 || errs[0] != "forbidden" {
		t.Fatalf("cancel by non-starter: %v", errs)
	}
	send(t, f.cAdmin, "ready_check_cancel", fmt.Sprintf(`{"check_id":%q}`, id))
	cs, _, _ = got(f.cAnna)
	if len(cs) != 1 || cs[0].Status != "cancelled" {
		t.Fatalf("cancel: %+v", cs)
	}
	// bad input
	send(t, f.cAdmin, "ready_check_start", `{"scope":"galaxy","id":"x"}`)
	send(t, f.cAdmin, "ready_check_answer", `{"check_id":"x"}`)
	if _, _, errs := got(f.cAdmin); len(errs) != 2 || errs[0] != "bad_request" || errs[1] != "bad_request" {
		t.Fatalf("bad input: %v", errs)
	}
}

func TestReadyCheckMemberLeavingCompletesCheck(t *testing.T) {
	f := newTogetherFx(t, nil)
	f.start(t, f.cAdmin)
	got(f.cAnna)
	got(f.cAdmin)
	if _, err := f.svc.LeaveRun(ctx, f.anna.ID, f.run.ID); err != nil {
		t.Fatal(err)
	}
	cs := got2(f.cAdmin)
	if len(cs) == 0 || cs[len(cs)-1].Status != "all_ready" || len(cs[len(cs)-1].Members) != 1 {
		t.Fatalf("after leave: %+v", cs)
	}
}

func TestPauseBroadcastAndCatchUp(t *testing.T) {
	f := newTogetherFx(t, nil)
	send(t, f.cBob, "pause_request", fmt.Sprintf(`{"scope":"run","id":%q,"paused":true}`, f.run.ID))
	if _, _, errs := got(f.cBob); len(errs) != 1 || errs[0] != "forbidden" {
		t.Fatalf("non-member pause: %v", errs)
	}
	send(t, f.cAnna, "pause_request", fmt.Sprintf(`{"scope":"run","id":%q,"paused":true}`, f.run.ID))
	for _, c := range []*hub.Client{f.cAdmin, f.cAnna} {
		_, ps, _ := got(c)
		if len(ps) != 1 || ps[0]["paused"] != true || ps[0]["by"] != f.anna.ID || ps[0]["id"] != f.run.ID {
			t.Fatalf("pause_state: %+v", ps)
		}
	}
	if _, ps, _ := got(f.cBob); len(ps) != 0 {
		t.Fatalf("non-member got pause_state")
	}
	// reconnect of the admin device: catch-up; bob gets nothing
	f.cAdmin.Disconnect()
	again := connect(t, f.svc, f.pAdmin)
	if _, ps, _ := got(again); len(ps) != 1 || ps[0]["paused"] != true || ps[0]["by"] != f.anna.ID {
		t.Fatalf("catch-up: %+v", ps)
	}
	if _, ps, _ := got(connect(t, f.svc, principal(t, f.svc, f.bob.ID, "Bob-2"))); len(ps) != 0 {
		t.Fatalf("catch-up leaked to non-member")
	}
	// resume clears it
	send(t, again, "pause_request", fmt.Sprintf(`{"scope":"run","id":%q,"paused":false}`, f.run.ID))
	if _, ps, _ := got(f.cAnna); len(ps) != 1 || ps[0]["paused"] != false {
		t.Fatalf("resume: %+v", ps)
	}
	again.Disconnect()
	if _, ps, _ := got(connect(t, f.svc, f.pAdmin)); len(ps) != 0 {
		t.Fatalf("no catch-up after resume: %+v", ps)
	}
}

func TestMultiDeviceUserAnswersOnce(t *testing.T) {
	f := newTogetherFx(t, nil)
	annaLaptop := connect(t, f.svc, principal(t, f.svc, f.anna.ID, "Anna-Laptop"))
	got(annaLaptop)
	f.start(t, f.cAdmin)
	cs, _, _ := got(annaLaptop)
	if len(cs) != 1 || len(cs[0].Members) != 2 {
		t.Fatalf("members must be users, not devices: %+v", cs)
	}
	send(t, annaLaptop, "ready_check_answer", fmt.Sprintf(`{"check_id":%q,"ready":true}`, cs[0].CheckID))
	for _, c := range []*hub.Client{f.cAdmin, f.cAnna, annaLaptop} {
		cs := got2(c)
		if len(cs) == 0 || cs[len(cs)-1].Status != "all_ready" {
			t.Fatalf("all devices must see all_ready: %+v", cs)
		}
	}
}

func TestSessionReadyCheckPauseAndEnd(t *testing.T) {
	svc, _ := hubtest.New(t, nil)
	admin, _ := svc.CreateAdmin(ctx, "admin", "secret-12345")
	anna, _ := svc.CreateUser(ctx, "anna", "Anna")
	bob, _ := svc.CreateUser(ctx, "bob", "Bob")
	g, err := svc.AddROM(ctx, strings.NewReader("homebrew-rom-dummy"), "demo.nds", "Demo", "", admin.ID)
	if err != nil {
		t.Fatal(err)
	}
	po, pa, pb := principal(t, svc, admin.ID, "Desktop"), principal(t, svc, anna.ID, "Anna-PC"), principal(t, svc, bob.ID, "Bob-PC")
	co, ca, cb := connect(t, svc, po), connect(t, svc, pa), connect(t, svc, pb)
	s, err := svc.PublishSession(ctx, po, g.ID, hub.VisibilityHubUsers)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := svc.JoinSession(ctx, pa, s.SessionID); err != nil {
		t.Fatal(err)
	}
	got(co)
	got(ca)
	got(cb)
	// bob watches nothing: not a member
	send(t, cb, "ready_check_start", fmt.Sprintf(`{"scope":"session","id":%q}`, s.SessionID))
	if _, _, errs := got(cb); len(errs) != 1 || errs[0] != "forbidden" {
		t.Fatalf("non-member session start: %v", errs)
	}
	send(t, ca, "ready_check_start", fmt.Sprintf(`{"scope":"session","id":%q}`, s.SessionID))
	cs := got2(co)
	if len(cs) != 1 || cs[0].Scope != "session" || len(cs[0].Members) != 2 || cs[0].By != anna.ID {
		t.Fatalf("session check: %+v", cs)
	}
	send(t, ca, "pause_request", fmt.Sprintf(`{"scope":"session","id":%q,"paused":true}`, s.SessionID))
	if _, ps, _ := got(co); len(ps) != 1 || ps[0]["paused"] != true {
		t.Fatalf("session pause: %+v", ps)
	}
	got(ca)
	if err := svc.EndSession(ctx, po, s.SessionID); err != nil {
		t.Fatal(err)
	}
	for _, c := range []*hub.Client{co, ca} {
		cs := got2(c)
		if len(cs) == 0 || cs[len(cs)-1].Status != "cancelled" {
			t.Fatalf("session end must cancel: %+v", cs)
		}
	}
	// pause cleared: a reconnect gets no catch-up (and the session is gone anyway)
	co.Disconnect()
	if _, ps, _ := got(connect(t, svc, po)); len(ps) != 0 {
		t.Fatalf("pause survived the session: %+v", ps)
	}
}

func TestRunLeavingActiveStateEndsCheckAndPause(t *testing.T) {
	for _, to := range []string{hub.RunPaused, hub.RunFinished} {
		t.Run(to, func(t *testing.T) {
			f := newTogetherFx(t, nil)
			f.start(t, f.cAdmin)
			send(t, f.cAnna, "pause_request", fmt.Sprintf(`{"scope":"run","id":%q,"paused":true}`, f.run.ID))
			got(f.cAdmin)
			got(f.cAnna)
			if _, err := f.svc.SetRunStatus(ctx, f.admin.ID, f.run.ID, to); err != nil {
				t.Fatal(err)
			}
			for _, c := range []*hub.Client{f.cAdmin, f.cAnna} {
				cs, ps, _ := got(c)
				if len(cs) == 0 || cs[len(cs)-1].Status != "cancelled" {
					t.Fatalf("check must be cancelled: %+v", cs)
				}
				if len(ps) != 1 || ps[0]["paused"] != false {
					t.Fatalf("pause must be cleared: %+v", ps)
				}
			}
			again := connect(t, f.svc, principal(t, f.svc, f.anna.ID, "Anna-2"))
			if cs, ps, _ := got(again); len(ps) != 0 || len(cs) != 0 {
				t.Fatalf("no catch-up after the run left active: %+v %+v", cs, ps)
			}
		})
	}
}
