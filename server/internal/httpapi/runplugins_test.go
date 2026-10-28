package httpapi

import (
	"bytes"
	"context"
	"encoding/json"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/google/uuid"
	"github.com/phabioo/uzume/server/internal/hub/hubtest"
)

// Plugin updates in a running run (uzume issue #335, feature run_plugin_updates_v1): PUT /runs/{id}/plugins.
// These tests use the real plugin store as catalog, so manifests (requires, run_update_from) take part.

type rpResp struct {
	RunID   string `json:"run_id"`
	Plugins []struct {
		PluginID        string  `json:"plugin_id"`
		Version         string  `json:"version"`
		SHA256          string  `json:"sha256"`
		PreviousVersion *string `json:"previous_version"`
	} `json:"plugins"`
}

func (x rpResp) pins() string {
	var parts []string
	for _, p := range x.Plugins {
		s := p.PluginID + "@" + p.Version
		if p.PreviousVersion != nil {
			s += "(was " + *p.PreviousVersion + ")"
		}
		parts = append(parts, s)
	}
	return strings.Join(parts, ",")
}

type changesPayload struct {
	Changes []struct {
		PluginID string `json:"plugin_id"`
		From     string `json:"from"`
		To       string `json:"to"`
	} `json:"changes"`
	By     string `json:"by_user_id"`
	ByName string `json:"by_user_name"`
}

// rpEnv is a run environment whose plugin catalog is the real plugin store.
func newRPEnv(t *testing.T) *runEnv {
	t.Helper()
	r := newRunEnv(t)
	r.svc.SetPluginCatalog(r.svc)
	return r
}

// offer adds plugin versions to the store; fields are extra manifest fields (requires is empty unless given).
func (r *runEnv) offer(id, version string, fields map[string]any) {
	r.t.Helper()
	f := map[string]any{"requires": map[string]string{}}
	for k, v := range fields {
		f[k] = v
	}
	data := hubtest.PluginZipWith(r.t, id, version, nil, f, nil)
	st, err := r.svc.StagePluginUpload(context.Background(), bytes.NewReader(data))
	if err != nil {
		r.t.Fatal(err)
	}
	if _, err := r.svc.OfferStagedPlugin(context.Background(), st.StageID, nil, r.admin.ID); err != nil {
		r.t.Fatal(err)
	}
}

func (r *runEnv) versions(id string) string {
	r.t.Helper()
	ps, err := r.svc.ListPlugins(context.Background())
	if err != nil {
		r.t.Fatal(err)
	}
	var vs []string
	for _, p := range ps {
		if p.PluginID == id {
			vs = append(vs, p.Version)
		}
	}
	return strings.Join(vs, ",")
}

func (r *runEnv) createWith(d dev, name string, plugins ...[2]string) rpResp {
	r.t.Helper()
	var ps []any
	for _, p := range plugins {
		ps = append(ps, map[string]any{"plugin_id": p[0], "version": p[1]})
	}
	rec := r.api(d, "POST", "/api/v1/runs", map[string]any{"name": name, "plugins": ps, "game_id": r.game.ID, "invite_user_ids": []string{}})
	wantStatus(r.t, rec, 201, "")
	return decode[rpResp](r.t, rec)
}

func rpBody(plugins ...[2]string) map[string]any {
	ps := []any{}
	for _, p := range plugins {
		ps = append(ps, map[string]any{"plugin_id": p[0], "version": p[1]})
	}
	return map[string]any{"plugins": ps}
}

func (r *runEnv) putPlugins(d dev, id string, body any) *httptest.ResponseRecorder {
	r.t.Helper()
	return r.api(d, "PUT", "/api/v1/runs/"+id+"/plugins", body)
}

