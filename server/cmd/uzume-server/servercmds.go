package main

import (
	"context"
	"crypto/tls"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"io/fs"
	"log/slog"
	"net"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/phabioo/uzume/server/internal/backup"
	"github.com/phabioo/uzume/server/internal/config"
	"github.com/phabioo/uzume/server/internal/hub"
	"github.com/phabioo/uzume/server/internal/tlsutil"
	"github.com/phabioo/uzume/server/internal/updates"
	"github.com/phabioo/uzume/server/internal/version"
)

// Server commands for setup and emergencies (0.18 package 6, ADR 0030 D4):
//
//	uzume-server setup-code [-json]               a new setup code for setting up this server from the app
//	uzume-server code [-json]                     the security code of the certificate the server serves
//	uzume-server reset-admin [-user NAME] [-json] a one-time password for an admin
//	uzume-server status [-json]                   version, address, codes, set up or not
//
// They work next to the running server (the database is shared like for `setup-admin`), except `reset` and `restore`.

// commandHelp is one line of the help text and of `status`.
type commandHelp struct{ usage, summary string }

// serverCommands are the commands for setup and emergencies, in the order of the design (docs/design/uzume/frame.md,
// 0-4 G); moreCommands are the other ones.
var serverCommands = []commandHelp{
	{"setup-code", "print a new setup code to set up this server from the app (first setup)"},
	{"code", "print the security code to compare when you set up or join"},
	{"reset-admin", "print a one-time admin password; nothing else is touched"},
	{"status", "show version, address, codes and whether the server is set up"},
	{"update", "install the newest version on the channel (update -check only looks)"},
	{"backup", "make a backup now (also: backup list | config | verify <file>)"},
	{"restore <file>", "restore a backup, replaces everything, makes a safety backup first (server stopped)"},
	{"reset", "delete all data and start fresh, asks for the server name (server stopped)"},
}

var moreCommands = []commandHelp{
	{"setup-admin -username NAME", "create the first admin with a password from stdin (without the app)"},
	{"rollback --list", "list earlier versions and what going back does; also: rollback <version> | --resume"},
	{"renew-cert", "renew the self-generated TLS certificate now (changes the security code)"},
	{"import-cores <dir>", "import libretro buildbot core zips from a folder (offline)"},
	{"import-cheats <zip>", "import a libretro-database archive as the cheat database (offline)"},
	{"import-plugins <dir>", "offer every *.fbplugin file of a folder to apps (offline)"},
	{"import-tools <zip>...", "add randomizer engine release zips to the tools catalog (offline)"},
	{"diagnostics <out.zip>", "write the diagnostics bundle (logs, configuration; no database access)"},
	{"import-data --from DIR", "copy the data folder of a stopped server (the app's \"On this PC\" server) into this server's data folder"},
	{"sharing stop|start", "Windows: stop or start sharing of the server service (administrator); see docs/guides/server-configuration.md"},
	{"grant-folder <path>", "Windows: let the server service read a folder (administrator)"},
	{"version [--json]", "print the version"},
	{"help", "this list"},
}

// printHelp is the text of `uzume-server help`.
func printHelp(w io.Writer) {
	fmt.Fprintln(w, "Uzume server", version.String())
	fmt.Fprintln(w, "\nUsage: uzume-server [flags]            start the server (flags and UZUME_* variables: docs/guides/server-configuration.md)")
	fmt.Fprintln(w, "       uzume-server <command> [flags]")
	fmt.Fprintln(w, "\nCommands for setup and emergencies:")
	for _, c := range serverCommands {
		fmt.Fprintf(w, "  uzume-server %-*s  %s\n", commandWidth(), c.usage, c.summary)
	}
	fmt.Fprintln(w, "\nOther commands:")
	for _, c := range moreCommands {
		fmt.Fprintf(w, "  uzume-server %-*s  %s\n", commandWidth(), c.usage, c.summary)
	}
	fmt.Fprintln(w, "\nA command finds the data folder like the service does (UZUME_DATA_DIR, -data-dir; on Linux also /etc/uzume/server.env).")
	fmt.Fprintln(w, "Windows: run commands in a terminal as administrator. Docker: docker exec -it uzume-server uzume-server <command>.")
}

