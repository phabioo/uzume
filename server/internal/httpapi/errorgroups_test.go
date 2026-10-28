package httpapi

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/phabioo/uzume/server/internal/errfp"
	"github.com/phabioo/uzume/server/internal/hub"
	"github.com/phabioo/uzume/server/internal/hub/hubtest"
)

// Error events and error groups over HTTP (feature reports_v2, ADR 0035); every request is validated against the
// OpenAPI spec. Only placeholder data: hub.example.org, 192.0.2.x, 203.0.113.x.

type evT = map[string]any

func errEvent(kind, component, message, version string) evT {
	return evT{"kind": kind, "component": component, "fingerprint": errfp.Fingerprint(kind, component, message), "title": "Error in " + component,
		"message": message, "app_version": version, "os": "Linux", "occurred_at": time.Now().UTC().Format(time.RFC3339)}
}

type groupT struct {
	Fingerprint, Kind, Component, Title string
	Count                               int64
	Devices                             int
	FirstSeen                           string `json:"first_seen"`
	LastSeen                            string `json:"last_seen"`
	Versions                            []string
	IssueURL                            *string `json:"issue_url"`
	Seen                                bool
	IssuesBase                          *string `json:"issues_base"`
	// detail
	Events []struct {
		AppVersion string `json:"app_version"`
		DeviceName string `json:"device_name"`
		UserName   string `json:"user_name"`
		Message    string
	}
	PublicTitle   string `json:"public_title"`
	PublicSummary string `json:"public_summary"`
}

func (s *sessEnv) listGroups(tok string) (gs []groupT, newN int) {
	s.t.Helper()
	rec := s.do("GET", "/api/v1/admin/reports/errors", nil, opt{token: tok})
	wantStatus(s.t, rec, 200, "")
	out := decode[struct {
		Groups []groupT
		New    int
	}](s.t, rec)
	return out.Groups, out.New
}

func (s *sessEnv) sendEvents(tok string, evs ...evT) (accepted, dropped int, code int) {
	s.t.Helper()
	rec := s.do("POST", "/api/v1/reports/errors", evT{"events": evs}, opt{token: tok})
	if rec.Code != 202 {
		return 0, 0, rec.Code
	}
	r := decode[struct{ Accepted, Dropped int }](s.t, rec)
	return r.Accepted, r.Dropped, rec.Code
}

