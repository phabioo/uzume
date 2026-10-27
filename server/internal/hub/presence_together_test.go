package hub_test

import (
	"bytes"
	"encoding/json"
	"fmt"
	"slices"
	"testing"

	"github.com/google/uuid"
	"github.com/phabioo/uzume/server/internal/hub"
	"github.com/phabioo/uzume/server/internal/hub/hubtest"
)

// connect opens a WSS client for the principal (hello handled, registered).
func connect(t *testing.T, svc *hub.Service, p hub.Principal) *hub.Client {
	t.Helper()
	cl := svc.NewClient(p)
	if err := cl.Handle([]byte(fmt.Sprintf(`{"type":"hello","payload":{"protocol_version":1,"device_id":%q}}`, p.Device.ID))); err != nil {
		t.Fatal(err)
	}
	return cl
}

// lastPresence drains c and returns the newest presence_update payload about deviceID (nil = none).
func lastPresence(c *hub.Client, deviceID string) map[string]string {
	var last map[string]string
	for _, m := range drain(c) {
		var env struct {
			Type    string            `json:"type"`
			Payload map[string]string `json:"payload"`
		}
		if json.Unmarshal([]byte(m), &env) == nil && env.Type == "presence_update" && env.Payload["device_id"] == deviceID {
			last = env.Payload
		}
	}
	return last
}

func TestPresenceTogetherFieldsAndSessionACL(t *testing.T) {
	svc, _ := hubtest.New(t, nil)
	admin, _ := svc.CreateAdmin(ctx, "fabio", "secret-12345")
	anna, _ := svc.CreateUser(ctx, "anna", "Anna")
	bob, _ := svc.CreateUser(ctx, "bob", "Bob")
	g, err := svc.AddROM(ctx, bytes.NewReader(randomROM(2000)), "demo.nds", "", "", admin.ID)
	if err != nil {
		t.Fatal(err)
	}
	owner, pa, pb := principal(t, svc, admin.ID, "Desktop"), principal(t, svc, anna.ID, "Anna-PC"), principal(t, svc, bob.ID, "Bob-PC")
	od := owner.Device.ID
	co := connect(t, svc, owner)
	ca, cb := connect(t, svc, pa), connect(t, svc, pb)
	drain(ca)
	drain(cb)
	drain(co)

	play := func() {
		t.Helper()
		// the client session_id is accepted and ignored
		if err := co.Handle([]byte(fmt.Sprintf(`{"type":"presence_update","payload":{"state":"in_game","game_id":%q,"session_id":%q}}`, g.ID, uuid.NewString()))); err != nil {
			t.Fatal(err)
		}
	}
	play()
	p := lastPresence(ca, od)
	if p == nil || p["display_name"] != "fabio" || p["state"] != "in_game" || p["game_id"] != g.ID ||
		p["game_title"] != g.Title || p["system"] != g.System || p["game_title"] == "" {
		t.Fatalf("presence fields: %+v", p)
	}
	if _, ok := p["session_id"]; ok {
		t.Fatalf("the ignored client session_id must not be echoed: %+v", p)
	}
	drain(cb)

	// Private: nobody else sees the session_id.
	s, err := svc.PublishSession(ctx, owner, g.ID, hub.VisibilityPrivate)
	if err != nil {
		t.Fatal(err)
	}
	if p := lastPresence(ca, od); p != nil && p["session_id"] != "" {
		t.Fatalf("private session_id leaked: %+v", p)
	}
	play() // a fresh broadcast must not leak either
	if p := lastPresence(cb, od); p == nil || p["session_id"] != "" {
		t.Fatalf("private broadcast: %+v", p)
	}
	drain(ca)

	// Invite only: appears for the invited user, not for the other one.
	if _, err := svc.SetSessionVisibility(ctx, owner, s.SessionID, hub.VisibilityInviteOnly); err != nil {
		t.Fatal(err)
	}
	if p := lastPresence(ca, od); p != nil && p["session_id"] != "" {
		t.Fatalf("uninvited sees session_id: %+v", p)
	}
	if _, err := svc.InviteUser(ctx, owner, s.SessionID, anna.ID); err != nil {
		t.Fatal(err)
	}
	if p := lastPresence(ca, od); p == nil || p["session_id"] != s.SessionID {
		t.Fatalf("invited user must get session_id: %+v", p)
	}
	if p := lastPresence(cb, od); p != nil && p["session_id"] != "" {
		t.Fatalf("not invited sees session_id: %+v", p)
	}

	// Snapshot on attach follows the same rules.
	ca2 := connect(t, svc, principal(t, svc, anna.ID, "Anna-Laptop"))
	if p := lastPresence(ca2, od); p == nil || p["session_id"] != s.SessionID {
		t.Fatalf("snapshot for invited: %+v", p)
	}
	cb2 := connect(t, svc, principal(t, svc, bob.ID, "Bob-Laptop"))
	if p := lastPresence(cb2, od); p == nil || p["session_id"] != "" || p["game_title"] == "" {
		t.Fatalf("snapshot for uninvited: %+v", p)
	}

	// hub_users: everybody; ending removes it again.
	if _, err := svc.SetSessionVisibility(ctx, owner, s.SessionID, hub.VisibilityHubUsers); err != nil {
		t.Fatal(err)
	}
	if p := lastPresence(cb, od); p == nil || p["session_id"] != s.SessionID {
		t.Fatalf("hub_users: %+v", p)
	}
	if err := svc.EndSession(ctx, owner, s.SessionID); err != nil {
		t.Fatal(err)
	}
	if p := lastPresence(cb, od); p == nil || p["session_id"] != "" {
		t.Fatalf("after end: %+v", p)
	}
}

func TestPresenceUnknownGameOmitsTitle(t *testing.T) {
	svc, _ := hubtest.New(t, nil)
	admin, _ := svc.CreateAdmin(ctx, "fabio", "secret-12345")
	anna, _ := svc.CreateUser(ctx, "anna", "Anna")
	op := principal(t, svc, admin.ID, "Desktop")
	co := connect(t, svc, op)
	ca := connect(t, svc, principal(t, svc, anna.ID, "Anna-PC"))
	drain(ca)
	game := uuid.NewString() // not in the library
	if err := co.Handle([]byte(fmt.Sprintf(`{"type":"presence_update","payload":{"state":"in_game","game_id":%q}}`, game))); err != nil {
		t.Fatal(err)
	}
	p := lastPresence(ca, op.Device.ID)
	if p == nil || p["game_id"] != game {
		t.Fatalf("presence: %+v", p)
	}
	if _, ok := p["game_title"]; ok {
		t.Fatalf("game_title for unknown game: %+v", p)
	}
	if _, ok := p["system"]; ok {
		t.Fatalf("system for unknown game: %+v", p)
	}
}

func TestHelloAckAdvertisesTogether(t *testing.T) {
	svc, _ := hubtest.New(t, nil)
	admin, _ := svc.CreateAdmin(ctx, "fabio", "secret-12345")
	c := connect(t, svc, principal(t, svc, admin.ID, "Desktop"))
	for _, m := range drain(c) {
		var env struct {
			Type    string `json:"type"`
			Payload struct {
				Features []string `json:"features"`
			} `json:"payload"`
		}
		if json.Unmarshal([]byte(m), &env) == nil && env.Type == "hello_ack" {
			if !slices.Contains(env.Payload.Features, hub.FeatureTogetherV1) {
				t.Fatalf("features: %v", env.Payload.Features)
			}
			return
		}
	}
	t.Fatal("no hello_ack")
}
