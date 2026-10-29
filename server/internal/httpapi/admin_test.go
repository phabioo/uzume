package httpapi

import (
	"context"
	"log/slog"
	"strings"
	"testing"
	"time"

	"github.com/google/uuid"

	"github.com/phabioo/uzume/server/internal/diag"
	"github.com/phabioo/uzume/server/internal/statusinfo"
)

func adminEnv(t *testing.T, ring *diag.ErrorRing, logRing *diag.Ring) (*env, string) {
	t.Helper()
	src := &statusinfo.Source{PublicHost: "hub.example.com", PublicPort: 8443, Errors: ring, Log: logRing}
	e := newEnvNoAdmin(t, nil, WithStatus(src))
	src.Svc = e.svc
	admin, err := e.svc.CreateAdmin(context.Background(), "admin", "secret-12345")
	if err != nil {
		t.Fatal(err)
	}
	e.admin = admin
	return e, e.login(uuid.NewString())
}

func TestAdminEndpointsAuth(t *testing.T) {
	e, _ := adminEnv(t, diag.NewErrorRing(5), diag.NewRing(5, 1024))
	anna, err := e.svc.CreateUser(context.Background(), "anna", "Anna")
	if err != nil {
		t.Fatal(err)
	}
	admin := e.admin
	e.admin = anna
	userTok := e.login(uuid.NewString())
	e.admin = admin

	calls := []struct {
		method, path string
		body         any
	}{
		{"GET", "/api/v1/admin/status", nil},
		{"GET", "/api/v1/admin/status/log", nil},
		{"GET", "/api/v1/admin/metrics", nil},
		{"PUT", "/api/v1/admin/metrics", map[string]any{"enabled": true}},
		{"POST", "/api/v1/admin/metrics/token", nil},
		{"GET", "/api/v1/admin/storage", nil},
		{"GET", "/api/v1/admin/storage/screenshots", nil},
		{"POST", "/api/v1/admin/storage/cleanup", map[string]any{"id": "old_snapshots"}},
	}
	for _, c := range calls {
		rec := e.do(c.method, c.path, c.body, opt{})
		wantStatus(t, rec, 401, "")
		rec = e.do(c.method, c.path, c.body, opt{token: "fba_wrongwrongwrongwrongwrong"})
		wantStatus(t, rec, 401, "")
		rec = e.do(c.method, c.path, c.body, opt{token: userTok})
		wantStatus(t, rec, 403, "")
		if errCode(t, rec) != "forbidden" {
			t.Fatalf("%s %s: %s", c.method, c.path, rec.Body.String())
		}
	}
	// A non-admin must not have switched anything on.
	if e.svc.MetricsEnabled(context.Background()) {
		t.Fatal("metrics switched on by a user")
	}
}

func TestAdminStatusShape(t *testing.T) {
	ring, logRing := diag.NewErrorRing(10), diag.NewRing(10, 4096)
	e, tok := adminEnv(t, ring, logRing)
	log := slog.New(diag.NewCaptureHandler(slog.NewTextHandler(logRing, nil), ring))
	log.Error("db slow", "token", "supersecretvalue")
	logRing.Write([]byte("level=INFO msg=x password=hunter2hunter2\n"))

	rec := e.do("GET", "/api/v1/admin/status", nil, opt{token: tok})
	wantStatus(t, rec, 200, "")
	st := decode[map[string]any](t, rec)
	for _, k := range []string{"hub_name", "hub_version", "uptime_seconds", "public_address", "time", "problems", "memory", "sessions", "viewers", "players", "errors", "metrics_enabled"} {
		if _, ok := st[k]; !ok {
			t.Fatalf("%q missing: %v", k, st)
		}
	}
	if st["public_address"] != "hub.example.com:8443" {
		t.Fatalf("public_address %v", st["public_address"])
	}
	if _, ok := st["turn"]; ok {
		t.Fatal("no TURN relay: the key is absent")
	}
	if _, ok := st["memory"].(map[string]any)["process_bytes"]; !ok {
		t.Fatalf("memory %v", st["memory"])
	}
	if arr, ok := st["sessions"].([]any); !ok || len(arr) != 0 {
		t.Fatalf("sessions %v", st["sessions"])
	}
	errs := st["errors"].([]any)
	if len(errs) != 1 {
		t.Fatalf("errors %v", errs)
	}
	ent := errs[0].(map[string]any)
	if ent["level"] != "error" || !strings.Contains(ent["message"].(string), "db slow") || strings.Contains(ent["message"].(string), "supersecretvalue") {
		t.Fatalf("error entry %v", ent)
	}
	if _, err := time.Parse(time.RFC3339, st["time"].(string)); err != nil {
		t.Fatal(err)
	}

	// The log download is redacted text.
	rec = e.do("GET", "/api/v1/admin/status/log", nil, opt{token: tok})
	wantStatus(t, rec, 200, "")
	if !strings.HasPrefix(rec.Header().Get("Content-Type"), "text/plain") || !strings.Contains(rec.Header().Get("Content-Disposition"), "attachment") {
		t.Fatalf("headers %v", rec.Header())
	}
	body := rec.Body.String()
	if !strings.Contains(body, "msg=x") || strings.Contains(body, "hunter2hunter2") || strings.Contains(body, "supersecretvalue") {
		t.Fatalf("log %q", body)
	}
}

func TestAdminMetricsSetting(t *testing.T) {
	e, tok := adminEnv(t, diag.NewErrorRing(5), diag.NewRing(5, 1024))
	m := decode[map[string]any](t, e.do("GET", "/api/v1/admin/metrics", nil, opt{token: tok}))
	if m["enabled"] != false || m["token_set"] != false || m["token"] != nil {
		t.Fatalf("default %v", m)
	}
	m = decode[map[string]any](t, e.do("PUT", "/api/v1/admin/metrics", map[string]any{"enabled": true}, opt{token: tok}))
	first, _ := m["token"].(string)
	if m["enabled"] != true || m["token_set"] != true || !strings.HasPrefix(first, "fbm_") {
		t.Fatalf("enable creates the token once: %v", m)
	}
	if !e.svc.CheckMetricsToken(context.Background(), first) {
		t.Fatal("token must work")
	}
	m = decode[map[string]any](t, e.do("GET", "/api/v1/admin/metrics", nil, opt{token: tok}))
	if m["enabled"] != true || m["token_set"] != true || m["token"] != nil {
		t.Fatalf("the token is never shown again: %v", m)
	}
	n := decode[map[string]any](t, e.do("POST", "/api/v1/admin/metrics/token", nil, opt{token: tok}))
	second, _ := n["token"].(string)
	if !strings.HasPrefix(second, "fbm_") || second == first || e.svc.CheckMetricsToken(context.Background(), first) || !e.svc.CheckMetricsToken(context.Background(), second) {
		t.Fatalf("regenerate: %v", n)
	}
	m = decode[map[string]any](t, e.do("PUT", "/api/v1/admin/metrics", map[string]any{"enabled": false}, opt{token: tok}))
	if m["enabled"] != false || m["token"] != nil || e.svc.CheckMetricsToken(context.Background(), second) {
		t.Fatalf("off: %v", m)
	}
}
