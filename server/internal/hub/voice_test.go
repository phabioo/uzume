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

type vMember struct {
	UserID      string `json:"user_id"`
	DeviceID    string `json:"device_id"`
	Mic         string `json:"mic"`
	MutedByHost bool   `json:"muted_by_host"`
}

type vRoom struct {
	Scope   string    `json:"scope"`
	ID      string    `json:"id"`
	Members []vMember `json:"members"`
}

// voiceGot drains c and returns the voice_room messages, the voice_signal payloads and the error codes.
func voiceGot(c *hub.Client) (rooms []vRoom, sigs []map[string]any, errs []string) {
	for _, raw := range drain(c) {
		var env struct {
			Type    string          `json:"type"`
			Payload json.RawMessage `json:"payload"`
		}
		if json.Unmarshal([]byte(raw), &env) != nil {
			continue
		}
		switch env.Type {
		case "voice_room":
			var r vRoom
			_ = json.Unmarshal(env.Payload, &r)
			rooms = append(rooms, r)
		case "voice_signal":
			var m map[string]any
			_ = json.Unmarshal(env.Payload, &m)
			sigs = append(sigs, m)
		case "error":
			var m map[string]string
			_ = json.Unmarshal(env.Payload, &m)
			errs = append(errs, m["code"])
		}
	}
	return
}

func lastRoom(t *testing.T, c *hub.Client) vRoom {
	t.Helper()
	rs, _, errs := voiceGot(c)
	if len(rs) == 0 {
		t.Fatalf("no voice_room (errors %v)", errs)
	}
	return rs[len(rs)-1]
}

func vj(t *testing.T, c *hub.Client, scope, id string) {
	t.Helper()
	send(t, c, "voice_join", fmt.Sprintf(`{"scope":%q,"id":%q}`, scope, id))
}

func wantErr(t *testing.T, what string, c *hub.Client, code string) {
	t.Helper()
	if _, _, errs := voiceGot(c); len(errs) != 1 || errs[0] != code {
		t.Fatalf("%s: errors %v, want %s", what, errs, code)
	}
}

func TestVoiceJoinLeaveAndStates(t *testing.T) {
	f := newTogetherFx(t, nil)
	vj(t, f.cAdmin, "run", f.run.ID)
	r := lastRoom(t, f.cAdmin)
	if len(r.Members) != 1 || r.Members[0].UserID != f.admin.ID || r.Members[0].Mic != "ptt" || r.Members[0].DeviceID != f.pAdmin.Device.ID {
		t.Fatalf("room: %+v", r)
	}
	vj(t, f.cAnna, "run", f.run.ID)
	if r := lastRoom(t, f.cAdmin); len(r.Members) != 2 {
		t.Fatalf("admin must see anna: %+v", r)
	}
	if r := lastRoom(t, f.cAnna); len(r.Members) != 2 {
		t.Fatalf("joiner must get the room: %+v", r)
	}
	send(t, f.cAnna, "voice_state", fmt.Sprintf(`{"scope":"run","id":%q,"mic":"on"}`, f.run.ID))
	if r := lastRoom(t, f.cAdmin); r.Members[1].Mic != "on" {
		t.Fatalf("mic: %+v", r)
	}
	send(t, f.cAnna, "voice_state", fmt.Sprintf(`{"scope":"run","id":%q,"mic":"loud"}`, f.run.ID))
	wantErr(t, "bad mic", f.cAnna, "bad_request")
	vj(t, f.cBob, "run", f.run.ID)
	wantErr(t, "non-member", f.cBob, "forbidden")
	send(t, f.cAnna, "voice_leave", fmt.Sprintf(`{"scope":"run","id":%q}`, f.run.ID))
	if r := lastRoom(t, f.cAdmin); len(r.Members) != 1 {
		t.Fatalf("after leave: %+v", r)
	}
	if r := lastRoom(t, f.cAnna); len(r.Members) != 1 { // the leaver is told too
		t.Fatalf("leaver: %+v", r)
	}
}

