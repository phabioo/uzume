package httpapi

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"slices"
	"strings"
	"testing"
	"time"

	"github.com/coder/websocket"
	"github.com/google/uuid"

	"github.com/phabioo/uzume/server/internal/hub"
)

// Sessions over REST and a real WSS (httptest server), ADR 0006 D1-D4.

type sessEnv struct {
	*env
	srv  *httptest.Server
	game hub.Game
	anna hub.User // foreign user
	bob  hub.User // second foreign user
	ips  int
}

type dev struct {
	id, tok, user, name string
}

func newSessEnv(t *testing.T, mod func(*hub.Options), opts ...Option) *sessEnv {
	t.Helper()
	e := newEnv(t, mod, opts...)
	srv := httptest.NewServer(e.mux)
	t.Cleanup(srv.Close)
	g, err := e.svc.AddROM(context.Background(), bytes.NewReader([]byte("homebrew-rom-dummy")), "demo.nds", "Demo Homebrew", "", e.admin.ID)
	if err != nil {
		t.Fatal(err)
	}
	s := &sessEnv{env: e, srv: srv, game: g}
	if s.anna, err = e.svc.CreateUser(context.Background(), "anna", "Anna"); err != nil {
		t.Fatal(err)
	}
	if s.bob, err = e.svc.CreateUser(context.Background(), "bob", "Bob"); err != nil {
		t.Fatal(err)
	}
	return s
}

// device pairs a new device for the user and returns it with an access token.
func (s *sessEnv) device(userID, name string) dev {
	s.t.Helper()
	ctx := context.Background()
	id := uuid.NewString()
	s.ips++ // the server rate-limits pairing requests per IP
	c, err := s.svc.CreatePairingRequest(ctx, hub.PairingInput{DeviceID: id, DeviceName: name, Platform: "linux", Arch: "x86_64",
		PlayerVersion: "0.1.0", ProtocolVersion: 1, RemoteAddr: fmt.Sprintf("10.0.%d.%d", s.ips/250, s.ips%250)})
	if err != nil {
		s.t.Fatal(err)
	}
	if err := s.svc.ApprovePairing(ctx, c.RequestID, userID); err != nil {
		s.t.Fatal(err)
	}
	res, err := s.svc.PollPairing(ctx, c.RequestID, c.PollToken)
	if err != nil {
		s.t.Fatal(err)
	}
	tok, err := s.svc.IssueAccessToken(ctx, id, res.DeviceCredential)
	if err != nil {
		s.t.Fatal(err)
	}
	return dev{id: id, tok: tok.Token, user: userID, name: name}
}

func (s *sessEnv) api(d dev, method, path string, body any) *httptest.ResponseRecorder {
	s.t.Helper()
	return s.do(method, path, body, opt{token: d.tok})
}

type sessResp struct {
	SessionID string `json:"session_id"`
	GameTitle string `json:"game_title"`
	Owner     struct {
		UserID      string `json:"user_id"`
		DisplayName string `json:"display_name"`
		DeviceName  string `json:"device_name"`
	} `json:"owner"`
	Visibility  string `json:"visibility"`
	ViewerCount int    `json:"viewer_count"`
	Viewers     *[]struct {
		ViewerID    string `json:"viewer_id"`
		DisplayName string `json:"display_name"`
		DeviceName  string `json:"device_name"`
	} `json:"viewers"`
	Invites *[]struct {
		UserID string `json:"user_id"`
		State  string `json:"state"`
		Online bool   `json:"online"`
	} `json:"invites"`
	IsOwner bool `json:"is_owner"`
	Invited bool `json:"invited"`
}

func (s *sessEnv) publish(d dev, vis string) sessResp {
	s.t.Helper()
	rec := s.api(d, "POST", "/api/v1/sessions", map[string]any{"game_id": s.game.ID, "visibility": vis})
	wantStatus(s.t, rec, 201, "")
	return decode[sessResp](s.t, rec)
}

func (s *sessEnv) list(d dev) []sessResp {
	s.t.Helper()
	rec := s.api(d, "GET", "/api/v1/sessions", nil)
	wantStatus(s.t, rec, 200, "")
	return decode[struct {
		Sessions []sessResp `json:"sessions"`
	}](s.t, rec).Sessions
}

func (s *sessEnv) get(d dev, id string) *httptest.ResponseRecorder {
	s.t.Helper()
	return s.api(d, "GET", "/api/v1/sessions/"+id, nil)
}

func (s *sessEnv) patch(d dev, id, vis string) *httptest.ResponseRecorder {
	s.t.Helper()
	return s.api(d, "PATCH", "/api/v1/sessions/"+id, map[string]any{"visibility": vis})
}

type joinResp struct {
	ViewerID    string `json:"viewer_id"`
	Permissions struct {
		ViewVideo bool `json:"view_video"`
		HearAudio bool `json:"hear_audio"`
		SendInput bool `json:"send_input"`
	} `json:"permissions"`
	IceServers []string `json:"ice_servers"`
}

func (s *sessEnv) join(d dev, id string) *httptest.ResponseRecorder {
	s.t.Helper()
	return s.api(d, "POST", "/api/v1/sessions/"+id+"/join", nil)
}

func (s *sessEnv) mustJoin(d dev, id string) string {
	s.t.Helper()
	rec := s.join(d, id)
	wantStatus(s.t, rec, 201, "")
	return decode[joinResp](s.t, rec).ViewerID
}

func (s *sessEnv) invite(d dev, id, userID string) *httptest.ResponseRecorder {
	s.t.Helper()
	return s.api(d, "PUT", "/api/v1/sessions/"+id+"/invites/"+userID, nil)
}

// ---- WSS test client ----

type wsMsg struct {
	Type    string          `json:"type"`
	ID      string          `json:"id"`
	Payload json.RawMessage `json:"payload"`
}

func (m wsMsg) field(name string) string {
	var p map[string]any
	json.Unmarshal(m.Payload, &p)
	v, _ := p[name].(string)
	return v
}

