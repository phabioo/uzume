package httpapi

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/google/uuid"

	"github.com/phabioo/uzume/server/internal/hub"
)

// Runs over REST and WSS (0.11 package 3): contract sections 5 and 6.

const (
	pluginA = "net.example.board"
	pluginB = "net.example.notes"
	pluginC = "net.example.unused"
)

type fakeCatalog struct{ offered map[string]string }

func (f *fakeCatalog) OfferedPlugin(_ context.Context, id, version string) (string, bool, error) {
	sha, ok := f.offered[id+"@"+version]
	return sha, ok, nil
}

type runEnv struct {
	*sessEnv
	carol, dave, erin, frank, gina, hank hub.User
}

func newRunEnv(t *testing.T) *runEnv {
	t.Helper()
	s := newSessEnv(t, nil)
	s.svc.SetPluginCatalog(&fakeCatalog{offered: map[string]string{
		pluginA + "@1.0.0": strings.Repeat("a", 64),
		pluginB + "@2.1.0": strings.Repeat("b", 64),
	}})
	r := &runEnv{sessEnv: s}
	for name, dst := range map[string]*hub.User{"carol": &r.carol, "dave": &r.dave, "erin": &r.erin, "frank": &r.frank, "gina": &r.gina, "hank": &r.hank} {
		u, err := s.svc.CreateUser(context.Background(), name, strings.ToUpper(name[:1])+name[1:])
		if err != nil {
			t.Fatal(err)
		}
		*dst = u
	}
	return r
}

type partResp struct {
	UserID   string  `json:"user_id"`
	UserName string  `json:"user_name"`
	Status   string  `json:"status"`
	GameID   *string `json:"game_id"`
	Color    *int    `json:"color"`
	States   map[string]struct {
		State     json.RawMessage `json:"state"`
		StateRev  int64           `json:"state_rev"`
		UpdatedAt string          `json:"updated_at"`
	} `json:"states"`
}

type runResp struct {
	RunID     string          `json:"run_id"`
	Name      string          `json:"name"`
	Status    string          `json:"status"`
	CreatedBy string          `json:"created_by"`
	StatusBy  string          `json:"status_by"`
	Settings  json.RawMessage `json:"settings"`
	Plugins   []struct {
		PluginID string `json:"plugin_id"`
		Version  string `json:"version"`
		SHA256   string `json:"sha256"`
	} `json:"plugins"`
	Participants []partResp `json:"participants"`
	LastSeq      int64      `json:"last_seq"`
}

func (r runResp) part(userID string) partResp {
	for _, p := range r.Participants {
		if p.UserID == userID {
			return p
		}
	}
	return partResp{}
}

type eventResp struct {
	Seq      int64           `json:"seq"`
	UserID   *string         `json:"user_id"`
	UserName string          `json:"user_name"`
	DeviceID *string         `json:"device_id"`
	PluginID *string         `json:"plugin_id"`
	Source   string          `json:"source"`
	Type     string          `json:"type"`
	Text     string          `json:"text"`
	Payload  json.RawMessage `json:"payload"`
	ClientID string          `json:"client_id"`
}

func (r *runEnv) createBody(name string, invite ...string) map[string]any {
	if invite == nil {
		invite = []string{}
	}
	return map[string]any{"name": name, "plugins": []any{map[string]any{"plugin_id": pluginA, "version": "1.0.0"}},
		"game_id": r.game.ID, "invite_user_ids": invite}
}

func (r *runEnv) create(d dev, name string, invite ...string) runResp {
	r.t.Helper()
	rec := r.api(d, "POST", "/api/v1/runs", r.createBody(name, invite...))
	wantStatus(r.t, rec, 201, "")
	return decode[runResp](r.t, rec)
}

func (r *runEnv) get(d dev, id string) runResp {
	r.t.Helper()
	rec := r.api(d, "GET", "/api/v1/runs/"+id, nil)
	wantStatus(r.t, rec, 200, "")
	return decode[runResp](r.t, rec)
}

func (r *runEnv) post(d dev, id, rest string, body any) *httptest.ResponseRecorder {
	r.t.Helper()
	return r.api(d, "POST", "/api/v1/runs/"+id+"/"+rest, body)
}

func (r *runEnv) join(d dev, id string) *httptest.ResponseRecorder {
	r.t.Helper()
	return r.post(d, id, "join", map[string]any{"game_id": r.game.ID})
}

func (r *runEnv) mustJoin(d dev, id string) runResp {
	r.t.Helper()
	rec := r.join(d, id)
	wantStatus(r.t, rec, 200, "")
	return decode[runResp](r.t, rec)
}

func (r *runEnv) status(d dev, id, status string) *httptest.ResponseRecorder {
	r.t.Helper()
	return r.post(d, id, "status", map[string]any{"status": status})
}

func (r *runEnv) eventBody(clientID, typ string) map[string]any {
	return map[string]any{"client_id": clientID, "plugin_id": pluginA, "source": "manual", "type": typ, "text": "text " + typ, "payload": map[string]any{"n": 1}}
}

func (r *runEnv) event(d dev, id, typ string) *httptest.ResponseRecorder {
	r.t.Helper()
	return r.post(d, id, "events", r.eventBody(uuid.NewString(), typ))
}

func (r *runEnv) events(d dev, id string, after int64) []eventResp {
	r.t.Helper()
	rec := r.api(d, "GET", fmt.Sprintf("/api/v1/runs/%s/events?after=%d&limit=500", id, after), nil)
	wantStatus(r.t, rec, 200, "")
	return decode[struct{ Events []eventResp }](r.t, rec).Events
}

func (r *runEnv) putState(d dev, id string, base int64, state any) *httptest.ResponseRecorder {
	r.t.Helper()
	return r.putStateFor(d, id, pluginA, base, state)
}

func (r *runEnv) putStateFor(d dev, id, plugin string, base int64, state any) *httptest.ResponseRecorder {
	r.t.Helper()
	return r.api(d, "PUT", "/api/v1/runs/"+id+"/state/"+plugin, map[string]any{"base_rev": base, "state": state})
}

func (r *runEnv) putBoard(d dev, id, plugin string, tree any) *httptest.ResponseRecorder {
	r.t.Helper()
	return r.api(d, "PUT", "/api/v1/runs/"+id+"/boards/"+plugin, map[string]any{"tree": tree})
}

// ---- REST ----