func TestErrorGroupsLifecycle(t *testing.T) {
	s := newSessEnv(t, nil)
	admin := s.login("00000000-0000-4000-8000-000000000001")
	anna := s.device(s.anna.ID, "Annas Laptop")
	bob := s.device(s.bob.ID, "Bobs PC")

	// Access: no token 401; a user is 403 on the admin endpoints but may send events.
	wantStatus(t, s.do("POST", "/api/v1/reports/errors", evT{"events": []evT{errEvent("app_crash", "app", "x", "0.24.0")}}, opt{}), 401, "")
	for _, c := range [][2]string{{"GET", "/api/v1/admin/reports/errors"}, {"GET", "/api/v1/admin/reports/errors/uz-000000000000"},
		{"PATCH", "/api/v1/admin/reports/errors/uz-000000000000"}, {"DELETE", "/api/v1/admin/reports/errors/uz-000000000000"}} {
		var body any
		if c[0] == "PATCH" {
			body = evT{"seen": true}
		}
		wantStatus(t, s.do(c[0], c[1], body, opt{token: anna.tok}), 403, "forbidden")
		wantStatus(t, s.do(c[0], c[1], body, opt{}), 401, "")
	}

	// Two devices send the same problem (numbers differ), one sends another.
	msg := "Cannot reach hub.example.org:8443 (192.0.2.7) for /home/anna/Games/Demo Homebrew.nds after 12 tries"
	e1 := errEvent("sync_failure", "app", msg, "0.24.0")
	e2 := errEvent("sync_failure", "app", strings.Replace(msg, "12", "99", 1), "0.23.1")
	if e1["fingerprint"] != e2["fingerprint"] {
		t.Fatal("same problem, same fingerprint expected")
	}
	if a, d, c := s.sendEvents(anna.tok, e1); a != 1 || d != 0 || c != 202 {
		t.Fatalf("%d %d %d", a, d, c)
	}
	if a, d, _ := s.sendEvents(bob.tok, e2, errEvent("core_failure", "core:azahar", "load failed", "0.24.0")); a != 2 || d != 0 {
		t.Fatalf("%d %d", a, d)
	}

	gs, newN := s.listGroups(admin)
	if len(gs) != 2 || newN != 2 {
		t.Fatalf("groups %+v new %d", gs, newN)
	}
	var sync, core groupT
	for _, g := range gs {
		if g.Kind == "sync_failure" {
			sync = g
		} else {
			core = g
		}
	}
	if sync.Count != 2 || sync.Devices != 2 || sync.Seen || sync.IssueURL != nil || sync.Component != "app" ||
		strings.Join(sync.Versions, ",") != "0.23.1,0.24.0" || sync.IssuesBase == nil || *sync.IssuesBase != "https://github.com/phabioo/uzume" {
		t.Fatalf("sync group %+v", sync)
	}
	if core.IssuesBase == nil || *core.IssuesBase != "https://github.com/phabioo/azahar" {
		t.Fatalf("azahar base %+v", core)
	}

	// Detail: newest events first, admin sees the names, the public text has none of the private data.
	rec := s.do("GET", "/api/v1/admin/reports/errors/"+sync.Fingerprint, nil, opt{token: admin})
	wantStatus(t, rec, 200, "")
	d := decode[groupT](t, rec)
	if len(d.Events) != 2 || d.Events[0].DeviceName != "Bobs PC" || d.Events[0].UserName != "Bob" || d.Events[1].UserName != "Anna" ||
		!strings.Contains(d.Events[0].Message, "hub.example.org") {
		t.Fatalf("events %+v", d.Events)
	}
	pub := d.PublicTitle + "\n" + d.PublicSummary
	for _, bad := range []string{"hub.example.org", "192.0.2.7", "8443", "/home/anna", "Demo Homebrew", "Anna", "Bob"} {
		if strings.Contains(pub, bad) {
			t.Errorf("public text leaks %q: %s", bad, pub)
		}
	}
	if !strings.Contains(d.PublicSummary, "<host>") || !strings.Contains(d.PublicSummary, "<ip>") || !strings.Contains(d.PublicSummary, "<game>") ||
		!strings.Contains(d.PublicSummary, "Seen 2 times on 2 devices, versions 0.23.1, 0.24.0.") {
		t.Fatalf("summary %q", d.PublicSummary)
	}
	if g, n := s.listGroups(admin); n != 2 || g[0].Seen { // reading does not mark seen
		t.Fatal("reading must not mark seen")
	}

	// PATCH: seen, then a new event makes it unseen again; issue_url set/forget/validate.
	rec = s.do("PATCH", "/api/v1/admin/reports/errors/"+sync.Fingerprint, evT{"seen": true}, opt{token: admin})
	wantStatus(t, rec, 200, "")
	if g := decode[groupT](t, rec); !g.Seen {
		t.Fatalf("%+v", g)
	}
	if _, n := s.listGroups(admin); n != 1 {
		t.Fatalf("new %d, want 1", n)
	}
	s.clk.Advance(time.Minute)
	s.sendEvents(anna.tok, e1)
	if gs, n := s.listGroups(admin); n != 2 || gs[0].Seen && gs[1].Seen {
		t.Fatalf("a new event must reopen the group: %+v new %d", gs, n)
	}
	const issue = "https://github.com/phabioo/uzume/issues/123"
	rec = s.do("PATCH", "/api/v1/admin/reports/errors/"+sync.Fingerprint, evT{"issue_url": issue}, opt{token: admin})
	wantStatus(t, rec, 200, "")
	if g := decode[groupT](t, rec); g.IssueURL == nil || *g.IssueURL != issue || g.Seen {
		t.Fatalf("issue only: %+v (must not mark seen)", g)
	}
	for _, bad := range []any{"http://github.com/phabioo/uzume/issues/1", "https://github.com/phabioo/uzume/pull/1", "https://github.com/phabioo/uzume/issues/x",
		"https://gitlab.com/phabioo/uzume/issues/1", "https://github.com/phabioo/uzume/issues/1/", "https://github.com/phabioo/uzume", 5} {
		wantStatus(t, s.do("PATCH", "/api/v1/admin/reports/errors/"+sync.Fingerprint, evT{"issue_url": bad}, opt{token: admin, raw: true}), 400, "bad_request")
	}
	wantStatus(t, s.do("PATCH", "/api/v1/admin/reports/errors/"+sync.Fingerprint, evT{}, opt{token: admin, raw: true}), 400, "bad_request")
	wantStatus(t, s.do("PATCH", "/api/v1/admin/reports/errors/"+sync.Fingerprint, evT{"seen": false}, opt{token: admin, raw: true}), 400, "bad_request")
	rec = s.do("PATCH", "/api/v1/admin/reports/errors/"+sync.Fingerprint, evT{"issue_url": nil, "seen": true}, opt{token: admin})
	wantStatus(t, rec, 200, "")
	if g := decode[groupT](t, rec); g.IssueURL != nil || !g.Seen {
		t.Fatalf("forget: %+v", g)
	}
	wantStatus(t, s.do("PATCH", "/api/v1/admin/reports/errors/uz-ffffffffffff", evT{"seen": true}, opt{token: admin}), 404, "not_found")

	// DELETE
	wantStatus(t, s.do("DELETE", "/api/v1/admin/reports/errors/"+sync.Fingerprint, nil, opt{token: admin}), 204, "")
	wantStatus(t, s.do("DELETE", "/api/v1/admin/reports/errors/"+sync.Fingerprint, nil, opt{token: admin}), 404, "not_found")
	wantStatus(t, s.do("GET", "/api/v1/admin/reports/errors/"+sync.Fingerprint, nil, opt{token: admin}), 404, "not_found")
	wantStatus(t, s.do("GET", "/api/v1/admin/reports/errors/not-a-fingerprint", nil, opt{token: admin, raw: true}), 404, "")
	if gs, _ := s.listGroups(admin); len(gs) != 1 {
		t.Fatalf("%+v", gs)
	}
	// The old report routes still work next to the new ones.
	wantStatus(t, s.do("GET", "/api/v1/admin/reports", nil, opt{token: admin}), 200, "")
	wantStatus(t, s.do("GET", "/api/v1/admin/reports/1/archive", nil, opt{token: admin}), 404, "")
}