func (m wsMsg) session() sessResp {
	var p struct{ Session sessResp }
	json.Unmarshal(m.Payload, &p)
	return p.Session
}

type wsClient struct {
	t      *testing.T
	conn   *websocket.Conn
	msgs   chan wsMsg
	closed chan error
}

func (s *sessEnv) wsURL() string { return "ws" + strings.TrimPrefix(s.srv.URL, "http") + "/api/v1/ws" }

// dialRaw opens the WSS connection without sending hello.
func (s *sessEnv) dialRaw(d dev) *wsClient {
	s.t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	conn, _, err := websocket.Dial(ctx, s.wsURL(), &websocket.DialOptions{HTTPHeader: http.Header{"Authorization": {"Bearer " + d.tok}}})
	if err != nil {
		s.t.Fatal(err)
	}
	c := &wsClient{t: s.t, conn: conn, msgs: make(chan wsMsg, 256), closed: make(chan error, 1)}
	go func() {
		for {
			_, data, err := conn.Read(context.Background())
			if err != nil {
				c.closed <- err
				return
			}
			if err := validateWS(s.t, data); err != nil {
				s.t.Errorf("message from the server violates its schema: %v: %s", err, data)
			}
			var m wsMsg
			json.Unmarshal(data, &m)
			c.msgs <- m
		}
	}()
	s.t.Cleanup(func() { conn.CloseNow() })
	return c
}

// dial connects, sends hello and waits for hello_ack.
func (s *sessEnv) dial(d dev) *wsClient {
	s.t.Helper()
	c := s.dialRaw(d)
	c.send("hello", map[string]any{"protocol_version": 1, "device_id": d.id})
	c.await("hello_ack")
	// The server sends hello_ack before it registers the connection (attach); wait for that, so a push right after
	// dial (ice_update, run_updated) cannot miss this client.
	for deadline := time.Now().Add(5 * time.Second); !slices.Contains(s.svc.ConnectedDeviceIDs(), d.id); {
		if time.Now().After(deadline) {
			s.t.Fatalf("device %s not registered after hello_ack", d.id)
		}
		time.Sleep(5 * time.Millisecond)
	}
	return c
}

func (c *wsClient) send(typ string, payload any) {
	c.t.Helper()
	b, _ := json.Marshal(map[string]any{"type": typ, "id": "x", "payload": payload})
	if err := validateWS(c.t, b); err != nil && typ != "bogus" {
		c.t.Fatalf("test sends an invalid message: %v", err)
	}
	if err := c.conn.Write(context.Background(), websocket.MessageText, b); err != nil {
		c.t.Fatal(err)
	}
}

func (c *wsClient) sendRaw(b []byte) {
	c.t.Helper()
	if err := c.conn.Write(context.Background(), websocket.MessageText, b); err != nil {
		c.t.Fatal(err)
	}
}

// awaitWhere skips messages until one of the type satisfies pred.
func (c *wsClient) awaitWhere(typ string, pred func(wsMsg) bool) wsMsg {
	c.t.Helper()
	deadline := time.After(3 * time.Second)
	for {
		select {
		case m := <-c.msgs:
			if m.Type == typ && (pred == nil || pred(m)) {
				return m
			}
		case err := <-c.closed:
			c.t.Fatalf("connection closed while waiting for %s: %v", typ, err)
		case <-deadline:
			c.t.Fatalf("timeout waiting for %s", typ)
		}
	}
}

func (c *wsClient) await(typ string) wsMsg { c.t.Helper(); return c.awaitWhere(typ, nil) }

// settle drops everything that is queued or arrives within a short quiet period.
func (c *wsClient) settle() {
	for {
		select {
		case <-c.msgs:
		case <-time.After(150 * time.Millisecond):
			return
		}
	}
}

// quiet fails if a message of one of the types arrives within the period (no types: any message).
func (c *wsClient) quiet(types ...string) {
	c.t.Helper()
	deadline := time.After(250 * time.Millisecond)
	for {
		select {
		case m := <-c.msgs:
			if len(types) == 0 {
				c.t.Fatalf("unexpected message %s", m.Type)
			}
			for _, ty := range types {
				if m.Type == ty {
					c.t.Fatalf("unexpected message %s: %s", m.Type, m.Payload)
				}
			}
		case <-deadline:
			return
		}
	}
}

// wantClosed waits for the connection to be closed by the server and returns the close status.
func (c *wsClient) wantClosed() websocket.StatusCode {
	c.t.Helper()
	deadline := time.After(3 * time.Second)
	for {
		select {
		case <-c.msgs:
		case err := <-c.closed:
			return websocket.CloseStatus(err)
		case <-deadline:
			c.t.Fatal("connection was not closed")
		}
	}
}

func (c *wsClient) wantError(code string) {
	c.t.Helper()
	m := c.await("error")
	if got := m.field("code"); got != code {
		c.t.Fatalf("error code %q, want %q", got, code)
	}
}

func isSession(id string) func(wsMsg) bool {
	return func(m wsMsg) bool { return m.session().SessionID == id || m.field("session_id") == id }
}

// ---- tests ----

