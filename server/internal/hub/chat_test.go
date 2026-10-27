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

type chatMsg struct {
	Scope    string   `json:"scope"`
	ID       string   `json:"id"`
	Seq      int64    `json:"seq"`
	UserID   string   `json:"user_id"`
	Text     string   `json:"text"`
	SentAt   string   `json:"sent_at"`
	Mentions []string `json:"mentions"`
	ClientID string   `json:"client_id"`
}

func chats(c *hub.Client) (msgs []chatMsg, errs []string) {
	for _, raw := range drain(c) {
		var env struct {
			Type    string          `json:"type"`
			Payload json.RawMessage `json:"payload"`
		}
		if json.Unmarshal([]byte(raw), &env) != nil {
			continue
		}
		switch env.Type {
		case "chat_message":
			var m chatMsg
			_ = json.Unmarshal(env.Payload, &m)
			msgs = append(msgs, m)
		case "error":
			var m map[string]string
			_ = json.Unmarshal(env.Payload, &m)
			errs = append(errs, m["code"])
		}
	}
	return
}

func chatSend(t *testing.T, c *hub.Client, scope, id, cid, text string) {
	t.Helper()
	b, _ := json.Marshal(map[string]string{"scope": scope, "id": id, "client_id": cid, "text": text})
	send(t, c, "chat_send", string(b))
}

func TestRunChatBroadcastMentionsAndMembers(t *testing.T) {
	f := newTogetherFx(t, nil)
	chatSend(t, f.cAdmin, "run", f.run.ID, "c1", "  hi @ANNA and @anna and @nobody @bob  ")
	for _, c := range []*hub.Client{f.cAdmin, f.cAnna} {
		ms, errs := chats(c)
		if len(errs) != 0 || len(ms) != 1 {
			t.Fatalf("members get it: %v %v", ms, errs)
		}
		m := ms[0]
		if m.Scope != "run" || m.ID != f.run.ID || m.Seq != 1 || m.UserID != f.admin.ID || m.Text != "hi @ANNA and @anna and @nobody @bob" || m.ClientID != "c1" {
			t.Fatalf("message: %+v", m)
		}
		// bob exists but is no member: his name stays plain text
		if len(m.Mentions) != 1 || m.Mentions[0] != f.anna.ID {
			t.Fatalf("mentions: %v", m.Mentions)
		}
	}
	if ms, _ := chats(f.cBob); len(ms) != 0 {
		t.Fatalf("non-member received %v", ms)
	}
	chatSend(t, f.cBob, "run", f.run.ID, "x", "let me in")
	if ms, errs := chats(f.cBob); len(ms) != 0 || len(errs) != 1 || errs[0] != "forbidden" {
		t.Fatalf("non-member send: %v %v", ms, errs)
	}
	if ms, _ := chats(f.cAdmin); len(ms) != 0 {
		t.Fatal("non-member message must not be delivered")
	}
}

func TestChatLengthLimits(t *testing.T) {
	f := newTogetherFx(t, nil)
	chatSend(t, f.cAdmin, "run", f.run.ID, "a", "   ")
	chatSend(t, f.cAdmin, "run", f.run.ID, "b", strings.Repeat("ä", 501))
	chatSend(t, f.cAdmin, "run", f.run.ID, "c", "x")
	f.clk.Advance(time.Second)
	chatSend(t, f.cAdmin, "run", f.run.ID, "d", strings.Repeat("ä", 500)) // 1000 bytes, 500 characters
	ms, errs := chats(f.cAdmin)
	if len(errs) != 2 || errs[0] != "bad_request" || errs[1] != "bad_request" || len(ms) != 2 {
		t.Fatalf("limits: %d msgs, errs %v", len(ms), errs)
	}
}

func TestChatRateLimit(t *testing.T) {
	f := newTogetherFx(t, nil)
	for i := 0; i < 7; i++ {
		chatSend(t, f.cAdmin, "run", f.run.ID, fmt.Sprint("r", i), "spam")
	}
	ms, errs := chats(f.cAdmin)
	if len(ms) != 5 || len(errs) != 2 || errs[0] != "rate_limited" {
		t.Fatalf("burst: %d msgs, errs %v", len(ms), errs)
	}
	// own bucket per user
	chatSend(t, f.cAnna, "run", f.run.ID, "a1", "still fine")
	if _, errs := chats(f.cAnna); len(errs) != 0 {
		t.Fatalf("anna limited by admin: %v", errs)
	}
	chats(f.cAdmin)            // includes anna's message
	f.clk.Advance(time.Second) // 2 tokens
	chatSend(t, f.cAdmin, "run", f.run.ID, "r8", "again")
	chatSend(t, f.cAdmin, "run", f.run.ID, "r9", "again")
	chatSend(t, f.cAdmin, "run", f.run.ID, "r10", "again")
	ms, errs = chats(f.cAdmin)
	if len(ms) != 2 || len(errs) != 1 {
		t.Fatalf("refill: %d msgs, errs %v", len(ms), errs)
	}
}