func TestRunPluginsAuthzAndValidation(t *testing.T) {
	r := newRPEnv(t)
	anna, bob := r.device(r.anna.ID, "Annas PC"), r.device(r.bob.ID, "Bobs PC")
	carol, admin := r.device(r.carol.ID, "Carols PC"), r.device(r.admin.ID, "Admin PC")
	stranger := r.device(r.dave.ID, "Daves PC")
	r.offer(pluginA, "1.0.0", nil)
	run := r.createWith(anna, "Upd", [2]string{pluginA, "1.0.0"})
	id := run.RunID
	// Invite and join two more people.
	wantStatus(t, r.post(anna, id, "invite", map[string]any{"user_ids": []string{r.bob.ID, r.carol.ID}}), 200, "")
	r.mustJoin(bob, id)
	r.offer(pluginA, "1.1.0", nil)
	to := func(v string) map[string]any { return rpBody([2]string{pluginA, v}) }

	wantStatus(t, r.do("PUT", "/api/v1/runs/"+id+"/plugins", to("1.1.0"), opt{}), 401, "")
	wantStatus(t, r.putPlugins(bob, id, to("1.1.0")), 403, "forbidden")   // joined, not the creator
	wantStatus(t, r.putPlugins(carol, id, to("1.1.0")), 403, "forbidden") // invited
	wantStatus(t, r.putPlugins(stranger, id, to("1.1.0")), 404, "not_found")
	wantStatus(t, r.putPlugins(anna, uuid.NewString(), to("1.1.0")), 404, "not_found")
	if got := r.get(anna, id); got.Plugins[0].Version != "1.0.0" {
		t.Fatalf("a refused change applied: %+v", got.Plugins)
	}

	// Bad requests: not offered, listed twice, bad reference, empty list, no body.
	wantStatus(t, r.putPlugins(anna, id, to("9.9.9")), 400, "bad_request")
	wantStatus(t, r.putPlugins(anna, id, rpBody([2]string{pluginC, "1.0.0"})), 400, "bad_request")
	wantStatus(t, r.putPlugins(anna, id, rpBody([2]string{pluginA, "1.1.0"}, [2]string{pluginA, "1.1.0"})), 400, "bad_request")
	wantStatus(t, r.do("PUT", "/api/v1/runs/"+id+"/plugins", rpBody([2]string{"BAD ID", "1.1.0"}), opt{token: anna.tok, raw: true}), 400, "bad_request")
	wantStatus(t, r.do("PUT", "/api/v1/runs/"+id+"/plugins", rpBody(), opt{token: anna.tok, raw: true}), 400, "bad_request")
	wantStatus(t, r.do("PUT", "/api/v1/runs/"+id+"/plugins", nil, opt{token: anna.tok, raw: true}), 400, "bad_request")

	// The creator may, then an admin who is not part of the run.
	rec := r.putPlugins(anna, id, to("1.1.0"))
	wantStatus(t, rec, 200, "")
	if got := decode[rpResp](t, rec); got.RunID != id || got.pins() != pluginA+"@1.1.0(was 1.0.0)" {
		t.Fatalf("%s", got.pins())
	}
	r.offer(pluginA, "1.2.0", nil) // 1.1.0 is pinned and stays
	wantStatus(t, r.putPlugins(admin, id, to("1.2.0")), 200, "")
	if got := r.get(bob, id); got.Plugins[0].Version != "1.2.0" {
		t.Fatalf("%+v", got.Plugins)
	}

	// A run that is over cannot be updated.
	r.offer(pluginA, "1.3.0", nil)
	wantStatus(t, r.status(anna, id, "finished"), 200, "")
	wantStatus(t, r.putPlugins(anna, id, to("1.3.0")), 409, "conflict")
	wantStatus(t, r.putPlugins(admin, id, to("1.3.0")), 409, "conflict")
}

func TestRunPluginsUpgradeEventAndPush(t *testing.T) {
	r := newRPEnv(t)
	anna, bob := r.device(r.anna.ID, "Annas PC"), r.device(r.bob.ID, "Bobs PC")
	carol, dave := r.device(r.carol.ID, "Carols PC"), r.device(r.dave.ID, "Daves PC")
	r.offer(pluginA, "1.0.0", nil)
	run := r.createWith(anna, "Live", [2]string{pluginA, "1.0.0"})
	id := run.RunID
	wantStatus(t, r.post(anna, id, "invite", map[string]any{"user_ids": []string{r.bob.ID, r.carol.ID}}), 200, "")
	r.mustJoin(bob, id)
	r.offer(pluginA, "2.0.0", nil)
	wa, wb, wc, wd := r.dial(anna), r.dial(bob), r.dial(carol), r.dial(dave)
	for _, w := range []*wsClient{wa, wb, wc, wd} {
		w.settle()
	}
	last := r.get(anna, id).LastSeq

	sha2 := ""
	if p, err := r.svc.GetPlugin(context.Background(), pluginA, "2.0.0"); err == nil {
		sha2 = p.SHA256
	} else {
		t.Fatal(err)
	}
	rec := r.putPlugins(anna, id, rpBody([2]string{pluginA, "2.0.0"}))
	wantStatus(t, rec, 200, "")
	got := decode[rpResp](t, rec)
	if got.pins() != pluginA+"@2.0.0(was 1.0.0)" || got.Plugins[0].SHA256 != sha2 {
		t.Fatalf("%s %s", got.pins(), got.Plugins[0].SHA256)
	}

	for _, w := range []*wsClient{wa, wb, wc} {
		w.awaitWhere("run_updated", func(m wsMsg) bool {
			var p struct{ Run rpResp }
			json.Unmarshal(m.Payload, &p)
			return p.Run.RunID == id && p.Run.pins() == pluginA+"@2.0.0(was 1.0.0)"
		})
	}
	for _, w := range []*wsClient{wa, wb} {
		_, e := runEventOf(w.awaitWhere("run_event", isRunEvent(id, "plugins.updated")))
		if e.Source != "hub" || e.UserID != nil || e.PluginID != nil || e.Seq != last+1 {
			t.Fatalf("%+v", e)
		}
		var p changesPayload
		if err := json.Unmarshal(e.Payload, &p); err != nil || len(p.Changes) != 1 || p.Changes[0].PluginID != pluginA ||
			p.Changes[0].From != "1.0.0" || p.Changes[0].To != "2.0.0" || p.By != r.anna.ID || p.ByName != "Anna" {
			t.Fatalf("payload %s: %+v %v", e.Payload, p, err)
		}
	}
	wc.quiet("run_event")
	wd.quiet("run_updated", "run_event")
	if evs := r.events(anna, id, last); len(evs) != 1 || evs[0].Type != "plugins.updated" {
		t.Fatalf("%+v", evs)
	}

	// The same version again writes nothing: no event, no push.
	wantStatus(t, r.putPlugins(anna, id, rpBody([2]string{pluginA, "2.0.0"})), 200, "")
	if evs := r.events(anna, id, last); len(evs) != 1 {
		t.Fatalf("a no-op wrote an event: %+v", evs)
	}
	wb.quiet("run_event", "run_updated")
	if got := decode[rpResp](t, r.api(anna, "GET", "/api/v1/runs/"+id, nil)); got.Plugins[0].PreviousVersion == nil || *got.Plugins[0].PreviousVersion != "1.0.0" {
		t.Fatalf("a no-op changed previous_version: %+v", got.Plugins)
	}
}

