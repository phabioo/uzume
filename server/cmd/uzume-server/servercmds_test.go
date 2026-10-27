package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"flag"
	"log/slog"
	"net"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
	"time"

	"github.com/phabioo/uzume/server/internal/backup"
	"github.com/phabioo/uzume/server/internal/config"
	"github.com/phabioo/uzume/server/internal/hub"
	"github.com/phabioo/uzume/server/internal/tlsutil"
)

var setupCodeRe = regexp.MustCompile(`[0-9A-HJKMNP-TV-Z]{4}-[0-9A-HJKMNP-TV-Z]{2}`)

// testService opens the service of a data folder like the commands do; the caller closes it.
func testService(t *testing.T, dataDir string) (*hub.Service, func()) {
	t.Helper()
	fs := flag.NewFlagSet("test", flag.ContinueOnError)
	cfg := config.Register(fs, func(string) string { return "" })
	if err := fs.Parse([]string{"-data-dir", dataDir}); err != nil {
		t.Fatal(err)
	}
	if err := cfg.Validate(); err != nil {
		t.Fatal(err)
	}
	svc, closeFn, err := openService(context.Background(), cfg, false)
	if err != nil {
		t.Fatal(err)
	}
	return svc, closeFn
}

// newServerData creates a data folder with a database (and optionally the first admin "max").
func newServerData(t *testing.T, admin bool) string {
	t.Helper()
	dir := t.TempDir()
	svc, closeFn := testService(t, dir)
	defer closeFn()
	if admin {
		if _, err := svc.CreateAdmin(context.Background(), "max", "a-long-password-1"); err != nil {
			t.Fatal(err)
		}
	}
	return dir
}

func TestSetupCodeCommand(t *testing.T) {
	var out strings.Builder
	if err := runSetupCode([]string{"-data-dir", t.TempDir()}, &out); err == nil || !strings.Contains(err.Error(), "not been started") {
		t.Fatalf("no database: %v", err)
	}

	dir := newServerData(t, false)
	if _, err := tlsutil.EnsureSelfSigned(dir); err != nil {
		t.Fatal(err)
	}
	out.Reset()
	if err := runSetupCode([]string{"-data-dir", dir, "-listen", "192.0.2.10:8443"}, &out); err != nil {
		t.Fatal(err)
	}
	first := regexp.MustCompile(`Setup code: +(\S+)`).FindStringSubmatch(out.String())
	if first == nil || !setupCodeRe.MatchString(first[1]) || !strings.Contains(out.String(), "Security code:") || !strings.Contains(out.String(), "192.0.2.10:8443") ||
		!strings.Contains(out.String(), "Set up a server › On another device") ||
		!strings.Contains(out.String(), "The server is not running: it replaces this code when it starts.") {
		t.Fatalf("output: %s", out.String())
	}

	// With the server running (lock held) the note is absent.
	release, err := backup.LockDataDir(dir)
	if err != nil {
		t.Fatal(err)
	}
	out.Reset()
	if err := runSetupCode([]string{"-data-dir", dir}, &out); err != nil {
		t.Fatal(err)
	}
	release()
	if strings.Contains(out.String(), "not running") {
		t.Fatalf("output: %s", out.String())
	}

	// -json; a new code replaces the older one.
	out.Reset()
	if err := runSetupCode([]string{"-data-dir", dir, "-json"}, &out); err != nil {
		t.Fatal(err)
	}
	var j struct {
		SetupCode    string    `json:"setup_code"`
		ExpiresAt    time.Time `json:"expires_at"`
		ValidMinutes int       `json:"valid_minutes"`
		SecurityCode string    `json:"security_code"`
	}
	if err := json.Unmarshal([]byte(out.String()), &j); err != nil || !setupCodeRe.MatchString(j.SetupCode) || j.ValidMinutes != 60 || j.SecurityCode == "" {
		t.Fatalf("%v %s", err, out.String())
	}
	svc, closeFn := testService(t, dir)
	cur, _ := svc.CurrentSetupCode(context.Background())
	closeFn()
	if cur == nil || cur.Code != j.SetupCode {
		t.Fatalf("stored %+v, printed %s", cur, j.SetupCode)
	}

	// Once an admin exists the command refuses.
	if err := runSetupAdmin([]string{"-data-dir", dir, "-username", "max"}, strings.NewReader("a-long-password-1\n"), &out); err != nil {
		t.Fatal(err)
	}
	out.Reset()
	err = runSetupCode([]string{"-data-dir", dir}, &out)
	if err == nil || !strings.Contains(err.Error(), "This server is already set up") {
		t.Fatalf("set up: %v", err)
	}
}