func TestSessionACLPerVisibility(t *testing.T) {
	s := newSessEnv(t, nil)
	a1, a2 := s.device(s.admin.ID, "Desktop"), s.device(s.admin.ID, "Laptop")
	b1, c1 := s.device(s.anna.ID, "Anna-Laptop"), s.device(s.bob.ID, "Bob-PC")

	// private: only devices of the owner user.
	sess := s.publish(a1, "private")
	if sess.GameTitle != "Demo Homebrew" || sess.Owner.UserID != s.admin.ID || sess.Owner.DeviceName != "Desktop" || !sess.IsOwner ||
		sess.Viewers == nil || sess.Invites == nil || sess.Invited {
		t.Fatalf("%+v", sess)
	}
	if len(s.list(a1)) != 1 || len(s.list(a2)) != 1 || len(s.list(b1)) != 0 || len(s.list(c1)) != 0 {
		t.Fatal("private Session must be listed for the owner user's devices only")
	}
	rec := s.get(a2, sess.SessionID)
	wantStatus(t, rec, 200, "")
	if o := decode[sessResp](t, rec); o.IsOwner || o.Viewers != nil || o.Invites != nil {
		t.Fatalf("owner-only fields leaked to another device: %+v", o)
	}
	wantStatus(t, s.get(b1, sess.SessionID), 403, "session_forbidden")
	wantStatus(t, s.join(b1, sess.SessionID), 403, "session_forbidden")
	wantStatus(t, s.get(a1, uuid.NewString()), 404, "session_not_found")
	rec = s.join(a2, sess.SessionID)
	wantStatus(t, rec, 201, "")
	if j := decode[joinResp](t, rec); !j.Permissions.ViewVideo || !j.Permissions.HearAudio || j.Permissions.SendInput || j.IceServers == nil {
		t.Fatalf("%+v", j)
	}
	// The owner device cannot join its own Session; only the owner device may change it.
	wantStatus(t, s.join(a1, sess.SessionID), 400, "bad_request")
	wantStatus(t, s.patch(a2, sess.SessionID, "hub_users"), 403, "session_forbidden")
	wantStatus(t, s.do("PATCH", "/api/v1/sessions/"+sess.SessionID, map[string]any{"visibility": "everyone"}, opt{token: a1.tok, raw: true}), 400, "bad_request")
	wantStatus(t, s.api(a2, "DELETE", "/api/v1/sessions/"+sess.SessionID, nil), 403, "session_forbidden")

	// hub_users: every device.
	wantStatus(t, s.patch(a1, sess.SessionID, "hub_users"), 200, "")
	if len(s.list(b1)) != 1 || len(s.list(c1)) != 1 {
		t.Fatal("hub_users Session must be listed for foreign users")
	}
	rec = s.get(b1, sess.SessionID)
	wantStatus(t, rec, 200, "")
	if o := decode[sessResp](t, rec); o.IsOwner || o.Viewers != nil || o.Invites != nil || o.ViewerCount != 1 {
		t.Fatalf("%+v", o)
	}
	s.mustJoin(b1, sess.SessionID)
	s.mustJoin(c1, sess.SessionID)
	if o := decode[sessResp](t, s.get(a1, sess.SessionID)); o.Viewers == nil || len(*o.Viewers) != 3 || o.ViewerCount != 3 {
		t.Fatalf("owner sees %+v", o)
	}

	// invite_only: owner user plus invited users; the foreign viewers are revoked, the own device stays.
	wantStatus(t, s.patch(a1, sess.SessionID, "invite_only"), 200, "")
	if o := decode[sessResp](t, s.get(a1, sess.SessionID)); o.ViewerCount != 1 || (*o.Viewers)[0].DeviceName != "Laptop" {
		t.Fatalf("viewers after PATCH: %+v", o)
	}
	wantStatus(t, s.get(b1, sess.SessionID), 403, "session_forbidden")
	wantStatus(t, s.join(c1, sess.SessionID), 403, "session_forbidden")
	if len(s.list(b1)) != 0 {
		t.Fatal("invite_only Session listed for an uninvited user")
	}
	// Invite anna: she sees and may join; bob still not.
	wantStatus(t, s.invite(a2, sess.SessionID, s.anna.ID), 403, "session_forbidden")
	wantStatus(t, s.invite(a1, sess.SessionID, "u_nobody"), 404, "not_found")
	wantStatus(t, s.invite(a1, sess.SessionID, s.admin.ID), 400, "bad_request")
	wantStatus(t, s.invite(a1, sess.SessionID, s.anna.ID), 200, "")
	l := s.list(b1)
	if len(l) != 1 || !l[0].Invited || l[0].IsOwner {
		t.Fatalf("invitee list: %+v", l)
	}
	wantStatus(t, s.join(c1, sess.SessionID), 403, "session_forbidden")
	s.mustJoin(b1, sess.SessionID)
	o := decode[sessResp](t, s.get(a1, sess.SessionID))
	if o.Invites == nil || len(*o.Invites) != 1 || (*o.Invites)[0].UserID != s.anna.ID || (*o.Invites)[0].State != "joined" {
		t.Fatalf("invites: %+v", o.Invites)
	}
	// Ended Session: 410 for everyone, also for the owner's calls.
	wantStatus(t, s.api(a1, "DELETE", "/api/v1/sessions/"+sess.SessionID, nil), 204, "")
	wantStatus(t, s.get(b1, sess.SessionID), 410, "session_ended")
	wantStatus(t, s.join(a2, sess.SessionID), 410, "session_ended")
	wantStatus(t, s.patch(a1, sess.SessionID, "private"), 410, "session_ended")
	if len(s.list(a1)) != 0 || len(s.list(b1)) != 0 {
		t.Fatal("ended Session still listed (invites must expire with it)")
	}
}