func TestRunCreateAndVisibility(t *testing.T) {
	r := newRunEnv(t)
	d1, a1, b1 := r.device(r.admin.ID, "Desktop"), r.device(r.anna.ID, "Anna PC"), r.device(r.bob.ID, "Bob PC")

	run := r.create(d1, "  Summer Duo  ", r.anna.ID, r.anna.ID, r.admin.ID) // duplicates and the creator are dropped
	if run.Name != "Summer Duo" || run.Status != "starting" || run.CreatedBy != r.admin.ID || run.StatusBy != r.admin.ID || run.LastSeq != 2 {
		t.Fatalf("%+v", run)
	}
	if len(run.Plugins) != 1 || run.Plugins[0].PluginID != pluginA || run.Plugins[0].Version != "1.0.0" || run.Plugins[0].SHA256 != strings.Repeat("a", 64) {
		t.Fatalf("plugins %+v", run.Plugins)
	}
	if string(run.Settings) != "{}" || len(run.Participants) != 2 {
		t.Fatalf("%s %+v", run.Settings, run.Participants)
	}
	c := run.part(r.admin.ID)
	if c.Status != "joined" || c.Color == nil || *c.Color != 0 || c.GameID == nil || *c.GameID != r.game.ID || len(c.States) != 0 {
		t.Fatalf("creator %+v", c)
	}
	if an := run.part(r.anna.ID); an.Status != "invited" || an.Color != nil || an.GameID != nil || an.UserName != "Anna" {
		t.Fatalf("anna %+v", an)
	}

	// The server logged the creation: run.created and run.joined, as server events.
	evs := r.events(d1, run.RunID, 0)
	if len(evs) != 2 || evs[0].Seq != 1 || evs[0].Type != "run.created" || evs[1].Seq != 2 || evs[1].Type != "run.joined" {
		t.Fatalf("%+v", evs)
	}
	for _, e := range evs {
		if e.Source != "hub" || e.UserID != nil || e.DeviceID != nil || e.PluginID != nil || uuid.Validate(e.ClientID) != nil {
			t.Fatalf("hub event %+v", e)
		}
	}

	// Participants of any status see the run; everybody else gets 404 on every endpoint.
	r.get(a1, run.RunID)
	wantStatus(t, r.api(b1, "GET", "/api/v1/runs/"+run.RunID, nil), 404, "not_found")
	wantStatus(t, r.api(b1, "GET", "/api/v1/runs/"+run.RunID+"/events", nil), 404, "not_found")
	wantStatus(t, r.putState(b1, run.RunID, 0, map[string]any{}), 404, "not_found")
	wantStatus(t, r.putBoard(b1, run.RunID, pluginA, map[string]any{"type": "column"}), 404, "not_found")
	wantStatus(t, r.event(b1, run.RunID, "x"), 404, "not_found")
	wantStatus(t, r.join(b1, run.RunID), 404, "not_found")
	wantStatus(t, r.post(b1, run.RunID, "decline", nil), 404, "not_found")
	wantStatus(t, r.post(b1, run.RunID, "leave", nil), 404, "not_found")
	wantStatus(t, r.status(b1, run.RunID, "paused"), 404, "not_found")
	wantStatus(t, r.api(b1, "GET", "/api/v1/runs/"+uuid.NewString(), nil), 404, "not_found")
	wantStatus(t, r.do("GET", "/api/v1/runs/not-a-uuid", nil, opt{token: b1.tok, raw: true}), 404, "not_found")
	wantStatus(t, r.do("GET", "/api/v1/runs", nil, opt{}), 401, "")

	list := func(d dev) []runResp {
		rec := r.api(d, "GET", "/api/v1/runs", nil)
		wantStatus(t, rec, 200, "")
		return decode[struct{ Runs []runResp }](t, rec).Runs
	}
	if l := list(a1); len(l) != 1 || l[0].RunID != run.RunID {
		t.Fatalf("anna: %+v", l)
	}
	if l := list(b1); l == nil || len(l) != 0 {
		t.Fatalf("bob: %+v", l)
	}
}

func TestRunCreateValidation(t *testing.T) {
	r := newRunEnv(t)
	d1 := r.device(r.admin.ID, "Desktop")
	post := func(body map[string]any, raw bool) *httptest.ResponseRecorder {
		return r.do("POST", "/api/v1/runs", body, opt{token: d1.tok, raw: raw})
	}
	with := func(k string, v any) map[string]any { b := r.createBody("Run"); b[k] = v; return b }
	plugin := func(id, v string) map[string]any { return map[string]any{"plugin_id": id, "version": v} }

	wantStatus(t, post(with("plugins", []any{plugin(pluginA, "9.9.9")}), false), 400, "bad_request") // version not offered
	wantStatus(t, post(with("plugins", []any{plugin(pluginC, "1.0.0")}), false), 400, "bad_request") // plugin not offered
	wantStatus(t, post(with("plugins", []any{plugin(pluginA, "1.0.0"), plugin(pluginA, "1.0.0")}), false), 400, "bad_request")
	wantStatus(t, post(with("plugins", []any{}), true), 201, "")                      // runs_v2: a run without plugins is valid
	wantStatus(t, post(with("game_id", uuid.NewString()), false), 400, "bad_request") // unknown game
	wantStatus(t, post(with("invite_user_ids", []any{"u_nobody"}), false), 400, "bad_request")
	wantStatus(t, post(with("name", "   "), false), 400, "bad_request")
	wantStatus(t, post(with("name", strings.Repeat("n", 65)), true), 400, "bad_request")
	wantStatus(t, post(map[string]any{"name": "x"}, true), 400, "bad_request")
	wantStatus(t, post(with("settings", []any{1}), true), 400, "bad_request") // not an object

	// Settings are limited to 16 KiB (413) and stored byte-exact.
	wantStatus(t, post(with("settings", map[string]any{"pad": strings.Repeat("x", 17<<10)}), false), 413, "too_large")
	rec := r.do("POST", "/api/v1/runs", nil, opt{token: d1.tok, ctype: "application/json",
		data: []byte(`{"name":"Exact","plugins":[{"plugin_id":"` + pluginA + `","version":"1.0.0"}],"game_id":"` + r.game.ID + `","settings":{"big":12345678901234567890,"f":1.0}}`)})
	wantStatus(t, rec, 201, "")
	if got := string(decode[runResp](t, rec).Settings); got != `{"big":12345678901234567890,"f":1.0}` {
		t.Fatalf("settings %s", got)
	}

	// A run has at most 8 participants: the creator plus 7 invited users.
	seven := []string{r.anna.ID, r.bob.ID, r.carol.ID, r.dave.ID, r.erin.ID, r.frank.ID, r.gina.ID}
	run := r.create(d1, "Full", seven...)
	if len(run.Participants) != 8 {
		t.Fatalf("%d participants", len(run.Participants))
	}
	wantStatus(t, post(with("invite_user_ids", append(seven, r.hank.ID)), true), 400, "bad_request")
}

