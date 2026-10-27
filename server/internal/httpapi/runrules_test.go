package httpapi

import (
	"encoding/json"
	"fmt"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/google/uuid"
)

// Core run rules (0.20 package 2, feature run_rules_v1): defaults, create with rules, PUT /runs/{id}/rules with the
// creator-or-admin rule, the rules.changed event and the run_updated push, and the events the app writes itself.

type rulesT struct {
	States      bool   `json:"states"`
	Rewind      bool   `json:"rewind"`
	Cheats      string `json:"cheats"`
	MemoryWrite string `json:"memory_write"`
}

type runRulesResp struct {
	RunID string  `json:"run_id"`
	Rules *rulesT `json:"rules"`
}

type rulesChangedPayload struct {
	Before rulesT `json:"before"`
	After  rulesT `json:"after"`
	By     string `json:"by_user_id"`
}

var defaultRules = rulesT{Cheats: "off", MemoryWrite: "off"}

func (r *runEnv) putRules(d dev, id string, body any) *httptest.ResponseRecorder {
	r.t.Helper()
	return r.api(d, "PUT", "/api/v1/runs/"+id+"/rules", body)
}

func (r *runEnv) rulesOf(d dev, id string) rulesT {
	r.t.Helper()
	rec := r.api(d, "GET", "/api/v1/runs/"+id, nil)
	wantStatus(r.t, rec, 200, "")
	got := decode[runRulesResp](r.t, rec)
	if got.Rules == nil {
		r.t.Fatalf("run without rules: %s", rec.Body.String())
	}
	return *got.Rules
}

func TestRunRulesDefaultsAndCreate(t *testing.T) {
	r := newRunEnv(t)
	anna := r.device(r.anna.ID, "Annas PC")

	// A new run without rules gets the defaults; the list carries them too.
	run := r.create(anna, "Plain")
	if got := r.rulesOf(anna, run.RunID); got != defaultRules {
		t.Fatalf("%+v", got)
	}
	rec := r.api(anna, "GET", "/api/v1/runs", nil)
	wantStatus(t, rec, 200, "")
	for _, x := range decode[struct{ Runs []runRulesResp }](t, rec).Runs {
		if x.Rules == nil || *x.Rules != defaultRules {
			t.Fatalf("%+v", x)
		}
	}

	// Rules at creation.
	body := r.createBody("Strict")
	body["rules"] = map[string]any{"states": true, "rewind": true, "cheats": "logged", "memory_write": "off"}
	rec = r.api(anna, "POST", "/api/v1/runs", body)
	wantStatus(t, rec, 201, "")
	got := decode[runRulesResp](t, rec)
	if want := (rulesT{States: true, Rewind: true, Cheats: "logged", MemoryWrite: "off"}); got.Rules == nil || *got.Rules != want {
		t.Fatalf("%+v", got.Rules)
	}
	if r.rulesOf(anna, got.RunID) != *got.Rules {
		t.Fatal("rules differ after reading the run back")
	}

	// Invalid values are refused (sent unvalidated, the spec has the enum).
	for _, bad := range []map[string]any{
		{"states": true, "rewind": true, "cheats": "on", "memory_write": "off"},
		{"states": true, "rewind": true, "cheats": "off", "memory_write": ""},
	} {
		b := r.createBody("Bad")
		b["rules"] = bad
		wantStatus(t, r.do("POST", "/api/v1/runs", b, opt{token: anna.tok, raw: true}), 400, "bad_request")
	}
}