func TestSessionEventsFanOutToAllowedDevicesOnly(t *testing.T) {
	s := newSessEnv(t, nil)
	a1, a2 := s.device(s.admin.ID, "Desktop"), s.device(s.admin.ID, "Laptop")
	b1, c1 := s.device(s.anna.ID, "Anna-Laptop"), s.device(s.bob.ID, "Bob-PC")
	w1, w2, wb, wc := s.dial(a1), s.dial(a2), s.dial(b1), s.dial(c1)
	for _, w := range []*wsClient{w1, w2, wb, wc} {
		w.settle()
	}

	// private: owner user's devices only.
	sess := s.publish(a1, "private")
	m := w1.awaitWhere("session_update", isSession(sess.SessionID))
	if got := m.session(); !got.IsOwner || got.Viewers == nil {
		t.Fatalf("owner device update: %+v", got)
	}
	if got := w2.awaitWhere("session_update", isSession(sess.SessionID)).session(); got.IsOwner || got.Viewers != nil || got.Invites != nil {
		t.Fatalf("second device update: %+v", got)
	}
	wb.quiet("session_update", "session_ended")
	wc.quiet("session_update", "session_ended")

	// hub_users: everybody; the foreign devices see the Session (not the owner-only fields).
	wantStatus(t, s.patch(a1, sess.SessionID, "hub_users"), 200, "")
	for _, w := range []*wsClient{wb, wc} {
		got := w.awaitWhere("session_update", isSession(sess.SessionID)).session()
		if got.Visibility != "hub_users" || got.IsOwner || got.Viewers != nil || got.Invites != nil {
			t.Fatalf("foreign device update: %+v", got)
		}
	}
	w1.awaitWhere("session_update", func(m wsMsg) bool { return m.session().Visibility == "hub_users" })

	// invite_only with anna: bob's device loses sight (session_ended no_longer_visible), anna's gets session_invite.
	wb.settle()
	wc.settle()
	wantStatus(t, s.patch(a1, sess.SessionID, "invite_only"), 200, "")
	if m := wc.await("session_ended"); m.field("reason") != "no_longer_visible" || m.field("session_id") != sess.SessionID {
		t.Fatalf("%s", m.Payload)
	}
	wb.await("session_ended")
	wb.settle()
	wantStatus(t, s.invite(a1, sess.SessionID, s.anna.ID), 200, "")
	if got := wb.await("session_invite").session(); !got.Invited || got.SessionID != sess.SessionID || got.Viewers != nil {
		t.Fatalf("invite: %+v", got)
	}
	wc.quiet("session_invite", "session_update")

	// Presence goes to every connected device.
	w1.settle()
	wb.send("presence_update", map[string]any{"state": "in_game", "game_id": s.game.ID})
	p := w1.awaitWhere("presence_update", func(m wsMsg) bool { return m.field("device_id") == b1.id })
	if p.field("state") != "in_game" || p.field("game_id") != s.game.ID || p.field("user_id") != s.anna.ID {
		t.Fatalf("%s", p.Payload)
	}
	wb.sendRaw([]byte(`{"type":"presence_update","payload":{"state":"in_game","game_id":"not-a-uuid"}}`))
	wb.wantError("bad_request")
}

func TestRevocationOnVisibilityChangeWithdrawRemoveAndEnd(t *testing.T) {
	s := newSessEnv(t, nil)
	a1 := s.device(s.admin.ID, "Desktop")
	b1, c1 := s.device(s.anna.ID, "Anna-Laptop"), s.device(s.bob.ID, "Bob-PC")
	w1, wb, wc := s.dial(a1), s.dial(b1), s.dial(c1)

	sess := s.publish(a1, "hub_users")
	sid := sess.SessionID
	vb, vc := s.mustJoin(b1, sid), s.mustJoin(c1, sid)
	if m := w1.awaitWhere("viewer_joined", func(m wsMsg) bool { return m.field("viewer_id") == vb }); m.field("display_name") != "Anna" ||
		m.field("device_name") != "Anna-Laptop" || m.field("session_id") != sid {
		t.Fatalf("%s", m.Payload)
	}
	w1.awaitWhere("viewer_joined", func(m wsMsg) bool { return m.field("viewer_id") == vc })
	for _, w := range []*wsClient{w1, wb, wc} {
		w.settle()
	}

	// 1. Visibility change to private: both viewers revoked, both sides told.
	wantStatus(t, s.patch(a1, sid, "private"), 200, "")
	for vid, w := range map[string]*wsClient{vb: wb, vc: wc} {
		if m := w.awaitWhere("viewer_left", nil); m.field("viewer_id") != vid || m.field("reason") != "revoked" {
			t.Fatalf("viewer %s: %s", vid, m.Payload)
		}
		w.await("session_ended") // the Session is no longer visible to them
	}
	left := map[string]bool{}
	for i := 0; i < 2; i++ {
		m := w1.await("viewer_left")
		left[m.field("viewer_id")] = m.field("reason") == "revoked"
	}
	if !left[vb] || !left[vc] {
		t.Fatalf("owner was not told about both revocations: %v", left)
	}
	if o := decode[sessResp](t, s.get(a1, sid)); o.ViewerCount != 0 {
		t.Fatalf("%+v", o)
	}

	// 2. Withdrawing an invite removes that user's viewers.
	wantStatus(t, s.patch(a1, sid, "invite_only"), 200, "")
	wantStatus(t, s.invite(a1, sid, s.anna.ID), 200, "")
	vb = s.mustJoin(b1, sid)
	for _, w := range []*wsClient{w1, wb, wc} {
		w.settle()
	}
	wantStatus(t, s.api(a1, "DELETE", "/api/v1/sessions/"+sid+"/invites/"+s.anna.ID, nil), 204, "")
	if m := wb.await("viewer_left"); m.field("viewer_id") != vb || m.field("reason") != "revoked" {
		t.Fatalf("%s", m.Payload)
	}
	if m := w1.await("viewer_left"); m.field("viewer_id") != vb || m.field("reason") != "revoked" {
		t.Fatalf("%s", m.Payload)
	}
	wantStatus(t, s.api(a1, "DELETE", "/api/v1/sessions/"+sid+"/invites/"+s.anna.ID, nil), 404, "not_found")
	wantStatus(t, s.join(b1, sid), 403, "session_forbidden")

	// 3. Owner removes a viewer ("removed"); a viewer leaves ("left").
	wantStatus(t, s.patch(a1, sid, "hub_users"), 200, "")
	vb, vc = s.mustJoin(b1, sid), s.mustJoin(c1, sid)
	for _, w := range []*wsClient{w1, wb, wc} {
		w.settle()
	}
	wantStatus(t, s.api(c1, "DELETE", "/api/v1/sessions/"+sid+"/viewers/"+vb, nil), 403, "session_forbidden") // not his viewer
	wantStatus(t, s.api(a1, "DELETE", "/api/v1/sessions/"+sid+"/viewers/"+uuid.NewString(), nil), 404, "not_found")
	wantStatus(t, s.api(a1, "DELETE", "/api/v1/sessions/"+sid+"/viewers/"+vb, nil), 204, "")
	if m := wb.await("viewer_left"); m.field("viewer_id") != vb || m.field("reason") != "removed" {
		t.Fatalf("%s", m.Payload)
	}
	if m := w1.await("viewer_left"); m.field("viewer_id") != vb || m.field("reason") != "removed" {
		t.Fatalf("%s", m.Payload)
	}
	wantStatus(t, s.api(c1, "DELETE", "/api/v1/sessions/"+sid+"/viewers/"+vc, nil), 204, "")
	if m := w1.awaitWhere("viewer_left", func(m wsMsg) bool { return m.field("viewer_id") == vc }); m.field("reason") != "left" {
		t.Fatalf("%s", m.Payload)
	}
	wc.awaitWhere("viewer_left", nil)

	// 4. Session end: both sides get session_ended, relay stops.
	vb = s.mustJoin(b1, sid)
	for _, w := range []*wsClient{w1, wb, wc} {
		w.settle()
	}
	wantStatus(t, s.api(a1, "DELETE", "/api/v1/sessions/"+sid, nil), 204, "")
	for _, w := range []*wsClient{w1, wb, wc} {
		if m := w.await("session_ended"); m.field("session_id") != sid || m.field("reason") != "ended" {
			t.Fatalf("%s", m.Payload)
		}
	}
	wb.send("signal", map[string]any{"session_id": sid, "viewer_id": vb, "kind": "answer", "sdp": "x"})
	wb.wantError("session_forbidden")
}