func TestRunMembership(t *testing.T) {
	r := newRunEnv(t)
	d1, a1, b1, c1 := r.device(r.admin.ID, "Desktop"), r.device(r.anna.ID, "Anna PC"), r.device(r.bob.ID, "Bob PC"), r.device(r.carol.ID, "Carol PC")
	run := r.create(d1, "Trio", r.anna.ID, r.bob.ID, r.carol.ID)

	// Colours are assigned in join order and survive leaving.
	bj := r.mustJoin(b1, run.RunID)
	if p := bj.part(r.bob.ID); p.Status != "joined" || p.Color == nil || *p.Color != 1 {
		t.Fatalf("bob %+v", p)
	}
	aj := r.mustJoin(a1, run.RunID)
	if p := aj.part(r.anna.ID); p.Color == nil || *p.Color != 2 {
		t.Fatalf("anna %+v", p)
	}
	// Joining again with the same game is a no-op without a new event; another game is a conflict.
	last := aj.LastSeq
	if again := r.mustJoin(a1, run.RunID); again.LastSeq != last {
		t.Fatalf("seq %d, want %d", again.LastSeq, last)
	}
	other, err := r.svc.AddROM(context.Background(), strings.NewReader("other-homebrew-dummy"), "other.nds", "Other", "", r.admin.ID)
	if err != nil {
		t.Fatal(err)
	}
	wantStatus(t, r.post(a1, run.RunID, "join", map[string]any{"game_id": other.ID}), 409, "conflict")
	wantStatus(t, r.post(a1, run.RunID, "join", map[string]any{"game_id": uuid.NewString()}), 400, "bad_request")
	wantStatus(t, r.post(a1, run.RunID, "decline", nil), 409, "conflict") // joined: leave instead

	left := decode[runResp](t, r.post(a1, run.RunID, "leave", nil))
	if p := left.part(r.anna.ID); p.Status != "left" || p.Color == nil || *p.Color != 2 {
		t.Fatalf("anna after leave %+v", p)
	}
	if again := decode[runResp](t, r.post(a1, run.RunID, "leave", nil)); again.LastSeq != left.LastSeq {
		t.Fatal("leaving twice must not log twice")
	}
	if p := r.mustJoin(a1, run.RunID).part(r.anna.ID); p.Color == nil || *p.Color != 2 || p.Status != "joined" {
		t.Fatalf("anna rejoined %+v", p)
	}
	wantStatus(t, r.post(c1, run.RunID, "leave", nil), 409, "conflict") // invited, never joined

	dec := decode[runResp](t, r.post(c1, run.RunID, "decline", nil))
	if p := dec.part(r.carol.ID); p.Status != "declined" || p.Color != nil {
		t.Fatalf("carol %+v", p)
	}
	if again := decode[runResp](t, r.post(c1, run.RunID, "decline", nil)); again.LastSeq != dec.LastSeq {
		t.Fatal("declining twice must not log twice")
	}
	// A declined invite can still be accepted; her colour comes after the existing ones.
	if p := r.mustJoin(c1, run.RunID).part(r.carol.ID); p.Color == nil || *p.Color != 3 {
		t.Fatalf("carol %+v", p)
	}

	// Server events: joins, the leave, the decline.
	var types []string
	for _, e := range r.events(d1, run.RunID, 0) {
		types = append(types, e.Type)
	}
	want := "run.created run.joined run.joined run.joined run.left run.joined run.declined run.joined"
	if strings.Join(types, " ") != want {
		t.Fatalf("events %v", types)
	}
	for _, e := range r.events(d1, run.RunID, 6) {
		if e.Type == "run.declined" {
			var p struct {
				UserID   string `json:"user_id"`
				UserName string `json:"user_name"`
			}
			json.Unmarshal(e.Payload, &p)
			if p.UserID != r.carol.ID || p.UserName != "Carol" {
				t.Fatalf("payload %s", e.Payload)
			}
		}
	}

	// A finished run accepts no membership changes.
	wantStatus(t, r.status(d1, run.RunID, "finished"), 200, "")
	wantStatus(t, r.post(a1, run.RunID, "leave", nil), 409, "conflict")
}

func TestRunStatusTransitions(t *testing.T) {
	r := newRunEnv(t)
	d1, a1, b1 := r.device(r.admin.ID, "Desktop"), r.device(r.anna.ID, "Anna PC"), r.device(r.bob.ID, "Bob PC")
	run := r.create(d1, "Duo", r.anna.ID, r.bob.ID)
	id := run.RunID
	r.mustJoin(a1, id) // bob stays invited

	wantStatus(t, r.status(a1, id, "active"), 403, "forbidden") // starting -> active: the creator only
	wantStatus(t, r.status(a1, id, "paused"), 409, "conflict")  // starting -> paused does not exist
	wantStatus(t, r.do("POST", "/api/v1/runs/"+id+"/status", map[string]any{"status": "starting"}, opt{token: d1.tok, raw: true}), 400, "bad_request")
	started := decode[runResp](t, r.status(d1, id, "active"))
	if started.Status != "active" || started.StatusBy != r.admin.ID {
		t.Fatalf("%+v", started)
	}
	wantStatus(t, r.status(d1, id, "active"), 409, "conflict")

	wantStatus(t, r.status(b1, id, "paused"), 403, "forbidden") // invited, not joined
	paused := decode[runResp](t, r.status(a1, id, "paused"))
	if paused.Status != "paused" || paused.StatusBy != r.anna.ID {
		t.Fatalf("%+v", paused)
	}
	wantStatus(t, r.status(b1, id, "active"), 403, "forbidden")
	wantStatus(t, r.status(a1, id, "finished"), 403, "forbidden") // only the creator ends a run
	if resumed := decode[runResp](t, r.status(a1, id, "active")); resumed.Status != "active" || resumed.StatusBy != r.anna.ID {
		t.Fatalf("%+v", resumed)
	}

	// Status changes are server events with the previous and new status and who did it.
	evs := r.events(d1, id, 0)
	var statuses []string
	for _, e := range evs {
		if e.Type == "run.status" {
			var p struct {
				Status     string `json:"status"`
				Previous   string `json:"previous"`
				ByUserName string `json:"by_user_name"`
			}
			json.Unmarshal(e.Payload, &p)
			statuses = append(statuses, p.Previous+">"+p.Status+"/"+p.ByUserName)
			if e.Source != "hub" || e.UserID != nil {
				t.Fatalf("%+v", e)
			}
		}
	}
	if got := strings.Join(statuses, " "); got != "starting>active/admin active>paused/Anna paused>active/Anna" {
		t.Fatalf("status events %s", got)
	}

	// The creator ends it; nothing changes afterwards.
	wantStatus(t, r.status(d1, id, "abandoned"), 200, "")
	for _, st := range []string{"active", "paused", "finished", "abandoned"} {
		wantStatus(t, r.status(d1, id, st), 409, "conflict")
	}
	if got := r.get(a1, id); got.Status != "abandoned" {
		t.Fatalf("%+v", got)
	}

	// A starting run can be finished directly by its creator, not by another participant.
	run2 := r.create(d1, "Quick", r.anna.ID)
	r.mustJoin(a1, run2.RunID)
	wantStatus(t, r.status(a1, run2.RunID, "abandoned"), 403, "forbidden")
	wantStatus(t, r.status(d1, run2.RunID, "finished"), 200, "")
}

func TestRunStateRevisionsAndLimits(t *testing.T) {
	r := newRunEnv(t)
	d1, a1 := r.device(r.admin.ID, "Desktop"), r.device(r.anna.ID, "Anna PC")
	run := r.create(d1, "State", r.anna.ID)
	id := run.RunID

	rec := r.putState(d1, id, 0, map[string]any{"team": []any{"one"}})
	wantStatus(t, rec, 200, "")
	if rev := decode[struct {
		StateRev int64 `json:"state_rev"`
	}](t, rec).StateRev; rev != 1 {
		t.Fatalf("rev %d", rev)
	}
	// Stale base: 409 state_conflict with the current revision and state.
	rec = r.putState(d1, id, 0, map[string]any{"team": []any{"two"}})
	wantStatus(t, rec, 409, "state_conflict")
	conf := decode[struct {
		StateRev int64           `json:"state_rev"`
		State    json.RawMessage `json:"state"`
	}](t, rec)
	if conf.StateRev != 1 || string(conf.State) != `{"team":["one"]}` {
		t.Fatalf("%+v %s", conf, conf.State)
	}
	wantStatus(t, r.putState(d1, id, 2, map[string]any{}), 409, "state_conflict") // base ahead of the server
	wantStatus(t, r.putState(d1, id, 1, map[string]any{"team": []any{"two"}}), 200, "")
	got := r.get(d1, id) // Anna is only invited: she would not see states (TestRunAuthz*)
	if p := got.part(r.admin.ID).States[pluginA]; p.StateRev != 2 || string(p.State) != `{"team":["two"]}` || p.UpdatedAt == "" {
		t.Fatalf("%+v", p)
	}
	if p := got.part(r.anna.ID); len(p.States) != 0 { // a missing row means state {} with revision 0
		t.Fatalf("anna %+v", p)
	}

	// An invited participant has no state of their own yet.
	wantStatus(t, r.putState(a1, id, 0, map[string]any{}), 403, "forbidden")
	r.mustJoin(a1, id)
	wantStatus(t, r.putState(a1, id, 0, map[string]any{"mine": true}), 200, "")
	// Shape and size: not an object -> 400; over 64 KiB -> 413.
	wantStatus(t, r.do("PUT", "/api/v1/runs/"+id+"/state/"+pluginA, map[string]any{"base_rev": 2, "state": []any{1}}, opt{token: d1.tok, raw: true}), 400, "bad_request")
	wantStatus(t, r.do("PUT", "/api/v1/runs/"+id+"/state/"+pluginA, map[string]any{"base_rev": 2}, opt{token: d1.tok, raw: true}), 400, "bad_request")
	wantStatus(t, r.putState(d1, id, 2, map[string]any{"pad": strings.Repeat("x", 64<<10)}), 413, "too_large")
	// The server keeps opaque numbers exactly.
	rec = r.do("PUT", "/api/v1/runs/"+id+"/state/"+pluginA, nil, opt{token: d1.tok, ctype: "application/json", data: []byte(`{"base_rev":2,"state":{"id":9007199254740993,"f":0.10}}`)})
	wantStatus(t, rec, 200, "")
	if p := r.get(d1, id).part(r.admin.ID).States[pluginA]; string(p.State) != `{"id":9007199254740993,"f":0.10}` {
		t.Fatalf("state %s", p.State)
	}

	// Rate limit: 4/s, burst 20 per participant (the clock is frozen, so there is no refill).
	run2 := r.create(d1, "Rate")
	for i := int64(0); i < 20; i++ {
		wantStatus(t, r.putState(d1, run2.RunID, i, map[string]any{"i": i}), 200, "")
	}
	wantStatus(t, r.putState(d1, run2.RunID, 20, map[string]any{}), 429, "rate_limited")
	r.clk.Advance(time.Second) // 4 tokens
	for i := int64(20); i < 24; i++ {
		wantStatus(t, r.putState(d1, run2.RunID, i, map[string]any{"i": i}), 200, "")
	}
	wantStatus(t, r.putState(d1, run2.RunID, 24, map[string]any{}), 429, "rate_limited")
}