func TestChatIdempotentClientID(t *testing.T) {
	f := newTogetherFx(t, nil)
	chatSend(t, f.cAdmin, "run", f.run.ID, "same", "once")
	chats(f.cAdmin)
	chats(f.cAnna)
	chatSend(t, f.cAdmin, "run", f.run.ID, "same", "once again")
	ms, _ := chats(f.cAdmin)
	if len(ms) != 1 || ms[0].Seq != 1 || ms[0].Text != "once" {
		t.Fatalf("resend must return the stored message: %+v", ms)
	}
	if ms, _ := chats(f.cAnna); len(ms) != 0 {
		t.Fatalf("resend must go to the sender only: %+v", ms)
	}
	if got, _, err := f.svc.ListRunChat(ctx, f.admin.ID, f.run.ID, 0, 100); err != nil || len(got) != 1 {
		t.Fatalf("stored %v %v", got, err)
	}
}

func TestRunChatPersistenceAndCap(t *testing.T) {
	f := newTogetherFx(t, func(o *hub.Options) { o.RunChatMax = 3 })
	for i := 1; i <= 5; i++ {
		f.clk.Advance(time.Second)
		chatSend(t, f.cAdmin, "run", f.run.ID, fmt.Sprint("m", i), fmt.Sprint("msg ", i))
	}
	got, next, err := f.svc.ListRunChat(ctx, f.anna.ID, f.run.ID, 0, 100)
	if err != nil || next != nil || len(got) != 3 || got[0].Seq != 3 || got[2].Text != "msg 5" {
		t.Fatalf("cap: %+v %v %v", got, next, err)
	}
	got, next, _ = f.svc.ListRunChat(ctx, f.anna.ID, f.run.ID, 3, 1)
	if len(got) != 1 || got[0].Seq != 4 || next == nil || *next != 4 {
		t.Fatalf("paging: %+v %v", got, next)
	}
	if _, _, err := f.svc.ListRunChat(ctx, f.bob.ID, f.run.ID, 0, 10); err == nil {
		t.Fatal("non-participant must be refused")
	}
	// a resend of a trimmed message is a new message
	chatSend(t, f.cAdmin, "run", f.run.ID, "m1", "msg 1 again")
	if ms, _ := chats(f.cAnna); ms[len(ms)-1].Seq != 6 {
		t.Fatalf("seq continues: %+v", ms)
	}
}

func TestSessionChatRingAndDrop(t *testing.T) {
	svc, clk := hubtest.New(t, nil)
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
	chats(co)
	chats(ca)
	chats(cb)
	var seen []chatMsg
	for i := 1; i <= 210; i++ {
		clk.Advance(time.Second)
		ms, _ := chats(ca) // the outgoing queue is bounded: read as we go
		seen = append(seen, ms...)
		chats(co)
		chatSend(t, co, "session", s.SessionID, fmt.Sprint("s", i), fmt.Sprint("line ", i, " @anna"))
	}
	ms, errs := chats(ca)
	seen = append(seen, ms...)
	if ms := seen; len(errs) != 0 || len(ms) != 210 || ms[209].Seq != 210 || len(ms[0].Mentions) != 1 {
		t.Fatalf("viewer: %d msgs %v", len(ms), errs)
	}
	if ms, _ := chats(cb); len(ms) != 0 {
		t.Fatal("non-viewer received session chat")
	}
	ring, err := svc.ListSessionChat(ctx, pa, s.SessionID)
	if err != nil || len(ring) != 200 || ring[0].Seq != 11 || ring[199].Seq != 210 {
		t.Fatalf("ring: %d %v", len(ring), err)
	}
	if _, err := svc.ListSessionChat(ctx, pb, s.SessionID); err == nil {
		t.Fatal("non-viewer must not read the ring")
	}
	if err := svc.EndSession(ctx, po, s.SessionID); err != nil {
		t.Fatal(err)
	}
	if _, err := svc.ListSessionChat(ctx, pa, s.SessionID); err == nil {
		t.Fatal("ended session has no chat")
	}
	chatSend(t, co, "session", s.SessionID, "late", "x")
	if _, errs := chats(co); len(errs) == 0 || errs[len(errs)-1] != "forbidden" {
		t.Fatalf("send after end: %v", errs)
	}
}