func TestSignalRelayOnlyBetweenOwnerAndAuthorizedViewer(t *testing.T) {
	s := newSessEnv(t, nil)
	a1, a2, a3 := s.device(s.admin.ID, "Desktop"), s.device(s.admin.ID, "Laptop"), s.device(s.admin.ID, "Tablet")
	b1 := s.device(s.anna.ID, "Anna-Laptop")
	w1, w2, w3, wb := s.dial(a1), s.dial(a2), s.dial(a3), s.dial(b1)

	sess := s.publish(a1, "private")
	sid := sess.SessionID
	v2 := s.mustJoin(a2, sid)
	for _, w := range []*wsClient{w1, w2, w3, wb} {
		w.settle()
	}

	// Owner -> viewer and back, payload relayed unchanged (SDP never parsed).
	sdp := "v=0\r\no=- 1 1 IN IP4 192.0.2.1\r\n(placeholder offer ä)"
	w1.send("signal", map[string]any{"session_id": sid, "viewer_id": v2, "kind": "offer", "sdp": sdp})
	m := w2.await("signal")
	if m.field("sdp") != sdp || m.field("kind") != "offer" || m.field("viewer_id") != v2 || m.field("session_id") != sid {
		t.Fatalf("%s", m.Payload)
	}
	w2.send("signal", map[string]any{"session_id": sid, "viewer_id": v2, "kind": "answer", "sdp": "answer-sdp"})
	if m := w1.await("signal"); m.field("sdp") != "answer-sdp" || m.field("kind") != "answer" {
		t.Fatalf("%s", m.Payload)
	}
	w2.send("signal", map[string]any{"session_id": sid, "viewer_id": v2, "kind": "candidate", "candidate": "candidate:1 1 UDP 1 192.0.2.2 1 typ host", "mid": "0"})
	if m := w1.await("signal"); m.field("candidate") == "" || m.field("mid") != "0" {
		t.Fatalf("%s", m.Payload)
	}

	// Forbidden: a device that is neither the owner nor that viewer; a foreign user; wrong ids; unknown session.
	w3.send("signal", map[string]any{"session_id": sid, "viewer_id": v2, "kind": "offer", "sdp": "x"})
	w3.wantError("session_forbidden")
	wb.send("signal", map[string]any{"session_id": sid, "viewer_id": v2, "kind": "answer", "sdp": "x"})
	wb.wantError("session_forbidden")
	w2.send("signal", map[string]any{"session_id": sid, "viewer_id": uuid.NewString(), "kind": "answer", "sdp": "x"})
	w2.wantError("session_forbidden")
	w2.send("signal", map[string]any{"session_id": uuid.NewString(), "viewer_id": v2, "kind": "answer", "sdp": "x"})
	w2.wantError("session_forbidden")
	// Nothing leaked to the legitimate parties.
	w1.quiet("signal")
	w2.quiet("signal")
	// Malformed signal.
	w2.sendRaw([]byte(`{"type":"signal","payload":{"session_id":"` + sid + `","viewer_id":"` + v2 + `","kind":"hack"}}`))
	w2.wantError("bad_request")

	// After removal the relay stops in both directions.
	wantStatus(t, s.api(a1, "DELETE", "/api/v1/sessions/"+sid+"/viewers/"+v2, nil), 204, "")
	w1.settle()
	w1.send("signal", map[string]any{"session_id": sid, "viewer_id": v2, "kind": "offer", "sdp": "x"})
	w1.wantError("session_forbidden")
	w2.send("signal", map[string]any{"session_id": sid, "viewer_id": v2, "kind": "answer", "sdp": "x"})
	w2.wantError("session_forbidden")
}