func TestRunStatePerPlugin(t *testing.T) {
	r := newRunEnv(t)
	d1, a1 := r.device(r.admin.ID, "Desktop"), r.device(r.anna.ID, "Anna PC")
	body := r.createBody("Two", r.anna.ID)
	body["plugins"] = []any{map[string]any{"plugin_id": pluginA, "version": "1.0.0"}, map[string]any{"plugin_id": pluginB, "version": "2.1.0"}}
	rec := r.api(d1, "POST", "/api/v1/runs", body)
	wantStatus(t, rec, 201, "")
	id := decode[runResp](t, rec).RunID

	// Independent revisions per plugin.
	wantStatus(t, r.putStateFor(d1, id, pluginA, 0, map[string]any{"a": 1}), 200, "")
	wantStatus(t, r.putStateFor(d1, id, pluginA, 1, map[string]any{"a": 2}), 200, "")
	rec = r.putStateFor(d1, id, pluginB, 0, map[string]any{"b": 1})
	wantStatus(t, rec, 200, "")
	if rev := decode[struct {
		StateRev int64 `json:"state_rev"`
	}](t, rec).StateRev; rev != 1 {
		t.Fatalf("rev %d", rev)
	}
	// Conflict carries the current state of that plugin only.
	rec = r.putStateFor(d1, id, pluginB, 0, map[string]any{"b": 2})
	wantStatus(t, rec, 409, "state_conflict")
	conf := decode[struct {
		StateRev int64           `json:"state_rev"`
		State    json.RawMessage `json:"state"`
	}](t, rec)
	if conf.StateRev != 1 || string(conf.State) != `{"b":1}` {
		t.Fatalf("%+v", conf)
	}
	st := r.get(d1, id).part(r.admin.ID).States
	if len(st) != 2 || st[pluginA].StateRev != 2 || string(st[pluginA].State) != `{"a":2}` || st[pluginB].StateRev != 1 || string(st[pluginB].State) != `{"b":1}` {
		t.Fatalf("%+v", st)
	}
	// A plugin that is not part of the run -> 404; an invited user cannot write.
	wantStatus(t, r.putStateFor(d1, id, pluginC, 0, map[string]any{}), 404, "not_found")
	wantStatus(t, r.putStateFor(a1, id, pluginA, 0, map[string]any{}), 403, "forbidden")

	// The rate limit counts all plugins together (burst 20, the clock is frozen); 4 tokens were used above.
	for i := 0; i < 16; i++ {
		wantStatus(t, r.putStateFor(d1, id, []string{pluginA, pluginB}[i%2], 99, map[string]any{}), 409, "state_conflict")
	}
	wantStatus(t, r.putStateFor(d1, id, pluginA, 2, map[string]any{}), 429, "rate_limited")
}

func TestRunPausedRefusesWrites(t *testing.T) {
	r := newRunEnv(t)
	d1, a1 := r.device(r.admin.ID, "Desktop"), r.device(r.anna.ID, "Anna PC")
	run := r.create(d1, "Pause", r.anna.ID)
	id := run.RunID
	r.mustJoin(a1, id)
	wantStatus(t, r.status(d1, id, "active"), 200, "")
	clientID := uuid.NewString()
	wantStatus(t, r.post(a1, id, "events", r.eventBody(clientID, "caught")), 201, "")
	wantStatus(t, r.putBoard(a1, id, pluginA, map[string]any{"type": "column"}), 200, "")
	wantStatus(t, r.status(a1, id, "paused"), 200, "")

	wantStatus(t, r.event(a1, id, "caught"), 409, "run_not_active")
	wantStatus(t, r.putState(a1, id, 0, map[string]any{}), 409, "run_not_active")
	wantStatus(t, r.putBoard(a1, id, pluginA, map[string]any{"type": "column"}), 409, "run_not_active")
	// A retry of an event that was recorded before the pause still returns its seq.
	rec := r.post(a1, id, "events", r.eventBody(clientID, "caught"))
	wantStatus(t, rec, 200, "")
	// Reads stay possible.
	if len(r.events(a1, id, 0)) == 0 {
		t.Fatal("events invisible while paused")
	}
	wantStatus(t, r.status(a1, id, "active"), 200, "")
	wantStatus(t, r.event(a1, id, "caught"), 201, "")
	for _, end := range []string{"finished", "abandoned"} {
		r2 := r.create(d1, "End "+end, r.anna.ID)
		r.mustJoin(a1, r2.RunID)
		wantStatus(t, r.status(d1, r2.RunID, end), 200, "")
		wantStatus(t, r.event(d1, r2.RunID, "x"), 409, "run_not_active")
		wantStatus(t, r.putState(d1, r2.RunID, 0, map[string]any{}), 409, "run_not_active")
	}
}