func TestVoiceRoomCap(t *testing.T) {
	f := newTogetherFx(t, nil)
	cs := []*hub.Client{f.cAdmin}
	for i := 1; i < 8; i++ {
		f.clk.Advance(2 * time.Minute) // pairing is rate limited per source
		cs = append(cs, connect(t, f.svc, principal(t, f.svc, f.admin.ID, fmt.Sprintf("Dev%d", i))))
	}
	for _, c := range cs {
		vj(t, c, "run", f.run.ID)
	}
	if r := lastRoom(t, f.cAdmin); len(r.Members) != 8 {
		t.Fatalf("8 expected: %+v", r)
	}
	got(f.cAnna)
	vj(t, f.cAnna, "run", f.run.ID)
	wantErr(t, "ninth device", f.cAnna, "conflict")
	vj(t, cs[3], "run", f.run.ID) // re-join of a member is idempotent
	if r := lastRoom(t, f.cAdmin); len(r.Members) != 8 {
		t.Fatalf("rejoin: %+v", r)
	}
}

func TestVoiceOneRoomPerDevice(t *testing.T) {
	svc, _ := hubtest.New(t, nil)
	admin, _ := svc.CreateAdmin(ctx, "admin", "secret-12345")
	g, err := svc.AddROM(ctx, strings.NewReader("homebrew-rom-dummy"), "demo.nds", "Demo", "", admin.ID)
	if err != nil {
		t.Fatal(err)
	}
	run, err := svc.CreateRun(ctx, hub.CreateRunInput{UserID: admin.ID, Name: "Run", GameID: g.ID})
	if err != nil {
		t.Fatal(err)
	}
	po, po2 := principal(t, svc, admin.ID, "Desktop"), principal(t, svc, admin.ID, "Laptop")
	co, co2 := connect(t, svc, po), connect(t, svc, po2)
	s, err := svc.PublishSession(ctx, po, g.ID, hub.VisibilityHubUsers)
	if err != nil {
		t.Fatal(err)
	}
	drain(co)
	drain(co2)
	vj(t, co, "run", run.ID)
	vj(t, co2, "run", run.ID)
	voiceGot(co)
	vj(t, co, "session", s.SessionID) // leaves the run room
	rs, _, _ := voiceGot(co2)
	if len(rs) == 0 {
		t.Fatal("no room update")
	}
	last := rs[len(rs)-1]
	if last.Scope != "run" || len(last.Members) != 1 || last.Members[0].DeviceID != po2.Device.ID {
		t.Fatalf("run room after move: %+v", rs)
	}
	if r := lastRoom(t, co); r.Scope != "session" || len(r.Members) != 1 {
		t.Fatalf("session room: %+v", r)
	}
}

func TestVoiceSignalRelay(t *testing.T) {
	f := newTogetherFx(t, nil)
	vj(t, f.cAdmin, "run", f.run.ID)
	vj(t, f.cAnna, "run", f.run.ID)
	voiceGot(f.cAdmin)
	voiceGot(f.cAnna)
	sig := func(c *hub.Client, to, kind string) {
		send(t, c, "voice_signal", fmt.Sprintf(`{"scope":"run","id":%q,"to_device_id":%q,"kind":%q,"sdp":"v=0","extra":{"a":1}}`, f.run.ID, to, kind))
	}
	sig(f.cAdmin, f.pAnna.Device.ID, "offer")
	_, sigs, _ := voiceGot(f.cAnna)
	if len(sigs) != 1 || sigs[0]["from_device_id"] != f.pAdmin.Device.ID || sigs[0]["sdp"] != "v=0" || sigs[0]["kind"] != "offer" || sigs[0]["extra"] == nil || sigs[0]["to_device_id"] != nil {
		t.Fatalf("relay: %+v", sigs)
	}
	sig(f.cBob, f.pAnna.Device.ID, "offer") // outsider
	wantErr(t, "outsider", f.cBob, "forbidden")
	sig(f.cAdmin, f.pB.Device.ID, "offer") // target outside the room
	wantErr(t, "target outside room", f.cAdmin, "forbidden")
	sig(f.cAdmin, "00000000-0000-0000-0000-000000000000", "offer")
	wantErr(t, "unknown device", f.cAdmin, "forbidden")
	sig(f.cAdmin, f.pAnna.Device.ID, "bogus")
	wantErr(t, "bad kind", f.cAdmin, "bad_request")
	if _, sigs, _ := voiceGot(f.cAnna); len(sigs) != 0 {
		t.Fatalf("leak: %+v", sigs)
	}
}