func TestCodeCommand(t *testing.T) {
	dir := t.TempDir()
	var out strings.Builder
	if err := runCode([]string{"-data-dir", dir}, &out); !errors.Is(err, errNoCertificate) {
		t.Fatalf("no certificate: %v", err)
	}
	if _, err := os.Stat(filepath.Join(dir, "tls")); err == nil {
		t.Fatal("code must not create a certificate")
	}
	cert, err := tlsutil.EnsureSelfSigned(dir)
	if err != nil {
		t.Fatal(err)
	}
	want := tlsutil.SecurityCode(cert)
	if err := runCode([]string{"-data-dir", dir}, &out); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(out.String(), "Security code:       "+want) || !strings.Contains(out.String(), tlsutil.Fingerprint(cert)) {
		t.Fatalf("output: %s", out.String())
	}

	// -json, and an own certificate.
	own := t.TempDir()
	ownCert, err := tlsutil.EnsureSelfSigned(own)
	if err != nil {
		t.Fatal(err)
	}
	out.Reset()
	if err := runCode([]string{"-data-dir", dir, "-json", "-tls-cert", filepath.Join(own, "tls", "cert.pem"), "-tls-key", filepath.Join(own, "tls", "key.pem")}, &out); err != nil {
		t.Fatal(err)
	}
	var ci certInfo
	if err := json.Unmarshal([]byte(out.String()), &ci); err != nil || ci.SecurityCode != tlsutil.SecurityCode(ownCert) || ci.Source != "own" || ci.SecurityCode == want {
		t.Fatalf("%v %+v", err, ci)
	}
	if err := runCode([]string{"-data-dir", dir, "-dev"}, &out); !errors.Is(err, errNoTLS) {
		t.Fatalf("dev mode: %v", err)
	}
}

func TestResetAdminCommand(t *testing.T) {
	var out strings.Builder
	// No database, then a database without an admin: "Not set up yet".
	for _, dir := range []string{t.TempDir(), newServerData(t, false)} {
		err := runResetAdmin([]string{"-data-dir", dir}, &out)
		if err == nil || !strings.Contains(err.Error(), "Not set up yet: use uzume-server setup-code") {
			t.Fatalf("%s: %v", dir, err)
		}
	}

	// One admin: -user is optional, the password works once and must be changed.
	dir := newServerData(t, true)
	svc, closeFn := testService(t, dir)
	ctx := context.Background()
	if _, err := svc.CreateUser(ctx, "anna", "Anna"); err != nil {
		t.Fatal(err)
	}
	closeFn()
	out.Reset()
	if err := runResetAdmin([]string{"-data-dir", dir, "-json"}, &out); err != nil {
		t.Fatal(err)
	}
	var j struct {
		User     string `json:"user"`
		Password string `json:"temporary_password"`
		Must     bool   `json:"must_change_password"`
	}
	if err := json.Unmarshal([]byte(out.String()), &j); err != nil || j.User != "max" || j.Password == "" || !j.Must {
		t.Fatalf("%v %s", err, out.String())
	}
	svc, closeFn = testService(t, dir)
	if _, err := svc.VerifyPassword(ctx, "max", j.Password); err != nil {
		t.Fatalf("the temporary password must work: %v", err)
	}
	if _, err := svc.VerifyPassword(ctx, "max", "a-long-password-1"); err == nil {
		t.Fatal("the old password must not work")
	}
	// Several admins: -user is required and the error lists the names.
	anna, _ := svc.ListUsers(ctx)
	for _, u := range anna {
		if u.Username == "anna" {
			if err := svc.SetUserRole(ctx, "", u.ID, hub.RoleAdmin, "anna-long-password"); err != nil {
				t.Fatal(err)
			}
		}
	}
	closeFn()
	out.Reset()
	err := runResetAdmin([]string{"-data-dir", dir}, &out)
	if err == nil || !strings.Contains(err.Error(), "-user NAME") || !strings.Contains(err.Error(), "max") || !strings.Contains(err.Error(), "anna") {
		t.Fatalf("several admins: %v", err)
	}
	if err := runResetAdmin([]string{"-data-dir", dir, "-user", "nobody"}, &out); err == nil || !strings.Contains(err.Error(), "not an admin") {
		t.Fatalf("unknown: %v", err)
	}
	if err := runResetAdmin([]string{"-data-dir", dir, "-user", "Anna"}, &out); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(out.String(), `admin "anna"`) || strings.Contains(out.String(), "a-long-password") {
		t.Fatalf("output: %s", out.String())
	}
}