func TestRunEventsIdempotencyAndValidation(t *testing.T) {
	r := newRunEnv(t)
	d1, a1, b1 := r.device(r.admin.ID, "Desktop"), r.device(r.anna.ID, "Anna PC"), r.device(r.bob.ID, "Bob PC")
	run := r.create(d1, "Events", r.anna.ID, r.bob.ID)
	id := run.RunID
	r.mustJoin(a1, id)                                 // seq 3
	r.mustJoin(b1, id)                                 // seq 4
	wantStatus(t, r.status(d1, id, "active"), 200, "") // seq 5; events on a starting run would start it themselves

	cid := uuid.NewString()
	rec := r.post(d1, id, "events", r.eventBody(cid, "route.entered"))
	wantStatus(t, rec, 201, "")
	seq := decode[struct{ Seq int64 }](t, rec).Seq
	if seq != 6 {
		t.Fatalf("seq %d", seq)
	}
	rec = r.post(d1, id, "events", r.eventBody(cid, "route.entered"))
	wantStatus(t, rec, 200, "")
	if again := decode[struct{ Seq int64 }](t, rec).Seq; again != seq {
		t.Fatalf("retry returned seq %d, want %d", again, seq)
	}
	wantStatus(t, r.post(a1, id, "events", r.eventBody(cid, "route.entered")), 409, "conflict") // client_id of another user
	evs := r.events(a1, id, 5)
	if len(evs) != 1 {
		t.Fatalf("%+v", evs)
	}
	e := evs[0]
	if e.Seq != 6 || e.ClientID != cid || e.Source != "manual" || e.Type != "route.entered" || e.Text != "text route.entered" ||
		e.UserID == nil || *e.UserID != r.admin.ID || e.UserName != "admin" || e.DeviceID == nil || *e.DeviceID != d1.id ||
		e.PluginID == nil || *e.PluginID != pluginA || string(e.Payload) != `{"n":1}` {
		t.Fatalf("%+v", e)
	}
	// Defaults: text and payload are optional.
	wantStatus(t, r.post(a1, id, "events", map[string]any{"client_id": uuid.NewString(), "plugin_id": pluginA, "source": "automatic", "type": "death"}), 201, "")
	last := r.events(a1, id, 6)
	if len(last) != 1 || last[0].Text != "" || string(last[0].Payload) != "{}" || last[0].Source != "automatic" {
		t.Fatalf("%+v", last)
	}

	// Validation.
	bad := func(mod func(map[string]any), raw bool, status int, code string) {
		t.Helper()
		b := r.eventBody(uuid.NewString(), "x")
		mod(b)
		wantStatus(t, r.do("POST", "/api/v1/runs/"+id+"/events", b, opt{token: d1.tok, raw: raw}), status, code)
	}
	bad(func(b map[string]any) { b["plugin_id"] = pluginB }, false, 400, "bad_request") // not a plugin of this run
	bad(func(b map[string]any) { b["source"] = "hub" }, true, 400, "bad_request")
	bad(func(b map[string]any) { b["type"] = "Bad Type" }, true, 400, "bad_request")
	bad(func(b map[string]any) { b["text"] = strings.Repeat("t", 281) }, true, 400, "bad_request")
	bad(func(b map[string]any) { b["client_id"] = "nope" }, true, 400, "bad_request")
	bad(func(b map[string]any) { b["payload"] = []any{1} }, true, 400, "bad_request")
	bad(func(b map[string]any) { b["payload"] = map[string]any{"pad": strings.Repeat("p", 4<<10)} }, false, 413, "too_large")
	bad(func(b map[string]any) { b["text"] = strings.Repeat("t", 280) }, false, 201, "")
	bad(func(b map[string]any) { b["text"] = strings.Repeat("ä", 280) }, false, 201, "") // characters, not bytes

	// An invited (not joined) participant cannot write.
	r2 := r.create(d1, "Other", r.anna.ID)
	wantStatus(t, r.event(a1, r2.RunID, "x"), 403, "forbidden")

	// Paging.
	page := func(q string) []eventResp {
		rec := r.api(d1, "GET", "/api/v1/runs/"+id+"/events"+q, nil)
		wantStatus(t, rec, 200, "")
		return decode[struct{ Events []eventResp }](t, rec).Events
	}
	if p := page("?after=0&limit=3"); len(p) != 3 || p[0].Seq != 1 || p[2].Seq != 3 {
		t.Fatalf("%+v", p)
	}
	if p := page("?after=3&limit=2"); len(p) != 2 || p[0].Seq != 4 {
		t.Fatalf("%+v", p)
	}
	if p := page(""); len(p) < 5 || p[0].Seq != 1 {
		t.Fatalf("default page %+v", p)
	}
	full := decode[struct {
		Events  []eventResp
		LastSeq int64 `json:"last_seq"`
	}](t, r.api(d1, "GET", "/api/v1/runs/"+id+"/events?after=1000", nil))
	if len(full.Events) != 0 || full.Events == nil || full.LastSeq != r.get(d1, id).LastSeq {
		t.Fatalf("%+v", full)
	}
	for _, q := range []string{"?limit=0", "?limit=501", "?after=-1", "?limit=x"} {
		wantStatus(t, r.do("GET", "/api/v1/runs/"+id+"/events"+q, nil, opt{token: d1.tok, raw: true}), 400, "bad_request")
	}
}

func TestRunEventSeqGaplessUnderConcurrency(t *testing.T) {
	r := newRunEnv(t)
	devs := []dev{r.device(r.admin.ID, "Desktop"), r.device(r.anna.ID, "Anna PC"), r.device(r.bob.ID, "Bob PC")}
	run := r.create(devs[0], "Race", r.anna.ID, r.bob.ID)
	r.mustJoin(devs[1], run.RunID)
	r.mustJoin(devs[2], run.RunID)
	wantStatus(t, r.status(devs[0], run.RunID, "active"), 200, "")

	const perDevice = 25
	var wg sync.WaitGroup
	var mu sync.Mutex
	seqs := map[int64]bool{}
	var failures []string
	for _, d := range devs {
		for w := 0; w < 5; w++ {
			wg.Add(1)
			go func(d dev) {
				defer wg.Done()
				for i := 0; i < perDevice/5; i++ {
					rec := r.do("POST", "/api/v1/runs/"+run.RunID+"/events", r.eventBody(uuid.NewString(), "tick"), opt{token: d.tok, raw: true})
					mu.Lock()
					if rec.Code != 201 {
						failures = append(failures, rec.Body.String())
					} else {
						var res struct{ Seq int64 }
						json.Unmarshal(rec.Body.Bytes(), &res)
						if seqs[res.Seq] {
							failures = append(failures, fmt.Sprintf("seq %d twice", res.Seq))
						}
						seqs[res.Seq] = true
					}
					mu.Unlock()
				}
			}(d)
		}
	}
	wg.Wait()
	if len(failures) > 0 {
		t.Fatalf("%v", failures)
	}
	evs := r.events(devs[0], run.RunID, 0)
	if want := 5 + len(devs)*perDevice; len(evs) != want { // run.created, 3 x run.joined and run.status are the 5 server events
		t.Fatalf("%d events, want %d", len(evs), want)
	}
	for i, e := range evs {
		if e.Seq != int64(i+1) {
			t.Fatalf("event %d has seq %d: the log has a gap", i, e.Seq)
		}
	}
	if got := r.get(devs[0], run.RunID).LastSeq; got != int64(len(evs)) || len(seqs) != len(devs)*perDevice {
		t.Fatalf("last_seq %d, %d events, %d seqs", got, len(evs), len(seqs))
	}
	for s := range seqs {
		if s < 1 || s > int64(len(evs)) {
			t.Fatalf("seq %d out of range", s)
		}
	}
}

func TestRunEventRateLimit(t *testing.T) {
	r := newRunEnv(t)
	d1 := r.device(r.admin.ID, "Desktop")
	run := r.create(d1, "Rate")
	for i := 0; i < 30; i++ {
		wantStatus(t, r.event(d1, run.RunID, "tick"), 201, "")
	}
	wantStatus(t, r.event(d1, run.RunID, "tick"), 429, "rate_limited")
	r.clk.Advance(time.Second) // 2 tokens
	wantStatus(t, r.event(d1, run.RunID, "tick"), 201, "")
	wantStatus(t, r.event(d1, run.RunID, "tick"), 201, "")
	wantStatus(t, r.event(d1, run.RunID, "tick"), 429, "rate_limited")
	// The bucket belongs to the participant and the run: another run is unaffected.
	run2 := r.create(d1, "Rate 2")
	wantStatus(t, r.event(d1, run2.RunID, "tick"), 201, "")
}