func commandWidth() int {
	w := 0
	for _, l := range [][]commandHelp{serverCommands, moreCommands} {
		for _, c := range l {
			if len(c.usage) > w {
				w = len(c.usage)
			}
		}
	}
	return w
}

// userMessage is an error that is a sentence for the person at the terminal (capitalized, with its own punctuation).
type userMessage string

func (m userMessage) Error() string { return string(m) }

// ---- access and platform hints ----

// inContainer reports whether this is the official container image.
func inContainer() bool { return updates.InContainer() }

// stopHint says how to stop the server on this platform (for the commands that need it stopped).
func stopHint() string {
	switch {
	case inContainer():
		return "docker compose stop (or: docker stop uzume-server), then run the command in a one-off container with 'docker compose run --rm --no-deps <service> <command>'"
	case runtime.GOOS == "windows":
		return "in a terminal as administrator: Stop-Service " + hubServiceName
	default:
		return "for example 'sudo systemctl stop uzume-server'"
	}
}

// accessError turns a permission error on the data folder into a message that says what to do.
func accessError(dir string, err error) error {
	switch {
	case runtime.GOOS == "windows":
		return fmt.Errorf("access denied for the data folder %s: open a terminal as administrator (right-click, Run as administrator) and run the command again", dir)
	case inContainer():
		return fmt.Errorf("permission denied for the data folder %s: %w; run the command with docker exec -it uzume-server uzume-server <command>", dir, err)
	default:
		return fmt.Errorf("permission denied for the data folder %s: run the command with sudo, or as the service user (sudo -u uzume uzume-server <command>)", dir)
	}
}

// probeWritable checks that the data folder can be written (the commands open the database read-write).
func probeWritable(dir string) error {
	f, err := os.CreateTemp(dir, ".access-*")
	if err != nil {
		if errors.Is(err, fs.ErrPermission) {
			return accessError(dir, err)
		}
		return err
	}
	name := f.Name()
	f.Close()
	return os.Remove(name)
}

// dbPath is the database file of a data folder; hasDatabase reports whether the server created it already.
func dbPath(dataDir string) string { return filepath.Join(dataDir, "uzume.db") }

func hasDatabase(dataDir string) bool {
	_, err := os.Stat(dbPath(dataDir))
	return err == nil
}

// serverRunning tells whether a server holds the data folder lock; known is false when that cannot be told. The lock is
// taken without waiting and released at once, so a running server is never disturbed.
func serverRunning(dataDir string) (running, known bool) {
	if st, err := os.Stat(dataDir); err != nil || !st.IsDir() {
		return false, true
	}
	release, err := backup.LockDataDir(dataDir)
	switch {
	case err == nil:
		release()
		return false, true
	case errors.Is(err, backup.ErrLocked):
		return true, true
	}
	return false, false
}

// ---- certificate and addresses ----

var (
	errNoTLS         = errors.New("the server runs without TLS (development mode): there is no certificate and no security code")
	errNoCertificate = errors.New("the server has no certificate yet: it creates one the first time it starts")
)

// certInfo describes the certificate the server serves.
type certInfo struct {
	SecurityCode string `json:"security_code"`
	Fingerprint  string `json:"fingerprint"`
	Source       string `json:"source"` // self-generated or own
	Path         string `json:"path"`
}

// serverCertificate reads the certificate the server serves without creating, renewing or repairing anything (a
// command may run as another user than the service): the own certificate of -tls-cert, else the self-generated one in
// the data folder.
func serverCertificate(cfg *config.Config) (certInfo, error) {
	var (
		cert tls.Certificate
		err  error
		ci   certInfo
	)
	switch {
	case !cfg.UseTLS():
		return ci, errNoTLS
	case cfg.TLSCert != "":
		cert, err = tlsutil.Load(cfg.TLSCert, cfg.TLSKey)
		ci.Source, ci.Path = "own", cfg.TLSCert
	default:
		cert, err = tlsutil.LoadSelfSigned(cfg.DataDir)
		ci.Source, ci.Path = "self-generated", filepath.Join(cfg.DataDir, "tls", "cert.pem")
		if errors.Is(err, os.ErrNotExist) {
			return ci, errNoCertificate
		}
	}
	if err != nil {
		return ci, fmt.Errorf("read the TLS certificate: %w", err)
	}
	ci.SecurityCode, ci.Fingerprint = tlsutil.SecurityCode(cert), tlsutil.Fingerprint(cert)
	return ci, nil
}