func TestErrorEventsValidation(t *testing.T) {
	s := newSessEnv(t, nil)
	tok := s.device(s.anna.ID, "Annas Laptop").tok
	ok := errEvent("app_crash", "app", "boom", "0.24.0")
	with := func(k string, v any) evT {
		e := evT{}
		for a, b := range ok {
			e[a] = b
		}
		e[k] = v
		return e
	}
	long := func(n int) string { return strings.Repeat("x", n) }
	bad := map[string]any{
		"no events": []evT{},
		"21 events": func() []evT {
			var l []evT
			for i := 0; i < 21; i++ {
				l = append(l, ok)
			}
			return l
		}(),
		"unknown kind":       []evT{with("kind", "other")},
		"bad component":      []evT{with("component", "Core:azahar")},
		"component empty id": []evT{with("component", "plugin:")},
		"component long":     []evT{with("component", "plugin:"+long(81))},
		"bad fingerprint":    []evT{with("fingerprint", "uz-0123456789AB")},
		"short fingerprint":  []evT{with("fingerprint", "uz-0123")},
		"empty title":        []evT{with("title", "")},
		"two-line title":     []evT{with("title", "a\nb")},
		"long title":         []evT{with("title", long(201))},
		"long message":       []evT{with("message", long(2001))},
		"long version":       []evT{with("app_version", long(41))},
		"long os":            []evT{with("os", long(81))},
		"bad time":           []evT{with("occurred_at", "yesterday")},
		"one bad of two":     []evT{ok, with("kind", "nope")},
	}
	for name, evs := range bad {
		rec := s.do("POST", "/api/v1/reports/errors", evT{"events": evs}, opt{token: tok, raw: true})
		if rec.Code != 400 {
			t.Errorf("%s: status %d: %s", name, rec.Code, rec.Body.String())
		}
	}
	wantStatus(t, s.do("POST", "/api/v1/reports/errors", nil, opt{token: tok, raw: true, data: []byte("{nope"), ctype: "application/json"}), 400, "bad_request")
	// Nothing was stored by the refused requests (a refused batch is all or nothing).
	admin := s.login("00000000-0000-4000-8000-000000000001")
	if gs, _ := s.listGroups(admin); len(gs) != 0 {
		t.Fatalf("stored %+v", gs)
	}
	// The limits themselves are accepted.
	edge := evT{"kind": "plugin_error", "component": "plugin:" + long(80), "fingerprint": "uz-0123456789ab", "title": long(200), "message": long(2000),
		"app_version": long(40), "os": long(80), "occurred_at": time.Now().UTC().Format(time.RFC3339)}
	if a, _, c := s.sendEvents(tok, edge); a != 1 || c != 202 {
		t.Fatalf("edge: %d %d", a, c)
	}
}