func TestStatusStoppedDataDir(t *testing.T) {
	// Not started: nothing is created.
	dir := filepath.Join(t.TempDir(), "never")
	var out strings.Builder
	if err := runStatus([]string{"-data-dir", dir}, &out); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(out.String(), "not started yet") || !strings.Contains(out.String(), "Running         no") {
		t.Fatalf("output: %s", out.String())
	}
	if _, err := os.Stat(dir); err == nil {
		t.Fatal("status must not create the data folder")
	}

	// Ready for setup, server not running: no setup code is created (the server replaces it at its start), a hint says so.
	dir = newServerData(t, false)
	cert, _ := tlsutil.EnsureSelfSigned(dir)
	out.Reset()
	if err := runStatus([]string{"-data-dir", dir, "-listen", "192.0.2.10:8443"}, &out); err != nil {
		t.Fatal(err)
	}
	s := out.String()
	for _, want := range []string{"ready for setup", "Running         no", "192.0.2.10:8443", tlsutil.SecurityCode(cert),
		"The server makes a new setup code when it starts. Start it, then run uzume-server status again.",
		"uzume-server setup-code", "uzume-server reset-admin", "uzume-server restore"} {
		if !strings.Contains(s, want) {
			t.Errorf("missing %q in:\n%s", want, s)
		}
	}
	if strings.Contains(s, "Setup code ") || strings.Contains(s, "one use") {
		t.Errorf("a stopped server must not show a setup code:\n%s", s)
	}
	out.Reset()
	if err := runStatus([]string{"-data-dir", dir, "-json"}, &out); err != nil {
		t.Fatal(err)
	}
	var j statusJSON
	if err := json.Unmarshal([]byte(out.String()), &j); err != nil || j.State != "ready_for_setup" || j.SetupCode != "" || j.Running == nil || *j.Running {
		t.Fatalf("%v %+v", err, j)
	}

	// Ready for setup, server running (data folder lock held): shows the live setup code.
	release, err := backup.LockDataDir(dir)
	if err != nil {
		t.Fatal(err)
	}
	out.Reset()
	if err := runStatus([]string{"-data-dir", dir}, &out); err != nil {
		t.Fatal(err)
	}
	s = out.String()
	for _, want := range []string{"Running         yes", "one use, valid 60 min", "Open Uzume on another PC → Set up a server → On another device."} {
		if !strings.Contains(s, want) {
			t.Errorf("missing %q in:\n%s", want, s)
		}
	}
	code := regexp.MustCompile(`Setup code +(\S+)`).FindStringSubmatch(s)[1]
	out.Reset()
	if err := runStatus([]string{"-data-dir", dir, "-json"}, &out); err != nil {
		t.Fatal(err)
	}
	j = statusJSON{}
	if err := json.Unmarshal([]byte(out.String()), &j); err != nil || j.State != "ready_for_setup" || j.SetupCode != code || j.Running == nil || !*j.Running {
		t.Fatalf("%v %+v (code %s)", err, j, code)
	}
	release()

	// Set up: users and admins, no setup code.
	dir = newServerData(t, true)
	out.Reset()
	if err := runStatus([]string{"-data-dir", dir}, &out); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(out.String(), "· set up") || !strings.Contains(out.String(), "1 (1 admin)") || strings.Contains(out.String(), "Setup code") {
		t.Fatalf("output: %s", out.String())
	}
}