func TestRunBoards(t *testing.T) {
	r := newRunEnv(t)
	d1, a1 := r.device(r.admin.ID, "Desktop"), r.device(r.anna.ID, "Anna PC")
	run := r.create(d1, "Board", r.anna.ID)
	id := run.RunID
	tree := map[string]any{"type": "column", "children": []any{map[string]any{"type": "text", "text": "Hello"}}}

	rec := r.putBoard(d1, id, pluginA, tree)
	wantStatus(t, rec, 200, "")
	if rev := decode[struct{ Rev int64 }](t, rec).Rev; rev != 1 {
		t.Fatalf("rev %d", rev)
	}
	if rev := decode[struct{ Rev int64 }](t, r.putBoard(d1, id, pluginA, tree)).Rev; rev != 2 {
		t.Fatalf("rev %d", rev)
	}
	boards, err := r.svc.RunBoards(context.Background(), id)
	if err != nil || len(boards) != 1 || boards[0].PluginID != pluginA || boards[0].Rev != 2 || boards[0].UpdatedBy != "admin" ||
		string(boards[0].Tree) != `{"children":[{"text":"Hello","type":"text"}],"type":"column"}` {
		t.Fatalf("%v %+v", err, boards)
	}
	wantStatus(t, r.putBoard(a1, id, pluginA, tree), 403, "forbidden") // invited, not joined
	wantStatus(t, r.putBoard(d1, id, pluginB, tree), 404, "not_found") // not a plugin of the run
	wantStatus(t, r.putBoard(d1, id, pluginA, map[string]any{"pad": strings.Repeat("x", 64<<10)}), 413, "too_large")
	wantStatus(t, r.do("PUT", "/api/v1/runs/"+id+"/boards/"+pluginA, map[string]any{"tree": []any{}}, opt{token: d1.tok, raw: true}), 400, "bad_request")
	wantStatus(t, r.do("PUT", "/api/v1/runs/"+id+"/boards/"+pluginA, map[string]any{}, opt{token: d1.tok, raw: true}), 400, "bad_request")

	// 1 write/s, burst 5 per participant: 2 were used above and the rejected ones count too (3 more tokens).
	r.clk.Advance(time.Minute)
	for i := 0; i < 5; i++ {
		wantStatus(t, r.putBoard(d1, id, pluginA, tree), 200, "")
	}
	wantStatus(t, r.putBoard(d1, id, pluginA, tree), 429, "rate_limited")
}

func TestRunsUsingPluginVersion(t *testing.T) {
	r := newRunEnv(t)
	ctx := context.Background()
	d1 := r.device(r.admin.ID, "Desktop")
	uses := func(id, v string) bool {
		t.Helper()
		ok, err := r.svc.RunsUsingPluginVersion(ctx, id, v)
		if err != nil {
			t.Fatal(err)
		}
		return ok
	}
	if uses(pluginA, "1.0.0") {
		t.Fatal("no run yet")
	}
	run := r.create(d1, "Uses")
	if !uses(pluginA, "1.0.0") || uses(pluginA, "2.0.0") || uses(pluginB, "2.1.0") {
		t.Fatal("starting run must pin exactly its plugin version")
	}
	wantStatus(t, r.status(d1, run.RunID, "active"), 200, "")
	wantStatus(t, r.status(d1, run.RunID, "paused"), 200, "")
	if !uses(pluginA, "1.0.0") {
		t.Fatal("paused run still uses the version")
	}
	wantStatus(t, r.status(d1, run.RunID, "active"), 200, "")
	wantStatus(t, r.status(d1, run.RunID, "finished"), 200, "")
	if uses(pluginA, "1.0.0") {
		t.Fatal("finished run must release the version")
	}
}

// ---- WSS ----

func runUpdated(m wsMsg) runResp {
	var p struct{ Run runResp }
	json.Unmarshal(m.Payload, &p)
	return p.Run
}

func runEventOf(m wsMsg) (string, eventResp) {
	var p struct {
		RunID string `json:"run_id"`
		Event eventResp
	}
	json.Unmarshal(m.Payload, &p)
	return p.RunID, p.Event
}

func isRunUpdated(id string, status string) func(wsMsg) bool {
	return func(m wsMsg) bool { r := runUpdated(m); return r.RunID == id && (status == "" || r.Status == status) }
}

func isRunEvent(id, typ string) func(wsMsg) bool {
	return func(m wsMsg) bool { rid, e := runEventOf(m); return rid == id && (typ == "" || e.Type == typ) }
}

func TestRunWSFanOut(t *testing.T) {
	r := newRunEnv(t)
	d1, d1b := r.device(r.admin.ID, "Desktop"), r.device(r.admin.ID, "Laptop")
	a1, b1, c1 := r.device(r.anna.ID, "Anna PC"), r.device(r.bob.ID, "Bob PC"), r.device(r.carol.ID, "Carol PC")
	wd1, wd1b, wa, wb, wc := r.dial(d1), r.dial(d1b), r.dial(a1), r.dial(b1), r.dial(c1)
	for _, w := range []*wsClient{wd1, wd1b, wa, wb, wc} {
		w.settle()
	}

	// Create: run_updated to every participant (all statuses, both devices of the creator), none to a stranger;
	// run_event for run.created and run.joined only to the joined creator.
	run := r.create(d1, "Fan", r.anna.ID, r.bob.ID)
	id := run.RunID
	for _, w := range []*wsClient{wd1, wd1b, wa, wb} {
		if got := runUpdated(w.awaitWhere("run_updated", isRunUpdated(id, "starting"))); len(got.Participants) != 3 {
			t.Fatalf("%+v", got)
		}
	}
	for _, w := range []*wsClient{wd1, wd1b} {
		w.awaitWhere("run_event", isRunEvent(id, "run.created"))
		w.awaitWhere("run_event", isRunEvent(id, "run.joined"))
	}
	wc.quiet("run_updated", "run_event", "run_state")
	wa.quiet("run_event")
	wb.quiet("run_event")

	// Anna joins: everybody involved gets run_updated, the joined (now including Anna) get the server event.
	r.mustJoin(a1, id)
	for _, w := range []*wsClient{wd1, wd1b, wa, wb} {
		w.awaitWhere("run_updated", func(m wsMsg) bool { return runUpdated(m).part(r.anna.ID).Status == "joined" })
	}
	for _, w := range []*wsClient{wd1, wd1b, wa} {
		_, e := runEventOf(w.awaitWhere("run_event", isRunEvent(id, "run.joined")))
		if e.Seq != 3 {
			t.Fatalf("seq %d", e.Seq)
		}
	}
	wb.quiet("run_event")
	wc.quiet()

	// State: to the other joined participants and the writer's other device, not to the writing device,
	// not to invited users.
	wantStatus(t, r.putState(d1, id, 0, map[string]any{"team": []any{1}}), 200, "")
	for _, w := range []*wsClient{wd1b, wa} {
		m := w.await("run_state")
		var p struct {
			RunID    string          `json:"run_id"`
			UserID   string          `json:"user_id"`
			PluginID string          `json:"plugin_id"`
			StateRev int64           `json:"state_rev"`
			State    json.RawMessage `json:"state"`
		}
		json.Unmarshal(m.Payload, &p)
		if p.RunID != id || p.UserID != r.admin.ID || p.PluginID != pluginA || p.StateRev != 1 || string(p.State) != `{"team":[1]}` {
			t.Fatalf("%+v %s", p, p.State)
		}
	}
	wd1.quiet("run_state")
	wb.quiet("run_state")
	wc.quiet()

	// Start: the manual start goes out like any status change (events on a starting run would start it by themselves).
	wantStatus(t, r.status(d1, id, "active"), 200, "")
	for _, w := range []*wsClient{wd1, wd1b, wa, wb} {
		w.awaitWhere("run_updated", isRunUpdated(id, "active"))
	}
	for _, w := range []*wsClient{wd1, wd1b, wa} {
		w.awaitWhere("run_event", isRunEvent(id, "run.status"))
	}

	// Event: every joined participant's connections, the author's own device included.
	cid := uuid.NewString()
	wantStatus(t, r.post(a1, id, "events", r.eventBody(cid, "caught")), 201, "")
	for _, w := range []*wsClient{wd1, wd1b, wa} {
		_, e := runEventOf(w.awaitWhere("run_event", isRunEvent(id, "caught")))
		if e.ClientID != cid || e.Seq != 5 || e.UserID == nil || *e.UserID != r.anna.ID {
			t.Fatalf("%+v", e)
		}
	}
	wb.quiet("run_event")
	// A retry (200) is not published again.
	wantStatus(t, r.post(a1, id, "events", r.eventBody(cid, "caught")), 200, "")
	wd1.quiet("run_event")

	// Rejected writes publish nothing.
	wantStatus(t, r.putState(d1, id, 0, map[string]any{}), 409, "state_conflict")
	wantStatus(t, r.event(b1, id, "x"), 403, "forbidden")
	wd1.quiet("run_state", "run_event")

	// Status: run_updated to all participants, the run.status event to the joined.
	wantStatus(t, r.status(a1, id, "paused"), 200, "")
	for _, w := range []*wsClient{wd1, wd1b, wa, wb} {
		w.awaitWhere("run_updated", isRunUpdated(id, "paused"))
	}
	for _, w := range []*wsClient{wd1, wd1b, wa} {
		_, e := runEventOf(w.awaitWhere("run_event", func(m wsMsg) bool {
			_, e := runEventOf(m)
			return e.Type == "run.status" && strings.Contains(string(e.Payload), `"paused"`)
		}))
		if e.UserID != nil || e.Source != "hub" {
			t.Fatalf("%+v", e)
		}
	}
	wc.quiet()

	// Leave: Anna gets her own run.left event even though she is no longer joined; Bob (invited) gets none.
	wantStatus(t, r.post(a1, id, "leave", nil), 200, "")
	wa.awaitWhere("run_updated", func(m wsMsg) bool { return runUpdated(m).part(r.anna.ID).Status == "left" })
	wa.awaitWhere("run_event", isRunEvent(id, "run.left"))
	wd1.awaitWhere("run_event", isRunEvent(id, "run.left"))
	wb.awaitWhere("run_updated", isRunUpdated(id, "paused"))
	wb.quiet("run_event")
}