func TestOwnerGraceAndViewerRemovalOnDrop(t *testing.T) {
	grace := 600 * time.Millisecond
	s := newSessEnv(t, func(o *hub.Options) { o.OwnerGrace = grace })
	a1, a2 := s.device(s.admin.ID, "Desktop"), s.device(s.admin.ID, "Laptop")
	w1, w2 := s.dial(a1), s.dial(a2)
	sess := s.publish(a1, "private")
	sid := sess.SessionID
	v2 := s.mustJoin(a2, sid)
	w1.settle()
	w2.settle()

	// Viewer WSS drops: viewer removed at once ("disconnected"), owner told.
	w2.conn.CloseNow()
	if m := w1.await("viewer_left"); m.field("viewer_id") != v2 || m.field("reason") != "disconnected" {
		t.Fatalf("%s", m.Payload)
	}
	if o := decode[sessResp](t, s.get(a1, sid)); o.ViewerCount != 0 {
		t.Fatalf("%+v", o)
	}
	// Presence offline is broadcast.
	w3 := s.dial(s.device(s.admin.ID, "Observer"))
	w3.settle()

	// Owner drops and reconnects inside the grace period: Session survives.
	w1.conn.CloseNow()
	time.Sleep(grace / 3)
	w1b := s.dial(a1)
	time.Sleep(grace * 3 / 2)
	wantStatus(t, s.get(a1, sid), 200, "")
	w1b.await("session_update") // snapshot after the reconnect

	// Owner drops for good: after the grace period the Session ends, viewers learn it.
	v := s.mustJoin(a2, sid)
	w2b := s.dial(a2)
	w2b.settle()
	w1b.conn.CloseNow()
	w2b.quiet("session_ended") // still inside the grace period
	if m := w2b.awaitWhere("session_ended", nil); m.field("session_id") != sid || m.field("reason") != "owner_disconnected" {
		t.Fatalf("%s", m.Payload)
	}
	wantStatus(t, s.get(a2, sid), 410, "session_ended")
	_ = v

	// A Session published without any WSS connection also ends after the grace period.
	s2 := s.publish(s.device(s.admin.ID, "Headless"), "hub_users")
	wantStatus(t, s.get(a2, s2.SessionID), 200, "")
	deadline := time.Now().Add(3 * time.Second)
	for s.get(a2, s2.SessionID).Code != 410 {
		if time.Now().After(deadline) {
			t.Fatal("orphan Session was not ended")
		}
		time.Sleep(50 * time.Millisecond)
	}
	// A viewer that joined by REST but never connected is removed after the grace period.
	a4 := s.device(s.admin.ID, "Silent")
	s3 := s.publish(a1, "private")
	w1c := s.dial(a1)
	w1c.settle()
	vs := s.mustJoin(a4, s3.SessionID)
	if m := w1c.awaitWhere("viewer_left", func(m wsMsg) bool { return m.field("viewer_id") == vs }); m.field("reason") != "disconnected" {
		t.Fatalf("%s", m.Payload)
	}
}

func TestSessionFullAndSlotFreed(t *testing.T) {
	s := newSessEnv(t, nil)
	a1 := s.device(s.admin.ID, "Desktop")
	sess := s.publish(a1, "hub_users")
	var viewers []dev
	var ids []string
	for i := 0; i < hub.MaxSessionViewers; i++ {
		d := s.device(s.anna.ID, fmt.Sprintf("Viewer-%d", i))
		viewers = append(viewers, d)
		ids = append(ids, s.mustJoin(d, sess.SessionID))
	}
	extra := s.device(s.bob.ID, "Late")
	wantStatus(t, s.join(extra, sess.SessionID), 409, "session_full")
	// Joining again with a device that is already a viewer returns its viewer (no slot needed).
	if again := decode[joinResp](t, s.join(viewers[0], sess.SessionID)); again.ViewerID != ids[0] {
		t.Fatalf("rejoin: %s, want %s", again.ViewerID, ids[0])
	}
	wantStatus(t, s.api(viewers[1], "DELETE", "/api/v1/sessions/"+sess.SessionID+"/viewers/"+ids[1], nil), 204, "")
	s.mustJoin(extra, sess.SessionID)
	if o := decode[sessResp](t, s.get(a1, sess.SessionID)); o.ViewerCount != 4 {
		t.Fatalf("%+v", o)
	}
}

func TestDeclineFlow(t *testing.T) {
	s := newSessEnv(t, nil)
	a1 := s.device(s.admin.ID, "Desktop")
	b1, b2 := s.device(s.anna.ID, "Anna-Laptop"), s.device(s.anna.ID, "Anna-Phone")
	w1, wb1, wb2 := s.dial(a1), s.dial(b1), s.dial(b2)
	sess := s.publish(a1, "invite_only")
	sid := sess.SessionID
	wantStatus(t, s.invite(a1, sid, s.anna.ID), 200, "")
	if got := wb1.await("session_invite").session(); got.SessionID != sid || !got.Invited {
		t.Fatalf("%+v", got)
	}
	wb2.await("session_invite")
	if o := decode[sessResp](t, s.get(a1, sid)); (*o.Invites)[0].State != "invited" || !(*o.Invites)[0].Online {
		t.Fatalf("%+v", o.Invites)
	}
	w1.settle()
	wb1.settle()
	wb2.settle()

	// Only an invited user may decline.
	wantStatus(t, s.api(s.device(s.bob.ID, "Bob-PC"), "POST", "/api/v1/sessions/"+sid+"/decline", nil), 403, "session_forbidden")
	wantStatus(t, s.api(b1, "POST", "/api/v1/sessions/"+sid+"/decline", nil), 204, "")
	// The owner learns it via session_update; all of the decliner's devices drop the Session.
	got := w1.awaitWhere("session_update", func(m wsMsg) bool {
		inv := m.session().Invites
		return inv != nil && len(*inv) == 1 && (*inv)[0].State == "declined"
	}).session()
	if !got.IsOwner {
		t.Fatalf("%+v", got)
	}
	for _, w := range []*wsClient{wb1, wb2} {
		if m := w.await("session_ended"); m.field("reason") != "no_longer_visible" {
			t.Fatalf("%s", m.Payload)
		}
	}
	if len(s.list(b1)) != 0 {
		t.Fatal("declined invite is still listed")
	}
	wantStatus(t, s.join(b2, sid), 403, "session_forbidden")
	// Re-inviting reopens it.
	wantStatus(t, s.invite(a1, sid, s.anna.ID), 200, "")
	wb2.await("session_invite")
	s.mustJoin(b2, sid)
}