// serverAddresses are the host:port values apps can use to reach the server: the public host, a listen address that
// names one host, or else the addresses of this machine (not in a container, where they are the container's own).
func serverAddresses(eff config.Config, container bool, ifaceAddrs func() ([]net.Addr, error)) []string {
	host, port, err := net.SplitHostPort(eff.Listen)
	if err != nil || port == "" {
		host, port = "", "8443"
	}
	if !eff.NetworkSharing {
		return []string{net.JoinHostPort("127.0.0.1", port) + " (this PC only)"}
	}
	var out []string
	add := func(a string) {
		for _, o := range out {
			if o == a {
				return
			}
		}
		out = append(out, a)
	}
	if h := eff.PublicHostName(); h != "" {
		p := port
		if n := eff.PublicHostPort(); n > 0 {
			p = strconv.Itoa(n)
		}
		add(net.JoinHostPort(h, p))
	}
	ip := net.ParseIP(host)
	switch {
	case host != "" && (ip == nil || !ip.IsUnspecified()):
		add(net.JoinHostPort(host, port))
	case container:
		if len(out) == 0 {
			add("<address of the Docker host>:" + port)
		}
	default:
		addrs, _ := ifaceAddrs()
		var v4 []string
		for _, a := range addrs {
			ipn, ok := a.(*net.IPNet)
			if !ok || ipn.IP.To4() == nil || ipn.IP.IsLoopback() || ipn.IP.IsLinkLocalUnicast() {
				continue
			}
			v4 = append(v4, ipn.IP.String())
		}
		sort.Strings(v4)
		for _, a := range v4 {
			add(net.JoinHostPort(a, port))
		}
		if len(out) == 0 {
			add(net.JoinHostPort("127.0.0.1", port) + " (this PC only)")
		}
	}
	return out
}

// effectiveForCommand returns the configuration the server would run with: server.env and flags, then the network
// settings saved in the database (when there is one).
func effectiveForCommand(ctx context.Context, cfg *config.Config, svc *hub.Service) config.Config {
	if svc == nil {
		return *cfg
	}
	stored, err := svc.NetOverrides(ctx)
	if err != nil {
		return *cfg
	}
	eff, _ := effectiveConfig(cfg, stored, slog.New(slog.NewTextHandler(io.Discard, nil)))
	return eff
}

// ---- setup banner ----

// setupBanner is the text the server prints at start while it has no admin, and `setup-code` prints in short.
func setupBanner(ver string, addrs []string, securityCode string, code hub.SetupCode) string {
	var b strings.Builder
	rule := strings.Repeat("=", 66)
	fmt.Fprintln(&b, rule)
	fmt.Fprintf(&b, " Uzume server %s is ready for setup\n\n", ver)
	for i, a := range addrs {
		label := "Address"
		if i > 0 {
			label = ""
		}
		fmt.Fprintf(&b, " %-14s %s\n", label, a)
	}
	if securityCode != "" {
		fmt.Fprintf(&b, " %-14s %s\n", "Security code", securityCode)
	}
	fmt.Fprintf(&b, " %-14s %s  (works once, valid %d minutes, until %s)\n\n", "Setup code", code.Code, int(hub.SetupCodeValidity/time.Minute), code.Expires.Local().Format("15:04"))
	fmt.Fprintln(&b, " Enter the setup code in Uzume on another PC: Set up a server › On another device.")
	fmt.Fprintln(&b, " New code later: uzume-server setup-code")
	fmt.Fprint(&b, rule)
	return b.String()
}

// ---- setup-code ----