func TestRunWSCatchUpAfterHello(t *testing.T) {
	r := newRunEnv(t)
	ctx := context.Background()
	d1, a1, b1 := r.device(r.admin.ID, "Desktop"), r.device(r.anna.ID, "Anna PC"), r.device(r.bob.ID, "Bob PC")
	run := r.create(d1, "Catch", r.anna.ID, r.bob.ID)
	id := run.RunID
	r.mustJoin(a1, id)                                 // seq 3
	wantStatus(t, r.status(d1, id, "active"), 200, "") // seq 4
	wantStatus(t, r.putState(d1, id, 0, map[string]any{"v": 1}), 200, "")
	for i := 0; i < 5; i++ { // seq 5..9 while Anna is offline
		wantStatus(t, r.event(d1, id, fmt.Sprintf("offline.%d", i)), 201, "")
	}
	archived := r.create(d1, "Old", r.anna.ID)
	r.mustJoin(a1, archived.RunID)
	wantStatus(t, r.status(d1, archived.RunID, "finished"), 200, "")
	if err := r.svc.ArchiveRun(ctx, archived.RunID, true); err != nil {
		t.Fatal(err)
	}

	hello := func(d dev, runs any) *wsClient {
		c := r.dialRaw(d)
		p := map[string]any{"protocol_version": 1, "device_id": d.id}
		if runs != nil {
			p["runs"] = runs
		}
		c.send("hello", p)
		ack := c.await("hello_ack")
		var feats struct{ Features []string }
		json.Unmarshal(ack.Payload, &feats)
		if !strings.Contains(strings.Join(feats.Features, " "), "runs_v1") {
			t.Fatalf("hello_ack features %v lack runs_v1", feats.Features)
		}
		return c
	}
	entry := func(runID string, last int64) map[string]any {
		return map[string]any{"run_id": runID, "last_seq": last}
	}

	// Listed with last_seq 3: run_updated (full, with the state) first, then run_events 4..8 in order; nothing for
	// the archived run.
	a2 := r.device(r.anna.ID, "Anna Laptop")
	c := hello(a2, []any{entry(id, 4), entry(archived.RunID, 0), entry(uuid.NewString(), 5)})
	up := runUpdated(c.awaitWhere("run_updated", func(m wsMsg) bool {
		if runUpdated(m).RunID == archived.RunID {
			t.Error("an archived run was sent in the catch-up")
		}
		return isRunUpdated(id, "")(m)
	}))
	if up.LastSeq != 9 || string(up.part(r.admin.ID).States[pluginA].State) != `{"v":1}` {
		t.Fatalf("%+v", up)
	}
	for want := int64(5); want <= 9; want++ {
		rid, e := runEventOf(c.await("run_event"))
		if rid != id || e.Seq != want {
			t.Fatalf("got seq %d (run %s), want %d", e.Seq, rid, want)
		}
	}
	c.quiet("run_event", "run_updated")

	// Not listed: run_updated only. Up to date: no events.
	c2 := hello(r.device(r.anna.ID, "Anna Tablet"), nil)
	c2.awaitWhere("run_updated", isRunUpdated(id, ""))
	c2.quiet("run_event")
	c3 := hello(r.device(r.anna.ID, "Anna Phone"), []any{entry(id, 9)})
	c3.awaitWhere("run_updated", isRunUpdated(id, ""))
	c3.quiet("run_event")
	// A malformed runs list is ignored, never fatal.
	d4 := r.device(r.anna.ID, "Anna Watch")
	c4 := r.dialRaw(d4)
	c4.sendRaw([]byte(`{"type":"hello","payload":{"protocol_version":1,"device_id":"` + d4.id + `","runs":"bogus"}}`))
	c4.await("hello_ack")
	c4.awaitWhere("run_updated", isRunUpdated(id, ""))

	// An invited participant gets run_updated but never events; the archived run is not sent to anybody.
	cb := hello(b1, []any{entry(id, 0)})
	cb.awaitWhere("run_updated", isRunUpdated(id, ""))
	cb.quiet("run_event")
	for _, w := range []*wsClient{c, c2, c3, c4, cb} { // the bulk below must not flood clients nobody reads from
		w.quiet("run_updated")
		w.conn.CloseNow()
	}

	// More than 500 events missing: none are pushed, only run_updated. At most 500 missing: all of them, in order.
	for i := 0; i < 600; i++ {
		if i%25 == 0 {
			r.clk.Advance(15 * time.Second) // refill the 2/s bucket (30 tokens; access tokens last 15 min)
		}
		if _, _, err := r.svc.AppendRunEvent(ctx, hub.AppendRunEventInput{UserID: r.admin.ID, DeviceID: d1.id, RunID: id, ClientID: uuid.NewString(),
			PluginID: pluginA, Source: "automatic", Type: "bulk", Payload: json.RawMessage(`{}`)}); err != nil {
			t.Fatal(err)
		}
	}
	total := r.get(d1, id).LastSeq // 609
	if total != 609 {
		t.Fatalf("last_seq %d", total)
	}
	far := hello(r.device(r.anna.ID, "Far"), []any{entry(id, 101)}) // 508 missing
	far.awaitWhere("run_updated", isRunUpdated(id, ""))
	far.quiet("run_event")
	edge := hello(r.device(r.anna.ID, "Edge"), []any{entry(id, 109)}) // exactly 500 missing
	edge.awaitWhere("run_updated", isRunUpdated(id, ""))
	for want := int64(110); want <= total; want++ {
		_, e := runEventOf(edge.await("run_event"))
		if e.Seq != want {
			t.Fatalf("got seq %d, want %d", e.Seq, want)
		}
	}
	// The app pages the rest over REST.
	if evs := r.events(a1, id, 100); len(evs) != 500 || evs[0].Seq != 101 {
		t.Fatalf("%d events, first %d", len(evs), evs[0].Seq)
	}
}