func TestStatusSeesRunningServer(t *testing.T) {
	dir := newServerData(t, true)
	release, err := backup.LockDataDir(dir) // what the serving process holds
	if err != nil {
		t.Fatal(err)
	}
	var out strings.Builder
	if err := runStatus([]string{"-data-dir", dir}, &out); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(out.String(), "Running         yes") {
		t.Fatalf("output: %s", out.String())
	}
	release()
	// The probe released the lock again: the lock can be taken now and a running server was never disturbed.
	out.Reset()
	if err := runStatus([]string{"-data-dir", dir}, &out); err != nil || !strings.Contains(out.String(), "Running         no") {
		t.Fatalf("%v %s", err, out.String())
	}
}

func TestChooseAdmin(t *testing.T) {
	notSet := errors.New("not set up")
	if _, err := chooseAdmin(nil, "", notSet); err != notSet {
		t.Fatal(err)
	}
	a, b := hub.User{ID: "1", Username: "max"}, hub.User{ID: "2", Username: "Anna"}
	if u, err := chooseAdmin([]hub.User{a}, "", notSet); err != nil || u.ID != "1" {
		t.Fatal(u, err)
	}
	if _, err := chooseAdmin([]hub.User{a, b}, "", notSet); err == nil || !strings.Contains(err.Error(), "max, Anna") {
		t.Fatal(err)
	}
	if u, err := chooseAdmin([]hub.User{a, b}, "anna", notSet); err != nil || u.ID != "2" {
		t.Fatal(u, err)
	}
}

func TestServerAddresses(t *testing.T) {
	ifaces := func() ([]net.Addr, error) {
		return []net.Addr{&net.IPNet{IP: net.ParseIP("127.0.0.1")}, &net.IPNet{IP: net.ParseIP("192.0.2.10")}, &net.IPNet{IP: net.ParseIP("169.254.1.1")},
			&net.IPNet{IP: net.ParseIP("2001:db8::1")}, &net.IPNet{IP: net.ParseIP("192.0.2.20")}}, nil
	}
	cfg := func(args ...string) config.Config {
		fs := flag.NewFlagSet("t", flag.ContinueOnError)
		c := config.Register(fs, func(string) string { return "" })
		if err := fs.Parse(args); err != nil {
			t.Fatal(err)
		}
		return *c
	}
	for _, c := range []struct {
		name      string
		args      []string
		container bool
		want      string
	}{
		{"wildcard", nil, false, "192.0.2.10:8443,192.0.2.20:8443"},
		{"public host", []string{"-public-host", "server.example.com:9443"}, false, "server.example.com:9443,192.0.2.10:8443,192.0.2.20:8443"},
		{"one host", []string{"-listen", "192.0.2.10:8444"}, false, "192.0.2.10:8444"},
		{"container", nil, true, "<address of the Docker host>:8443"},
		{"container with public host", []string{"-public-host", "server.example.com"}, true, "server.example.com:8443"},
		{"sharing off", []string{"-network-sharing=false"}, false, "127.0.0.1:8443 (this PC only)"},
	} {
		if got := strings.Join(serverAddresses(cfg(c.args...), c.container, ifaces), ","); got != c.want {
			t.Errorf("%s: %s, want %s", c.name, got, c.want)
		}
	}
}

func TestSetupBanner(t *testing.T) {
	code := hub.SetupCode{Code: "7H4K-2Q", Expires: time.Date(2026, 10, 22, 14, 32, 0, 0, time.Local)}
	b := setupBanner("0.18.0", []string{"192.0.2.10:8443"}, "K7Q4-9WMP-2HXD", code)
	for _, want := range []string{"Uzume server 0.18.0 is ready for setup", "192.0.2.10:8443", "K7Q4-9WMP-2HXD", "7H4K-2Q", "valid 60 minutes",
		"Enter the setup code in Uzume on another PC: Set up a server › On another device.", "New code later: uzume-server setup-code"} {
		if !strings.Contains(b, want) {
			t.Errorf("missing %q in:\n%s", want, b)
		}
	}
}