func runSetupCode(args []string, out io.Writer) error {
	fs := flag.NewFlagSet("setup-code", flag.ContinueOnError)
	cfg := config.Register(fs, os.Getenv)
	asJSON := fs.Bool("json", false, "print one JSON object")
	if err := fs.Parse(args); err != nil {
		return err
	}
	if fs.NArg() > 0 {
		return errors.New("usage: uzume-server setup-code [-json]")
	}
	if err := cfg.Validate(); err != nil {
		return err
	}
	if !hasDatabase(cfg.DataDir) {
		return fmt.Errorf("the server has not been started yet (no database in %s): start it once, it prints a setup code", cfg.DataDir)
	}
	ctx := context.Background()
	svc, closeFn, err := openService(ctx, cfg, false)
	if err != nil {
		return err
	}
	defer closeFn()
	code, err := svc.NewSetupCode(ctx)
	if errors.Is(err, hub.ErrAlreadySetUp) {
		return userMessage("This server is already set up. Forgot the admin password? Run: uzume-server reset-admin")
	}
	if err != nil {
		return err
	}
	notRunning := false
	if running, known := serverRunning(cfg.DataDir); known && !running {
		notRunning = true
	}
	ci, cerr := serverCertificate(cfg)
	eff := effectiveForCommand(ctx, cfg, svc)
	addrs := serverAddresses(eff, inContainer(), net.InterfaceAddrs)
	if *asJSON {
		return json.NewEncoder(out).Encode(map[string]any{"setup_code": code.Code, "expires_at": code.Expires, "valid_minutes": int(hub.SetupCodeValidity / time.Minute),
			"security_code": ci.SecurityCode, "addresses": addrs})
	}
	fmt.Fprintf(out, "Setup code:     %s   (works once, valid %d minutes, until %s)\n", code.Code, int(hub.SetupCodeValidity/time.Minute), code.Expires.Local().Format("15:04"))
	switch {
	case cerr == nil:
		fmt.Fprintf(out, "Security code:  %s\n", ci.SecurityCode)
	case errors.Is(cerr, errNoCertificate), errors.Is(cerr, errNoTLS):
		fmt.Fprintf(out, "Security code:  none yet (%v)\n", cerr)
	}
	for _, a := range addrs {
		fmt.Fprintf(out, "Address:        %s\n", a)
	}
	fmt.Fprintln(out, "\nEnter the setup code in Uzume on another PC: Set up a server › On another device. Compare the security code when Uzume asks.")
	fmt.Fprintln(out, "An earlier setup code no longer works.")
	if notRunning {
		fmt.Fprintln(out, "The server is not running: it replaces this code when it starts. Start it first, then run uzume-server setup-code.")
	}
	return nil
}

// ---- code ----

func runCode(args []string, out io.Writer) error {
	fs := flag.NewFlagSet("code", flag.ContinueOnError)
	cfg := config.Register(fs, os.Getenv)
	asJSON := fs.Bool("json", false, "print one JSON object")
	if err := fs.Parse(args); err != nil {
		return err
	}
	if fs.NArg() > 0 {
		return errors.New("usage: uzume-server code [-json]")
	}
	if err := cfg.Validate(); err != nil {
		return err
	}
	ci, err := serverCertificate(cfg)
	if err != nil {
		return err
	}
	if *asJSON {
		return json.NewEncoder(out).Encode(ci)
	}
	fmt.Fprintf(out, "Security code:       %s\n", ci.SecurityCode)
	fmt.Fprintf(out, "SHA-256 fingerprint: %s\n", ci.Fingerprint)
	fmt.Fprintf(out, "Certificate:         %s (%s)\n", ci.Path, ci.Source)
	fmt.Fprintln(out, "\nUzume shows the same code when you set up or join this server. If the codes differ, do not continue.")
	return nil
}

// ---- reset-admin ----