func TestRunRulesPutAuthz(t *testing.T) {
	r := newRunEnv(t)
	anna, bob := r.device(r.anna.ID, "Annas PC"), r.device(r.bob.ID, "Bobs PC")
	carol, admin := r.device(r.carol.ID, "Carols PC"), r.device(r.admin.ID, "Admin PC")
	run := r.create(anna, "Rules", r.bob.ID, r.carol.ID)
	id := run.RunID
	r.mustJoin(bob, id)
	on := map[string]any{"states": true, "rewind": false, "cheats": "logged", "memory_write": "off"}

	wantStatus(t, r.do("PUT", "/api/v1/runs/"+id+"/rules", on, opt{}), 401, "")
	// A joined participant who did not create the run, and an invited one: 403.
	wantStatus(t, r.putRules(bob, id, on), 403, "forbidden")
	wantStatus(t, r.putRules(carol, id, on), 403, "forbidden")
	// Somebody outside the run does not learn that it exists.
	stranger := r.device(r.dave.ID, "Daves PC")
	wantStatus(t, r.putRules(stranger, id, on), 404, "not_found")
	wantStatus(t, r.putRules(anna, uuid.NewString(), on), 404, "not_found")
	if got := r.rulesOf(anna, id); got != defaultRules {
		t.Fatalf("a refused change must not apply: %+v", got)
	}

	// The creator may; the whole object is replaced.
	rec := r.putRules(anna, id, on)
	wantStatus(t, rec, 200, "")
	if got := decode[runRulesResp](t, rec); got.RunID != id || got.Rules == nil || *got.Rules != (rulesT{States: true, Cheats: "logged", MemoryWrite: "off"}) {
		t.Fatalf("%+v", got)
	}
	// A server admin who is not part of the run may too.
	wantStatus(t, r.putRules(admin, id, map[string]any{"states": false, "rewind": true, "cheats": "off", "memory_write": "logged"}), 200, "")
	if got := r.rulesOf(bob, id); got != (rulesT{Rewind: true, Cheats: "off", MemoryWrite: "logged"}) {
		t.Fatalf("%+v", got)
	}

	// Invalid bodies: unknown value (unvalidated), missing fields, no body.
	wantStatus(t, r.do("PUT", "/api/v1/runs/"+id+"/rules", map[string]any{"states": true, "rewind": true, "cheats": "maybe", "memory_write": "off"}, opt{token: anna.tok, raw: true}), 400, "bad_request")
	wantStatus(t, r.do("PUT", "/api/v1/runs/"+id+"/rules", map[string]any{"states": true}, opt{token: anna.tok, raw: true}), 400, "bad_request")

	// Paused runs can be changed, finished runs cannot.
	wantStatus(t, r.status(anna, id, "active"), 200, "")
	wantStatus(t, r.status(anna, id, "paused"), 200, "")
	wantStatus(t, r.putRules(anna, id, on), 200, "")
	wantStatus(t, r.status(anna, id, "finished"), 200, "")
	wantStatus(t, r.putRules(anna, id, defaultRulesBody()), 409, "conflict")
}

func defaultRulesBody() map[string]any {
	return map[string]any{"states": false, "rewind": false, "cheats": "off", "memory_write": "off"}
}

func TestRunRulesChangedEventAndPush(t *testing.T) {
	r := newRunEnv(t)
	anna, bob := r.device(r.anna.ID, "Annas PC"), r.device(r.bob.ID, "Bobs PC")
	carol, dave := r.device(r.carol.ID, "Carols PC"), r.device(r.dave.ID, "Daves PC")
	run := r.create(anna, "Live", r.bob.ID, r.carol.ID)
	id := run.RunID
	r.mustJoin(bob, id)
	wa, wb, wc, wd := r.dial(anna), r.dial(bob), r.dial(carol), r.dial(dave)
	for _, w := range []*wsClient{wa, wb, wc, wd} {
		w.settle()
	}
	last := run.LastSeq + 1 // bob joined

	want := rulesT{States: true, Rewind: true, Cheats: "logged", MemoryWrite: "logged"}
	wantStatus(t, r.putRules(anna, id, map[string]any{"states": true, "rewind": true, "cheats": "logged", "memory_write": "logged"}), 200, "")

	// run_updated carries the new rules to every participant (invited too); the event only to the joined ones.
	for _, w := range []*wsClient{wa, wb, wc} {
		m := w.awaitWhere("run_updated", func(m wsMsg) bool {
			var p struct{ Run runRulesResp }
			json.Unmarshal(m.Payload, &p)
			return p.Run.RunID == id && p.Run.Rules != nil && *p.Run.Rules == want
		})
		_ = m
	}
	for _, w := range []*wsClient{wa, wb} {
		_, e := runEventOf(w.awaitWhere("run_event", isRunEvent(id, "rules.changed")))
		if e.Source != "hub" || e.UserID != nil || e.Seq != last+1 {
			t.Fatalf("%+v", e)
		}
		var p rulesChangedPayload
		if err := json.Unmarshal(e.Payload, &p); err != nil || p.Before != defaultRules || p.After != want || p.By != r.anna.ID {
			t.Fatalf("payload %s: %+v %v", e.Payload, p, err)
		}
	}
	wc.quiet("run_event")
	wd.quiet("run_updated", "run_event")

	// The event is in the log.
	evs := r.events(anna, id, last)
	if len(evs) != 1 || evs[0].Type != "rules.changed" {
		t.Fatalf("%+v", evs)
	}

	// The same rules again change nothing: no event, no push.
	wantStatus(t, r.putRules(anna, id, map[string]any{"states": true, "rewind": true, "cheats": "logged", "memory_write": "logged"}), 200, "")
	if evs = r.events(anna, id, last); len(evs) != 1 {
		t.Fatalf("a no-op wrote an event: %+v", evs)
	}
	wb.quiet("run_event")

	// Switching a rule off is a change like any other (the apps react to the pushed rules).
	wantStatus(t, r.putRules(anna, id, defaultRulesBody()), 200, "")
	_, e := runEventOf(wb.awaitWhere("run_event", isRunEvent(id, "rules.changed")))
	var p rulesChangedPayload
	if err := json.Unmarshal(e.Payload, &p); err != nil || p.Before != want || p.After != defaultRules {
		t.Fatalf("payload %s: %+v %v", e.Payload, p, err)
	}
}