func TestRunWSLiveMessagesFollowCatchUp(t *testing.T) {
	// A live run_event that happens while the catch-up is still being delivered arrives after it, in seq order.
	r := newRunEnv(t)
	ctx := context.Background()
	d1, a1 := r.device(r.admin.ID, "Desktop"), r.device(r.anna.ID, "Anna PC")
	run := r.create(d1, "Order", r.anna.ID)
	r.mustJoin(a1, run.RunID)
	for i := 0; i < 400; i++ {
		if i%25 == 0 {
			r.clk.Advance(15 * time.Second)
		}
		if _, _, err := r.svc.AppendRunEvent(ctx, hub.AppendRunEventInput{UserID: r.admin.ID, DeviceID: d1.id, RunID: run.RunID, ClientID: uuid.NewString(),
			PluginID: pluginA, Source: "automatic", Type: "bulk", Payload: json.RawMessage(`{}`)}); err != nil {
			t.Fatal(err)
		}
	}
	c := r.dialRaw(a1)
	c.send("hello", map[string]any{"protocol_version": 1, "device_id": a1.id, "runs": []any{map[string]any{"run_id": run.RunID, "last_seq": 0}}})
	c.await("hello_ack")
	r.clk.Advance(time.Minute)
	for i := 0; i < 5; i++ { // live events while the 403 catch-up messages are delivered
		wantStatus(t, r.event(d1, run.RunID, "live"), 201, "")
	}
	var seqs []int64
	for len(seqs) < 400+5+3 {
		_, e := runEventOf(c.await("run_event"))
		seqs = append(seqs, e.Seq)
	}
	for i := 1; i < len(seqs); i++ {
		if seqs[i] != seqs[i-1]+1 {
			t.Fatalf("event order broken at %d: %d after %d", i, seqs[i], seqs[i-1])
		}
	}
}

func TestRunInvite(t *testing.T) {
	r := newRunEnv(t)
	d1, a1, b1, c1 := r.device(r.admin.ID, "Desktop"), r.device(r.anna.ID, "Anna PC"), r.device(r.bob.ID, "Bob PC"), r.device(r.carol.ID, "Carol PC")
	wd, wa, wb, wc := r.dial(d1), r.dial(a1), r.dial(b1), r.dial(c1)
	for _, w := range []*wsClient{wd, wa, wb, wc} {
		w.settle()
	}
	run := r.create(d1, "Invite", r.anna.ID)
	id := run.RunID
	wa.awaitWhere("run_updated", isRunUpdated(id, "starting"))

	// Invited, not joined: not allowed to invite (403). Strangers see 404, bodies are validated.
	wantStatus(t, r.post(a1, id, "invite", map[string]any{"user_ids": []string{r.bob.ID}}), 403, "forbidden")
	wantStatus(t, r.post(c1, id, "invite", map[string]any{"user_ids": []string{r.bob.ID}}), 404, "not_found")
	wantStatus(t, r.post(d1, id, "invite", map[string]any{"user_ids": []string{"u_nobody"}}), 400, "bad_request")
	wantStatus(t, r.do("POST", "/api/v1/runs/"+id+"/invite", map[string]any{"user_ids": []string{}}, opt{token: d1.tok, raw: true}), 400, "bad_request")

	// A joined participant invites; the server event names the invited users, run_updated reaches everybody involved.
	got := decode[runResp](t, r.post(d1, id, "invite", map[string]any{"user_ids": []string{r.bob.ID, r.carol.ID, r.bob.ID, r.admin.ID, r.anna.ID}}))
	if len(got.Participants) != 4 || got.part(r.bob.ID).Status != "invited" || got.part(r.carol.ID).Status != "invited" {
		t.Fatalf("%+v", got)
	}
	for _, w := range []*wsClient{wd, wa, wb, wc} {
		w.awaitWhere("run_updated", func(m wsMsg) bool { return len(runUpdated(m).Participants) == 4 })
	}
	_, e := runEventOf(wd.awaitWhere("run_event", isRunEvent(id, "run.invited")))
	var pl struct {
		UserID  string `json:"user_id"`
		Invited []struct {
			UserID string `json:"user_id"`
		} `json:"invited"`
	}
	if err := json.Unmarshal(e.Payload, &pl); err != nil || pl.UserID != r.admin.ID || len(pl.Invited) != 2 {
		t.Fatalf("payload %s %v", e.Payload, err)
	}
	wa.quiet("run_event") // Anna has not joined

	// Nobody new: no-op without an event. A user who declined is invited again.
	seq := r.get(d1, id).LastSeq
	if again := decode[runResp](t, r.post(d1, id, "invite", map[string]any{"user_ids": []string{r.bob.ID}})); again.LastSeq != seq {
		t.Fatalf("seq %d, want %d", again.LastSeq, seq)
	}
	wantStatus(t, r.post(b1, id, "decline", nil), 200, "")
	if re := decode[runResp](t, r.post(d1, id, "invite", map[string]any{"user_ids": []string{r.bob.ID}})); re.part(r.bob.ID).Status != "invited" || re.LastSeq != seq+2 {
		t.Fatalf("%+v", re)
	}

	// Allowed while paused, refused when the run is over.
	wantStatus(t, r.status(d1, id, "active"), 200, "")
	wantStatus(t, r.status(d1, id, "paused"), 200, "")
	wantStatus(t, r.post(d1, id, "invite", map[string]any{"user_ids": []string{r.dave.ID}}), 200, "")
	wantStatus(t, r.status(d1, id, "active"), 200, "")
	wantStatus(t, r.status(d1, id, "finished"), 200, "")
	wantStatus(t, r.post(d1, id, "invite", map[string]any{"user_ids": []string{r.erin.ID}}), 409, "conflict")
}

func TestRunInviteParticipantCap(t *testing.T) {
	r := newRunEnv(t)
	d1 := r.device(r.admin.ID, "Desktop")
	run := r.create(d1, "Cap", r.anna.ID, r.bob.ID, r.carol.ID, r.dave.ID, r.erin.ID, r.frank.ID)            // 7
	wantStatus(t, r.post(d1, run.RunID, "invite", map[string]any{"user_ids": []string{r.gina.ID}}), 200, "") // 8
	wantStatus(t, r.post(d1, run.RunID, "invite", map[string]any{"user_ids": []string{r.hank.ID}}), 409, "conflict")
	if got := r.get(d1, run.RunID); len(got.Participants) != 8 {
		t.Fatalf("%+v", got)
	}
}