func TestVoiceDisconnectAndRunLeave(t *testing.T) {
	f := newTogetherFx(t, nil)
	vj(t, f.cAdmin, "run", f.run.ID)
	vj(t, f.cAnna, "run", f.run.ID)
	voiceGot(f.cAdmin)
	voiceGot(f.cAnna)
	f.cAnna.Disconnect()
	if r := lastRoom(t, f.cAdmin); len(r.Members) != 1 || r.Members[0].UserID != f.admin.ID {
		t.Fatalf("disconnect: %+v", r)
	}
	ca := connect(t, f.svc, f.pAnna)
	drain(ca)
	vj(t, ca, "run", f.run.ID)
	voiceGot(f.cAdmin)
	if _, err := f.svc.LeaveRun(ctx, f.anna.ID, f.run.ID); err != nil {
		t.Fatal(err)
	}
	if r := lastRoom(t, f.cAdmin); len(r.Members) != 1 {
		t.Fatalf("member left the run: %+v", r)
	}
	if r := lastRoom(t, ca); len(r.Members) != 1 {
		t.Fatalf("leaver is told: %+v", r)
	}
}

func TestVoiceSessionMuteAndEnd(t *testing.T) {
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
	drain(co)
	drain(ca)
	drain(cb)
	vj(t, co, "session", s.SessionID)
	vj(t, ca, "session", s.SessionID)
	vj(t, cb, "session", s.SessionID) // bob is no viewer
	wantErr(t, "non-member", cb, "forbidden")
	mute := func(c *hub.Client, user string, muted bool) {
		send(t, c, "voice_mute", fmt.Sprintf(`{"scope":"session","id":%q,"user_id":%q,"muted":%v}`, s.SessionID, user, muted))
	}
	voiceGot(co)
	voiceGot(ca)
	mute(ca, anna.ID, true) // a viewer cannot mute
	wantErr(t, "viewer mute", ca, "forbidden")
	mute(co, bob.ID, true) // not a member
	wantErr(t, "mute non-member", co, "not_found")
	mute(co, anna.ID, true)
	r := lastRoom(t, ca)
	if !r.Members[1].MutedByHost || r.Members[0].MutedByHost {
		t.Fatalf("mute: %+v", r)
	}
	voiceGot(co)
	send(t, ca, "voice_leave", fmt.Sprintf(`{"scope":"session","id":%q}`, s.SessionID))
	vj(t, ca, "session", s.SessionID)
	if r := lastRoom(t, co); !r.Members[1].MutedByHost {
		t.Fatalf("mute must survive rejoin: %+v", r)
	}
	mute(co, anna.ID, false)
	if r := lastRoom(t, ca); r.Members[1].MutedByHost {
		t.Fatalf("unmute: %+v", r)
	}
	if err := svc.EndSession(ctx, po, s.SessionID); err != nil {
		t.Fatal(err)
	}
	for _, c := range []*hub.Client{co, ca} {
		if r := lastRoom(t, c); len(r.Members) != 0 {
			t.Fatalf("session end must close the room: %+v", r)
		}
	}
	vj(t, co, "session", s.SessionID)
	wantErr(t, "join ended session", co, "forbidden")
}