func TestRunPluginsUpdateFromAndDowngrade(t *testing.T) {
	r := newRPEnv(t)
	anna := r.device(r.anna.ID, "Annas PC")
	r.offer(pluginA, "1.0.0", nil)
	run := r.createWith(anna, "Compat", [2]string{pluginA, "1.0.0"})
	id := run.RunID
	r.offer(pluginA, "2.0.0", map[string]any{"run_update_from": "1.5.0"})
	r.offer(pluginA, "1.6.0", nil) // older than the newest: nothing is pruned
	to := func(v string) map[string]any { return rpBody([2]string{pluginA, v}) }

	// 1.0.0 is below run_update_from 1.5.0: blocked, naming plugin, pinned and minimum version.
	rec := r.putPlugins(anna, id, to("2.0.0"))
	wantStatus(t, rec, 409, "plugin_update_incompatible")
	for _, want := range []string{pluginA, "2.0.0", "1.5.0", "1.0.0"} {
		if !strings.Contains(rec.Body.String(), want) {
			t.Fatalf("message lacks %q: %s", want, rec.Body.String())
		}
	}
	if got := r.get(anna, id); got.Plugins[0].Version != "1.0.0" {
		t.Fatalf("%+v", got.Plugins)
	}
	// Via 1.6.0 (no run_update_from) the run reaches the bridge version, then 2.0.0 is allowed.
	wantStatus(t, r.putPlugins(anna, id, to("1.6.0")), 200, "")
	wantStatus(t, r.putPlugins(anna, id, to("2.0.0")), 200, "")
	r.offer(pluginA, "3.0.0", map[string]any{"run_update_from": "1.5.0"}) // 1.6.0 is previous, 2.0.0 pinned: both stay
	got := decode[rpResp](t, r.putPlugins(anna, id, to("3.0.0")))
	if got.pins() != pluginA+"@3.0.0(was 2.0.0)" {
		t.Fatalf("%s", got.pins())
	}

	// Downgrade: only to the previous version.
	wantStatus(t, r.putPlugins(anna, id, to("1.6.0")), 409, "plugin_update_incompatible")
	got = decode[rpResp](t, r.putPlugins(anna, id, to("2.0.0")))
	if got.pins() != pluginA+"@2.0.0(was 3.0.0)" {
		t.Fatalf("%s", got.pins())
	}
	// And forward again (the version restored from is now the previous one).
	got = decode[rpResp](t, r.putPlugins(anna, id, to("3.0.0")))
	if got.pins() != pluginA+"@3.0.0(was 2.0.0)" {
		t.Fatalf("%s", got.pins())
	}
}