func TestErrorEventsRedactSecretsAndRatePerDevice(t *testing.T) {
	s := newSessEnv(t, nil)
	anna, annaCred := s.deviceWithCred(s.anna.ID)
	bob := s.device(s.bob.ID, "Bobs PC")
	admin := s.login("00000000-0000-4000-8000-000000000001")

	e := errEvent("sync_failure", "app", "request failed: Authorization: Bearer abcdefghijklmnop password=hunter2hunter2", "0.24.0")
	e["title"] = "Sync failed token=fba_secretsecretsecret"
	s.sendEvents(anna.tok, e)
	rec := s.do("GET", "/api/v1/admin/reports/errors/"+e["fingerprint"].(string), nil, opt{token: admin})
	body := rec.Body.String()
	if strings.Contains(body, "hunter2hunter2") || strings.Contains(body, "abcdefghijklmnop") || strings.Contains(body, "secretsecretsecret") || !strings.Contains(body, "[redacted]") {
		t.Fatalf("secrets stored: %s", body)
	}

	// 20 per device and hour: the surplus is dropped and counted, never an error. The first event counted above.
	var batch []evT
	for i := 0; i < 20; i++ {
		batch = append(batch, errEvent("app_crash", "app", fmt.Sprintf("crash kind %c", 'a'+i), "0.24.0"))
	}
	if a, d, c := s.sendEvents(anna.tok, batch...); a != 19 || d != 1 || c != 202 {
		t.Fatalf("accepted %d dropped %d status %d, want 19/1/202", a, d, c)
	}
	if a, d, c := s.sendEvents(anna.tok, batch[0]); a != 0 || d != 1 || c != 202 {
		t.Fatalf("over the limit: %d %d %d", a, d, c)
	}
	if a, d, _ := s.sendEvents(bob.tok, batch[0], batch[1]); a != 2 || d != 0 { // another device is not affected
		t.Fatalf("other device: %d %d", a, d)
	}
	gs, _ := s.listGroups(admin)
	for _, g := range gs {
		if g.Fingerprint == batch[19]["fingerprint"] {
			t.Fatal("the dropped event must not be stored")
		}
	}
	s.clk.Advance(61 * time.Minute) // the window is over (access tokens expire too: issue a new one)
	tok, err := s.svc.IssueAccessToken(context.Background(), anna.id, annaCred)
	if err != nil {
		t.Fatal(err)
	}
	if a, d, _ := s.sendEvents(tok.Token, batch[19]); a != 1 || d != 0 {
		t.Fatalf("after the hour: %d %d", a, d)
	}
}