func runResetAdmin(args []string, out io.Writer) error {
	fs := flag.NewFlagSet("reset-admin", flag.ContinueOnError)
	cfg := config.Register(fs, os.Getenv)
	user := fs.String("user", "", "the admin (optional when there is exactly one)")
	asJSON := fs.Bool("json", false, "print one JSON object")
	if err := fs.Parse(args); err != nil {
		return err
	}
	if fs.NArg() > 0 {
		return errors.New("usage: uzume-server reset-admin [-user NAME] [-json]")
	}
	if err := cfg.Validate(); err != nil {
		return err
	}
	errNotSetUp := userMessage("Not set up yet: use uzume-server setup-code")
	if !hasDatabase(cfg.DataDir) {
		return errNotSetUp
	}
	ctx := context.Background()
	svc, closeFn, err := openService(ctx, cfg, false)
	if err != nil {
		return err
	}
	defer closeFn()
	admins, err := svc.ListAdmins(ctx)
	if err != nil {
		return err
	}
	chosen, err := chooseAdmin(admins, *user, errNotSetUp)
	if err != nil {
		return err
	}
	temp, err := svc.ResetAdminPasswordOnServer(ctx, chosen.ID)
	if err != nil {
		return err
	}
	if *asJSON {
		return json.NewEncoder(out).Encode(map[string]any{"user": chosen.Username, "temporary_password": temp, "must_change_password": true})
	}
	fmt.Fprintf(out, "Temporary password for admin %q: %s\n\n", chosen.Username, temp)
	fmt.Fprintln(out, "Sign in with it; Uzume asks for a new password right away. It is shown only now.")
	fmt.Fprintln(out, "Saves, users, games and settings are untouched, and signed-in devices stay signed in.")
	return nil
}

// chooseAdmin picks the admin for reset-admin.
func chooseAdmin(admins []hub.User, name string, notSetUp error) (hub.User, error) {
	if len(admins) == 0 {
		return hub.User{}, notSetUp
	}
	names := make([]string, len(admins))
	for i, a := range admins {
		names[i] = a.Username
	}
	if name == "" {
		if len(admins) == 1 {
			return admins[0], nil
		}
		return hub.User{}, fmt.Errorf("this server has several admins: choose one with -user NAME (%s)", strings.Join(names, ", "))
	}
	for _, a := range admins {
		if a.Username == name {
			return a, nil
		}
	}
	for _, a := range admins {
		if strings.EqualFold(a.Username, name) {
			return a, nil
		}
	}
	return hub.User{}, fmt.Errorf("%q is not an admin of this server (admins: %s)", name, strings.Join(names, ", "))
}

// ---- status ----

type statusLastBackup struct {
	Name     string    `json:"name"`
	Finished time.Time `json:"finished"`
	Size     int64     `json:"size"`
}

type statusJSON struct {
	Product        string            `json:"product"`
	Version        string            `json:"version"`
	Channel        string            `json:"channel"`
	DataDir        string            `json:"data_dir"`
	Running        *bool             `json:"running"` // null: cannot be told
	State          string            `json:"state"`   // not_started, ready_for_setup or set_up
	Name           string            `json:"name,omitempty"`
	Addresses      []string          `json:"addresses"`
	SecurityCode   string            `json:"security_code,omitempty"`
	SetupCode      string            `json:"setup_code,omitempty"`
	SetupCodeUntil *time.Time        `json:"setup_code_expires_at,omitempty"`
	Users          int               `json:"users"`
	Admins         int               `json:"admins"`
	BackupFolder   string            `json:"backup_folder,omitempty"`
	LastBackup     *statusLastBackup `json:"last_backup,omitempty"`
}