func TestRunCoreEventsFromTheApp(t *testing.T) {
	r := newRunEnv(t)
	anna := r.device(r.anna.ID, "Annas PC")
	// A run without any plugin: the app still writes its own events.
	body := r.createBody("Naked")
	body["plugins"] = []any{}
	rec := r.api(anna, "POST", "/api/v1/runs", body)
	wantStatus(t, rec, 201, "")
	id := decode[runResp](t, rec).RunID

	for _, typ := range []string{"cheat.enabled", "cheat.disabled", "memory.write", "state.loaded"} {
		ev := map[string]any{"client_id": uuid.NewString(), "source": "automatic", "type": typ, "text": typ, "payload": map[string]any{"n": 1}}
		wantStatus(t, r.post(anna, id, "events", ev), 201, "")
	}
	// With the plugin that caused it, also when that plugin is not part of the run.
	ev := map[string]any{"client_id": uuid.NewString(), "plugin_id": "net.example.cheats", "source": "automatic", "type": "memory.write", "payload": map[string]any{"bytes": 4}}
	wantStatus(t, r.post(anna, id, "events", ev), 201, "")

	evs := r.events(anna, id, 2) // after run.created and run.joined
	if len(evs) != 5 {
		t.Fatalf("%+v", evs)
	}
	for i, typ := range []string{"cheat.enabled", "cheat.disabled", "memory.write", "state.loaded", "memory.write"} {
		e := evs[i]
		if e.Type != typ || e.Source != "automatic" || e.UserID == nil || *e.UserID != r.anna.ID || (i < 4 && e.PluginID != nil) {
			t.Fatalf("%d: %+v", i, e)
		}
	}
	if evs[4].PluginID == nil || *evs[4].PluginID != "net.example.cheats" || !strings.Contains(string(evs[4].Payload), `"bytes":4`) {
		t.Fatalf("%+v", evs[4])
	}

	// Every other type still needs a plugin of the run.
	ev = map[string]any{"client_id": uuid.NewString(), "source": "manual", "type": "boss.defeated"}
	wantStatus(t, r.do("POST", fmt.Sprintf("/api/v1/runs/%s/events", id), ev, opt{token: anna.tok}), 400, "bad_request")
	ev["plugin_id"] = pluginA
	wantStatus(t, r.post(anna, id, "events", ev), 400, "bad_request") // pluginA is not part of this run
	// The idempotence of client_id holds for the app's events too.
	cid := uuid.NewString()
	ev = map[string]any{"client_id": cid, "source": "automatic", "type": "state.loaded"}
	wantStatus(t, r.post(anna, id, "events", ev), 201, "")
	wantStatus(t, r.post(anna, id, "events", ev), 200, "")
}