func TestErrorGroupKeepsNewest200Events(t *testing.T) {
	s := newSessEnv(t, nil)
	admin := s.login("00000000-0000-4000-8000-000000000001")
	// Several devices, each within its own limit: 11 devices x 20 events = 220 events of one group.
	var fp string
	for d := 0; d < 11; d++ {
		tok := s.device(s.anna.ID, fmt.Sprintf("Device %d", d)).tok
		var batch []evT
		for i := 0; i < 20; i++ {
			e := errEvent("app_crash", "app", "same crash", fmt.Sprintf("0.24.%d", d))
			e["message"] = fmt.Sprintf("device %d event %d", d, i)
			fp = e["fingerprint"].(string)
			batch = append(batch, e)
		}
		if a, _, _ := s.sendEvents(tok, batch...); a != 20 {
			t.Fatalf("accepted %d", a)
		}
	}
	gs, _ := s.listGroups(admin)
	if len(gs) != 1 || gs[0].Count != 220 || gs[0].Devices != 11 || len(gs[0].Versions) != 10 || gs[0].Versions[0] != "0.24.10" {
		t.Fatalf("%+v", gs)
	}
	rec := s.do("GET", "/api/v1/admin/reports/errors/"+fp, nil, opt{token: admin})
	d := decode[groupT](t, rec)
	if len(d.Events) != 20 || d.Events[0].Message != "device 10 event 19" {
		t.Fatalf("detail events %d first %q", len(d.Events), d.Events[0].Message)
	}
}

func TestErrorGroupIssuesBase(t *testing.T) {
	s := newSessEnv(t, func(o *hub.Options) { o.CoreTrustKeys = hubtest.PluginKeys() })
	admin := s.login("00000000-0000-4000-8000-000000000001")
	tok := s.device(s.anna.ID, "Annas Laptop").tok
	ctx := context.Background()
	offer := func(id, version string, fields map[string]any) {
		t.Helper()
		st, err := s.svc.StagePluginUpload(ctx, bytes.NewReader(hubtest.PluginZipWith(t, id, version, nil, fields, nil)))
		if err != nil {
			t.Fatal(err)
		}
		if _, err := s.svc.OfferStagedPlugin(ctx, st.StageID, st.Permissions, s.admin.ID); err != nil {
			t.Fatal(err)
		}
	}
	offer("net.example.withissues", "1.0.0", map[string]any{"issues_url": "https://github.com/example-org/old-repo"})
	offer("net.example.withissues", "1.1.0", map[string]any{"issues_url": "https://github.com/example-org/withissues"})
	offer("net.example.noissues", "1.0.0", nil)
	offer("net.example.badissues", "1.0.0", map[string]any{"issues_url": "https://gitlab.com/example-org/badissues"})

	comps := []string{"app", "server", "core:azahar", "core:melondsds", "plugin:net.example.withissues", "plugin:net.example.noissues",
		"plugin:net.example.badissues", "plugin:net.example.unknown"}
	want := map[string]string{"app": "https://github.com/phabioo/uzume", "server": "https://github.com/phabioo/uzume",
		"core:azahar": "https://github.com/phabioo/azahar", "plugin:net.example.withissues": "https://github.com/example-org/withissues"}
	for _, c := range comps {
		kind := "plugin_error"
		if c == "app" {
			kind = "app_crash"
		}
		s.sendEvents(tok, errEvent(kind, c, "boom "+c, "0.24.0"))
	}
	gs, _ := s.listGroups(admin)
	if len(gs) != len(comps) {
		t.Fatalf("%d groups", len(gs))
	}
	for _, g := range gs {
		w, has := want[g.Component]
		switch {
		case has && (g.IssuesBase == nil || *g.IssuesBase != w):
			t.Errorf("%s: issues_base %v, want %s", g.Component, g.IssuesBase, w)
		case !has && g.IssuesBase != nil:
			t.Errorf("%s: issues_base %s, want null", g.Component, *g.IssuesBase)
		}
	}
}