func TestCodecCapabilityRejection(t *testing.T) {
	s := newSessEnv(t, nil)
	a1, a2 := s.device(s.admin.ID, "NoEncode"), s.device(s.admin.ID, "NoDecode")
	other := s.device(s.admin.ID, "Full")
	handshake := func(d dev, enc, dec bool) {
		body := handshakeBody(1, 1)
		body["video"] = map[string]any{"h264_encode": enc, "h264_decode": dec, "encoders": []string{}}
		wantStatus(t, s.api(d, "POST", "/api/v1/handshake", body), 200, "")
	}
	handshake(a1, false, true)
	handshake(a2, true, false)
	handshake(other, true, true)

	wantStatus(t, s.api(a1, "POST", "/api/v1/sessions", map[string]any{"game_id": s.game.ID, "visibility": "hub_users"}), 409, "capability_missing")
	sess := s.publish(a2, "hub_users") // can encode, may publish
	wantStatus(t, s.join(a2, sess.SessionID), 400, "bad_request")
	owner := s.publish(other, "hub_users")
	wantStatus(t, s.join(a2, owner.SessionID), 409, "capability_missing")
	s.mustJoin(a1, owner.SessionID) // can decode, may join
	if o := decode[sessResp](t, s.get(other, owner.SessionID)); o.ViewerCount != 1 {
		t.Fatalf("a rejected join must not create a viewer: %+v", o)
	}
	// A later handshake that reports the capability lifts the restriction.
	handshake(a2, true, true)
	s.mustJoin(a2, owner.SessionID)
	// Devices that never reported capabilities are not blocked.
	s.publish(s.device(s.admin.ID, "Unknown"), "private")
}

func TestPublishEndsPreviousSession(t *testing.T) {
	s := newSessEnv(t, nil)
	a1, a2 := s.device(s.admin.ID, "Desktop"), s.device(s.admin.ID, "Laptop")
	w1, w2 := s.dial(a1), s.dial(a2)
	first := s.publish(a1, "private")
	v := s.mustJoin(a2, first.SessionID)
	w1.settle()
	w2.settle()
	second := s.publish(a1, "private")
	if second.SessionID == first.SessionID {
		t.Fatal("same Session ID")
	}
	for _, w := range []*wsClient{w1, w2} {
		if m := w.awaitWhere("session_ended", isSession(first.SessionID)); m.field("reason") != "replaced" {
			t.Fatalf("%s", m.Payload)
		}
	}
	wantStatus(t, s.get(a2, first.SessionID), 410, "session_ended")
	if l := s.list(a2); len(l) != 1 || l[0].SessionID != second.SessionID || l[0].ViewerCount != 0 {
		t.Fatalf("%+v", l)
	}
	// The old viewer is gone; the relay of the old Session is closed.
	w1.send("signal", map[string]any{"session_id": first.SessionID, "viewer_id": v, "kind": "offer", "sdp": "x"})
	w1.wantError("session_forbidden")
	wantStatus(t, s.api(a1, "POST", "/api/v1/sessions", map[string]any{"game_id": uuid.NewString(), "visibility": "private"}), 404, "not_found")
}

func TestRevokedDeviceWSClosedAndSessionsEnd(t *testing.T) {
	s := newSessEnv(t, nil)
	a1, a2, a3 := s.device(s.admin.ID, "Desktop"), s.device(s.admin.ID, "Laptop"), s.device(s.admin.ID, "Tablet")
	w1, w2, w3 := s.dial(a1), s.dial(a2), s.dial(a3)
	sess := s.publish(a1, "hub_users")
	s.mustJoin(a2, sess.SessionID)
	v3 := s.mustJoin(a3, sess.SessionID)
	for _, w := range []*wsClient{w1, w2, w3} {
		w.settle()
	}

	// A viewer device is revoked: its WSS is closed, the owner learns that the viewer is gone.
	if err := s.svc.RevokeDevice(context.Background(), a3.id); err != nil {
		t.Fatal(err)
	}
	if code := w3.wantClosed(); code != websocket.StatusPolicyViolation {
		t.Fatalf("close status %d", code)
	}
	if m := w1.awaitWhere("viewer_left", func(m wsMsg) bool { return m.field("viewer_id") == v3 }); m.field("reason") != "revoked" {
		t.Fatalf("%s", m.Payload)
	}
	// The revoked token no longer opens a connection.
	if _, resp, err := websocket.Dial(context.Background(), s.wsURL(), &websocket.DialOptions{HTTPHeader: http.Header{"Authorization": {"Bearer " + a3.tok}}}); err == nil || resp == nil || resp.StatusCode != 401 {
		t.Fatalf("dial with a revoked token: %v %v", resp, err)
	}

	// The owner device is revoked: its Session ends for the remaining viewer.
	if err := s.svc.RevokeDevice(context.Background(), a1.id); err != nil {
		t.Fatal(err)
	}
	w1.wantClosed()
	if m := w2.await("session_ended"); m.field("session_id") != sess.SessionID || m.field("reason") != "device_revoked" {
		t.Fatalf("%s", m.Payload)
	}
	wantStatus(t, s.get(a2, sess.SessionID), 410, "session_ended")
	// Self revoke over REST closes the connection as well.
	wantStatus(t, s.api(a2, "POST", "/api/v1/auth/revoke", nil), 204, "")
	w2.wantClosed()
}