func TestAnnounceSetup(t *testing.T) {
	dir := newServerData(t, false)
	svc, closeFn := testService(t, dir)
	defer closeFn()
	var out, logs bytes.Buffer
	fs := flag.NewFlagSet("t", flag.ContinueOnError)
	cfg := config.Register(fs, func(string) string { return "" })
	log := newTestLogger(&logs)
	announceSetup(context.Background(), svc, *cfg, "K7Q4-9WMP-2HXD", log, &out)
	cur, _ := svc.CurrentSetupCode(context.Background())
	if cur == nil || !strings.Contains(out.String(), cur.Code) || !strings.Contains(logs.String(), cur.Code) {
		t.Fatalf("code %+v\nstdout: %s\nlog: %s", cur, out.String(), logs.String())
	}
	first := cur.Code
	// A restart replaces the code.
	out.Reset()
	announceSetup(context.Background(), svc, *cfg, "K7Q4-9WMP-2HXD", log, &out)
	cur, _ = svc.CurrentSetupCode(context.Background())
	if cur == nil || cur.Code == first { // a collision of two random codes is 1 in a billion
		t.Fatalf("a restart must replace the code: %+v (was %s)", cur, first)
	}
	// Nothing is printed once an admin exists.
	if _, err := svc.CreateAdmin(context.Background(), "max", "a-long-password-1"); err != nil {
		t.Fatal(err)
	}
	out.Reset()
	announceSetup(context.Background(), svc, *cfg, "", log, &out)
	if out.Len() != 0 {
		t.Fatalf("banner after setup: %s", out.String())
	}
}

func TestHelpListsEveryCommand(t *testing.T) {
	var out strings.Builder
	printHelp(&out)
	for _, c := range []string{"setup-code", "code", "reset-admin", "status", "update", "backup", "restore <file>", "reset", "setup-admin", "rollback", "renew-cert",
		"import-cores", "import-cheats", "import-tools", "import-plugins", "diagnostics", "grant-folder", "version", "help"} {
		if !strings.Contains(out.String(), "uzume-server "+c) {
			t.Errorf("help lacks %q:\n%s", c, out.String())
		}
	}
	err := dispatch([]string{"frobnicate"})
	var ue *usageError
	if !errors.As(err, &ue) || !strings.Contains(err.Error(), "uzume-server help") {
		t.Fatalf("unknown command: %v", err)
	}
}

func TestServerEnvFile(t *testing.T) {
	path := filepath.Join(t.TempDir(), "server.env")
	body := "# comment\nUZUME_LISTEN=:9443\nUZUME_DATA_DIR=\"/srv/uz me\"\nUZUME_NAME='Home \"One\"'\nUZUME_PUBLIC_HOST=hub.example.org\nOTHER=1\n\n; semicolon\nbad line\nUZUME_ALREADY=from-file\n"
	if err := os.WriteFile(path, []byte(body), 0o600); err != nil {
		t.Fatal(err)
	}
	env := map[string]string{"UZUME_ALREADY": "from-env", "UZUME_PUBLIC_HOST": ""}
	applyEnvFile(path, func(k string) (string, bool) { v, ok := env[k]; return v, ok }, func(k, v string) error { env[k] = v; return nil })
	want := map[string]string{"UZUME_LISTEN": ":9443", "UZUME_DATA_DIR": "/srv/uz me", "UZUME_NAME": `Home "One"`, "UZUME_PUBLIC_HOST": "hub.example.org", "UZUME_ALREADY": "from-env"}
	for k, v := range want {
		if env[k] != v {
			t.Errorf("%s = %q, want %q", k, env[k], v)
		}
	}
	if _, ok := env["OTHER"]; ok {
		t.Error("only UZUME_* keys are read")
	}
	// A missing file is fine.
	applyEnvFile(filepath.Join(t.TempDir(), "missing"), os.LookupEnv, func(k, v string) error { t.Fatal("set"); return nil })
}

func newTestLogger(w *bytes.Buffer) *slog.Logger { return slog.New(slog.NewTextHandler(w, nil)) }