func TestServerOwnErrorEvents(t *testing.T) {
	s := newSessEnv(t, nil)
	admin := s.login("00000000-0000-4000-8000-000000000001")
	ctx := context.Background()
	for i := 0; i < 25; i++ {
		s.svc.RecordServerError(ctx, hub.ErrKindServerError, "Internal error", fmt.Sprintf("GET /api/v1/x: failure %c", 'a'+i%26))
	}
	s.svc.RecordServerError(ctx, hub.ErrKindAppCrash, "not a server kind", "x") // refused kind
	gs, n := s.listGroups(admin)
	total := int64(0)
	for _, g := range gs {
		if g.Component != "server" || g.Kind != "server_error" || g.Devices != 1 {
			t.Fatalf("%+v", g)
		}
		total += g.Count
	}
	if total != 20 || n != len(gs) {
		t.Fatalf("recorded %d events in %d groups, want 20 (limit per hour)", total, len(gs))
	}
	s.clk.Advance(61 * time.Minute)
	admin = s.login("00000000-0000-4000-8000-000000000002")
	s.svc.RecordServerError(ctx, hub.ErrKindUpdateFailure, "Update to 0.24.1 failed", "Update to 0.24.1 failed: checksum mismatch")
	gs, _ = s.listGroups(admin)
	found := false
	for _, g := range gs {
		found = found || g.Kind == "update_failure"
	}
	if !found {
		t.Fatal("update_failure not recorded after the hour")
	}
}

func TestHandlerPanicAndInternalErrorAreRecorded(t *testing.T) {
	s := newSessEnv(t, nil)
	admin := s.login("00000000-0000-4000-8000-000000000001")
	srv := &Server{svc: s.svc, log: slog.New(slog.NewTextHandler(io.Discard, nil))}

	rec := httptest.NewRecorder()
	r := httptest.NewRequest("GET", "/api/v1/games/1234", nil)
	func() {
		defer srv.recoverHandler(rec, r)
		panic("index out of range [3] with length 2")
	}()
	wantStatus(t, rec, 500, "internal")

	rec = httptest.NewRecorder()
	srv.writeErr(rec, r, errors.New("database is locked"))
	wantStatus(t, rec, 500, "internal")
	rec = httptest.NewRecorder()
	srv.writeErr(rec, r, context.Canceled) // a client that went away is not recorded
	wantStatus(t, rec, 500, "internal")

	gs, _ := s.listGroups(admin)
	if len(gs) != 2 {
		t.Fatalf("%+v", gs)
	}
	for _, g := range gs {
		if g.Kind != "server_error" || g.Component != "server" || g.Count != 1 {
			t.Fatalf("%+v", g)
		}
	}
}

func TestFeatureReportsV2AdvertisedAndBadge(t *testing.T) {
	s := newSessEnv(t, nil)
	tok := s.device(s.anna.ID, "Annas Laptop").tok
	found := false
	for _, f := range hub.BaseFeatures() {
		found = found || f == "reports_v2"
	}
	if !found {
		t.Fatal("reports_v2 missing in BaseFeatures")
	}
	s.sendEvents(tok, errEvent("app_crash", "app", "boom", "0.24.0"))
	if n, err := s.svc.NewReportCount(context.Background()); err != nil || n != 1 {
		t.Fatalf("badge %d %v", n, err)
	}
}