func runStatus(args []string, out io.Writer) error {
	fs := flag.NewFlagSet("status", flag.ContinueOnError)
	cfg := config.Register(fs, os.Getenv)
	asJSON := fs.Bool("json", false, "print one JSON object")
	if err := fs.Parse(args); err != nil {
		return err
	}
	if fs.NArg() > 0 {
		return errors.New("usage: uzume-server status [-json]")
	}
	if err := cfg.Validate(); err != nil {
		return err
	}
	st := statusJSON{Product: "server", Version: version.String(), Channel: version.Channel, DataDir: cfg.DataDir, Addresses: []string{}, State: "not_started"}
	if running, known := serverRunning(cfg.DataDir); known {
		st.Running = &running
	}
	var setupHint string
	ctx := context.Background()
	var svc *hub.Service
	if hasDatabase(cfg.DataDir) {
		s, closeFn, err := openService(ctx, cfg, false)
		if err != nil {
			return err
		}
		defer closeFn()
		svc = s
		st.Name = svc.Info().Name
		admins, err := svc.ListAdmins(ctx)
		if err != nil {
			return err
		}
		st.Admins = len(admins)
		if c, err := svc.DiagCounts(ctx); err == nil {
			st.Users = c.Users
		}
		if len(admins) > 0 {
			st.State = "set_up"
		} else {
			st.State = "ready_for_setup"
			if st.Running != nil && !*st.Running {
				// Every server start replaces the setup code, so one made now would be dead on arrival.
				setupHint = "The server makes a new setup code when it starts. Start it, then run uzume-server status again."
			} else {
				code, err := svc.EnsureSetupCode(ctx)
				if err != nil {
					return err
				}
				st.SetupCode, st.SetupCodeUntil = code.Code, &code.Expires
			}
		}
		bs := svc.BackupStatus(ctx)
		st.BackupFolder = bs.Settings.Folder
		if bs.LastOK != nil {
			st.LastBackup = &statusLastBackup{Name: bs.LastOK.Name, Finished: bs.LastOK.Finished, Size: bs.LastOK.Size}
		}
	} else {
		setupHint = "The server has not been started yet: it creates its data and prints a setup code at the first start."
	}
	eff := effectiveForCommand(ctx, cfg, svc)
	st.Addresses = serverAddresses(eff, inContainer(), net.InterfaceAddrs)
	certNote := ""
	if ci, err := serverCertificate(cfg); err == nil {
		st.SecurityCode = ci.SecurityCode
	} else {
		certNote = err.Error()
	}
	if *asJSON {
		return json.NewEncoder(out).Encode(st)
	}
	printStatus(out, st, certNote, setupHint)
	return nil
}

func printStatus(out io.Writer, st statusJSON, certNote, hint string) {
	state := map[string]string{"not_started": "not started yet", "ready_for_setup": "ready for setup", "set_up": "set up"}[st.State]
	fmt.Fprintf(out, "Uzume server %s · %s\n\n", st.Version, state)
	row := func(label, value string) { fmt.Fprintf(out, "%-15s %s\n", label, value) }
	if st.Name != "" {
		row("Name", st.Name)
	}
	switch {
	case st.Running == nil:
		row("Running", "unknown")
	case *st.Running:
		row("Running", "yes")
	default:
		row("Running", "no")
	}
	for i, a := range st.Addresses {
		label := "Address"
		if i > 0 {
			label = ""
		}
		row(label, a)
	}
	switch {
	case st.SecurityCode != "":
		row("Security code", st.SecurityCode)
	case certNote != "":
		row("Security code", "none ("+certNote+")")
	}
	if st.State == "ready_for_setup" && st.SetupCode != "" {
		row("Setup code", fmt.Sprintf("%s   (one use, valid %d min, until %s)", st.SetupCode, int(hub.SetupCodeValidity/time.Minute), st.SetupCodeUntil.Local().Format("15:04")))
		fmt.Fprintln(out, "\nOpen Uzume on another PC → Set up a server → On another device.")
	}
	if st.State == "set_up" {
		row("Users", fmt.Sprintf("%d (%d admin%s)", st.Users, st.Admins, plural(st.Admins)))
	}
	if hint != "" {
		fmt.Fprintln(out, "\n"+hint)
	}
	fmt.Fprintln(out)
	row("Data folder", st.DataDir)
	switch {
	case st.BackupFolder == "":
		if st.State != "not_started" {
			row("Backups", "no folder set (uzume-server backup config --folder DIR)")
		}
	case st.LastBackup != nil:
		row("Backups", fmt.Sprintf("%s · last %s (%s)", st.BackupFolder, st.LastBackup.Finished.Local().Format("2006-01-02 15:04"), backup.FormatBytes(uint64(st.LastBackup.Size))))
	default:
		row("Backups", st.BackupFolder+" · no backup yet")
	}
	fmt.Fprintln(out, "\nCommands")
	for _, c := range serverCommands {
		fmt.Fprintf(out, "  uzume-server %-14s  %s\n", c.usage, c.summary)
	}
	fmt.Fprintln(out, "  uzume-server help             all commands")
}

func plural(n int) string {
	if n == 1 {
		return ""
	}
	return "s"
}