func TestRunPluginsRequires(t *testing.T) {
	r := newRPEnv(t)
	anna := r.device(r.anna.ID, "Annas PC")
	req := func(rng string) map[string]any { return map[string]any{"requires": map[string]string{pluginA: rng}} }
	r.offer(pluginA, "1.0.0", nil)
	r.offer(pluginB, "1.0.0", req("^1.0"))
	r.offer(pluginC, "1.0.0", nil)

	// Appending: the run has only B, which requires A. A may be appended; C (required by nobody) may not.
	run := r.createWith(anna, "Req", [2]string{pluginB, "1.0.0"})
	id := run.RunID
	wantStatus(t, r.putPlugins(anna, id, rpBody([2]string{pluginC, "1.0.0"})), 400, "bad_request")
	if got := r.get(anna, id); len(got.Plugins) != 1 {
		t.Fatalf("%+v", got.Plugins)
	}
	rec := r.putPlugins(anna, id, rpBody([2]string{pluginA, "1.0.0"}))
	wantStatus(t, rec, 200, "")
	if got := decode[rpResp](t, rec); got.pins() != pluginB+"@1.0.0,"+pluginA+"@1.0.0" {
		t.Fatalf("%s", got.pins())
	}
	evs := r.events(anna, id, 0)
	var p changesPayload
	last := evs[len(evs)-1]
	if last.Type != "plugins.updated" || json.Unmarshal(last.Payload, &p) != nil || len(p.Changes) != 1 || p.Changes[0].From != "" || p.Changes[0].To != "1.0.0" {
		t.Fatalf("%+v %s", last, last.Payload)
	}

	// Switching A to 2.0.0 breaks B's ^1.0: plugin_requires_missing, naming plugin and requirement; nothing changes.
	r.offer(pluginA, "2.0.0", nil)
	rec = r.putPlugins(anna, id, rpBody([2]string{pluginA, "2.0.0"}))
	wantStatus(t, rec, 409, "plugin_requires_missing")
	for _, want := range []string{pluginB, pluginA, "^1.0"} {
		if !strings.Contains(rec.Body.String(), want) {
			t.Fatalf("message lacks %q: %s", want, rec.Body.String())
		}
	}
	// Together with a B that accepts ^2.0 it works.
	r.offer(pluginB, "1.1.0", req("^2.0"))
	rec = r.putPlugins(anna, id, rpBody([2]string{pluginA, "2.0.0"}, [2]string{pluginB, "1.1.0"}))
	wantStatus(t, rec, 200, "")
	if got := decode[rpResp](t, rec); got.pins() != pluginB+"@1.1.0(was 1.0.0),"+pluginA+"@2.0.0(was 1.0.0)" {
		t.Fatalf("%s", got.pins())
	}

	// A run created without the required plugin: updating B alone is refused as well (A missing).
	r.offer(pluginB, "1.2.0", req("^2.0"))
	run2 := r.createWith(anna, "NoA", [2]string{pluginB, "1.1.0"})
	wantStatus(t, r.putPlugins(anna, run2.RunID, rpBody([2]string{pluginB, "1.2.0"})), 409, "plugin_requires_missing")
}

func TestRunPluginsPinnedVersionsKeptUntilRunEnds(t *testing.T) {
	r := newRPEnv(t)
	anna := r.device(r.anna.ID, "Annas PC")
	r.offer(pluginA, "1.0.0", nil)
	run := r.createWith(anna, "Keep", [2]string{pluginA, "1.0.0"})
	id := run.RunID
	r.offer(pluginA, "2.0.0", nil) // supersedes, but the run pins 1.0.0
	if got := r.versions(pluginA); got != "1.0.0,2.0.0" {
		t.Fatalf("%s", got)
	}
	wantStatus(t, r.putPlugins(anna, id, rpBody([2]string{pluginA, "2.0.0"})), 200, "")
	// 1.0.0 is now the previous version: still in use, not removable, not pruned by a newer upload.
	ctx := context.Background()
	if used, err := r.svc.PluginVersionInUse(ctx, pluginA, "1.0.0"); err != nil || !used {
		t.Fatalf("previous version not in use: %v %v", used, err)
	}
	if err := r.svc.RemovePlugin(ctx, pluginA, "1.0.0"); err == nil {
		t.Fatal("removed the previous version of an unfinished run")
	}
	r.offer(pluginA, "3.0.0", nil)
	if got := r.versions(pluginA); got != "1.0.0,2.0.0,3.0.0" {
		t.Fatalf("pruned: %s", got)
	}
	// Restoring still works after the newer upload.
	wantStatus(t, r.putPlugins(anna, id, rpBody([2]string{pluginA, "1.0.0"})), 200, "")

	// When the run ends, both versions become prunable.
	wantStatus(t, r.status(anna, id, "finished"), 200, "")
	r.svc.PruneOldPlugins(ctx)
	if got := r.versions(pluginA); got != "3.0.0" {
		t.Fatalf("after the run: %s", got)
	}
}