func TestWSHandshakeRules(t *testing.T) {
	s := newSessEnv(t, func(o *hub.Options) { o.ICEServers = []string{"stun:stun.example.org:3478"} })
	a1 := s.device(s.admin.ID, "Desktop")

	// No token: refused before the upgrade.
	if _, resp, err := websocket.Dial(context.Background(), s.wsURL(), nil); err == nil || resp == nil || resp.StatusCode != 401 {
		t.Fatalf("anonymous dial: %v %v", resp, err)
	}
	// hello_ack: version, features, ice_servers (also in the join response).
	c := s.dialRaw(a1)
	c.send("hello", map[string]any{"protocol_version": 1, "device_id": a1.id})
	ack := c.await("hello_ack")
	var p struct {
		ProtocolVersion int      `json:"protocol_version"`
		HubVersion      string   `json:"hub_version"`
		Features        []string `json:"features"`
		IceServers      []string `json:"ice_servers"`
	}
	json.Unmarshal(ack.Payload, &p)
	if p.ProtocolVersion != hub.ProtocolVersion || p.HubVersion == "" || len(p.IceServers) != 1 || p.IceServers[0] != "stun:stun.example.org:3478" ||
		strings.Join(p.Features, ",") != "saves_v1,saves_v2,sessions_v1,ice_update_v1,runs_v1,runs_v2,local_roms_v1,together_v1" || ack.ID != "x" {
		t.Fatalf("%+v id=%q", p, ack.ID)
	}
	sess := s.publish(a1, "hub_users")
	j := decode[joinResp](t, s.join(s.device(s.anna.ID, "Anna-Laptop"), sess.SessionID))
	if len(j.IceServers) != 1 || j.IceServers[0] != "stun:stun.example.org:3478" {
		t.Fatalf("%+v", j)
	}
	// Second hello and unknown types are rejected but keep the connection.
	c.send("hello", map[string]any{"protocol_version": 1, "device_id": a1.id})
	c.wantError("bad_request")
	c.send("bogus", map[string]any{})
	c.wantError("bad_request")

	// First message must be hello.
	c2 := s.dialRaw(a1)
	c2.send("presence_update", map[string]any{"state": "online"})
	c2.wantError("bad_request")
	if code := c2.wantClosed(); code != websocket.StatusPolicyViolation {
		t.Fatalf("close status %d", code)
	}
	// device_id must match the token; protocol too old is refused.
	c3 := s.dialRaw(a1)
	c3.send("hello", map[string]any{"protocol_version": 1, "device_id": uuid.NewString()})
	c3.wantError("forbidden")
	c3.wantClosed()
	// Hello timeout.
	old := wsHelloTimeout
	wsHelloTimeout = 200 * time.Millisecond
	defer func() { wsHelloTimeout = old }()
	c4 := s.dialRaw(a1)
	if code := c4.wantClosed(); code != websocket.StatusPolicyViolation {
		t.Fatalf("close status %d", code)
	}
}

func TestNewerConnectionReplacesOlderOne(t *testing.T) {
	s := newSessEnv(t, nil)
	a1, a2 := s.device(s.admin.ID, "Desktop"), s.device(s.admin.ID, "Laptop")
	old := s.dial(a1)
	sess := s.publish(a1, "private")
	v := s.mustJoin(a2, sess.SessionID)
	fresh := s.dial(a1)
	old.wantClosed()
	// Closing the old connection must not end the Session or remove viewers.
	time.Sleep(200 * time.Millisecond)
	if o := decode[sessResp](t, s.get(a1, sess.SessionID)); o.ViewerCount != 1 {
		t.Fatalf("%+v", o)
	}
	_ = v
	fresh.await("session_update") // snapshot after hello
}

func TestPingKeepaliveAndUsersList(t *testing.T) {
	old := wsPingInterval
	wsPingInterval = 50 * time.Millisecond
	defer func() { wsPingInterval = old }()
	s := newSessEnv(t, nil)
	a1 := s.device(s.admin.ID, "Desktop")
	b1 := s.device(s.anna.ID, "Anna-Laptop")

	users := func() map[string]bool {
		rec := s.api(a1, "GET", "/api/v1/users", nil)
		wantStatus(t, rec, 200, "")
		out := map[string]bool{}
		for _, u := range decode[struct {
			Users []struct {
				ID          string `json:"id"`
				DisplayName string `json:"display_name"`
				Online      bool   `json:"online"`
			} `json:"users"`
		}](t, rec).Users {
			out[u.ID] = u.Online
		}
		return out
	}
	if u := users(); len(u) != 3 || u[s.anna.ID] || u[s.admin.ID] {
		t.Fatalf("%v", u)
	}
	wa := s.dial(a1)
	wb := s.dial(b1)
	time.Sleep(300 * time.Millisecond) // several pings answered by the client library: the connection stays up
	if u := users(); !u[s.anna.ID] || !u[s.admin.ID] || u[s.bob.ID] {
		t.Fatalf("%v", u)
	}
	// presence offline when a device drops
	wa.settle()
	wb.conn.CloseNow()
	if m := wa.awaitWhere("presence_update", func(m wsMsg) bool { return m.field("state") == "offline" }); m.field("device_id") != b1.id {
		t.Fatalf("%s", m.Payload)
	}
	deadline := time.Now().Add(2 * time.Second)
	for users()[s.anna.ID] {
		if time.Now().After(deadline) {
			t.Fatal("user still online")
		}
		time.Sleep(20 * time.Millisecond)
	}
	wantStatus(t, s.do("GET", "/api/v1/users", nil, opt{}), 401, "unauthorized")
}

// The access token only authenticates the upgrade: a disabled user's open connection is closed on a ping tick.
func TestWSClosedWhenUserDisabled(t *testing.T) {
	old := wsPingInterval
	wsPingInterval = 50 * time.Millisecond
	defer func() { wsPingInterval = old }()
	s := newSessEnv(t, nil)
	b1 := s.device(s.anna.ID, "Anna-Laptop")
	w := s.dial(b1)
	time.Sleep(150 * time.Millisecond)
	if err := s.svc.DisableUser(context.Background(), s.anna.ID); err != nil {
		t.Fatal(err)
	}
	w.wantClosed()
}

// At most two connections per device may be open without having completed hello.
func TestWSPreHelloConnectionsLimited(t *testing.T) {
	s := newSessEnv(t, nil)
	a1 := s.device(s.admin.ID, "Desktop")
	s.dialRaw(a1)
	s.dialRaw(a1)
	_, resp, err := websocket.Dial(context.Background(), s.wsURL(), &websocket.DialOptions{HTTPHeader: http.Header{"Authorization": {"Bearer " + a1.tok}}})
	if err == nil || resp == nil || resp.StatusCode != 429 {
		t.Fatalf("third pre-hello connection: %v %v", resp, err)
	}
}
