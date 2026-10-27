// Command uzume-server is the Uzume server.
//
//	uzume-server [flags]                      start the server (API + info endpoint)
//	uzume-server setup-code [-json]           print a new setup code (set up this server from the app)
//	uzume-server code [-json]                 print the security code of the certificate the server serves
//	uzume-server reset-admin [-user NAME]     print a one-time password for an admin (nothing else is touched)
//	uzume-server status [-json]               version, address, codes, set up or not
//	uzume-server reset [-confirm NAME]        delete all data and start fresh (the server must be stopped)
//	uzume-server help                         list all commands
//	uzume-server setup-admin -username <name> create the first admin (password from stdin)
//	uzume-server import-data --from DIR       copy the data folder of a stopped server (the app's "On this PC" server) into this server's data folder
//	uzume-server sharing stop|start           Windows: stop (disable) or start (enable) the server service; needs administrator
//	uzume-server grant-folder <path>          Windows: let the server service account read a folder (needs administrator)
//	uzume-server renew-cert                   renew the self-generated TLS certificate now and exit
//	uzume-server import-cores <dir>           import libretro buildbot core zips from a directory (offline) and exit
//	uzume-server import-cheats <zip>          import a libretro-database archive for the cheat database (offline) and exit
//	uzume-server import-plugins <dir>         offer every *.uzplugin file of a directory to apps (offline; legacy .fbplugin accepted) and exit
//	uzume-server import-tools <zip>...        add randomizer engine release zips (UPR_FVX-v1_6_2-Windows.zip, ...) to the tools catalog (offline) and exit
//	uzume-server diagnostics <out.zip>        write the diagnostics bundle (logs, configuration; no database access) and exit
//	uzume-server backup [--out DIR]           make a backup now; also: backup list | config | verify <file>
//	uzume-server restore <file>               restore a backup (the server must be stopped; makes a safety backup first)
//	uzume-server version [--json]            print the version (JSON: product, version, channel, commit, protocol and schema versions)
//	uzume-server update [-check]              install the newest version of the channel (-check only looks)
//	uzume-server update check [-channel c]    print the update selection as JSON
//	uzume-server update stage [-channel c]    check, download, verify and stage the update, create the request file
//	uzume-server update apply-staged          root helper of the update unit: verify and install the staged .deb (Windows: .msi)
//	uzume-server update watch                 Windows: SYSTEM helper service, applies a staged update when the server requests it
//	uzume-server rollback --list              list the earlier versions and what going back does to the data; also: rollback <version> | --resume
package main

import (
	"bufio"
	"context"
	"crypto/tls"
	"errors"
	"flag"
	"fmt"
	"io"
	"io/fs"
	"log/slog"
	"net"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/phabioo/uzume/server/internal/backup"
	"github.com/phabioo/uzume/server/internal/clientip"
	"github.com/phabioo/uzume/server/internal/config"
	"github.com/phabioo/uzume/server/internal/httpapi"
	"github.com/phabioo/uzume/server/internal/hub"
	"github.com/phabioo/uzume/server/internal/serverhost"
	"github.com/phabioo/uzume/server/internal/store"
	"github.com/phabioo/uzume/server/internal/tlsutil"
	"github.com/phabioo/uzume/server/internal/turnsrv"
	"github.com/phabioo/uzume/server/internal/version"
)

func main() {
	args := os.Args[1:]
	err := dispatch(args)
	if errors.Is(err, errRestart) {
		// runServer returned: the server, TURN and the database are closed. Replace this process.
		err = reexec()
	}
	if errors.Is(err, flag.ErrHelp) {
		os.Exit(0) // the flag package printed the usage
	}
	var xe *exitError
	if errors.As(err, &xe) {
		fmt.Fprintln(os.Stderr, "Error:", xe.Error())
		os.Exit(xe.code)
	}
	var ue *usageError
	if errors.As(err, &ue) {
		fmt.Fprintln(os.Stderr, "Error:", ue.Error())
		os.Exit(2)
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, "Error:", err)
		os.Exit(1)
	}
}

// exitError is a failure with its own exit status (import-data refusals: 3).
type exitError struct {
	code int
	err  error
}

func (e *exitError) Error() string { return e.err.Error() }
func (e *exitError) Unwrap() error { return e.err }

// stdout is where the server writes its (few) lines for a parent process: the setup banner and, in child mode,
// UZUME_READY. A variable so that tests can capture it.
var stdout io.Writer = os.Stdout

// usageError is an unknown command (exit status 2).
type usageError struct{ msg string }

func (e *usageError) Error() string { return e.msg }

// dispatch runs the command named by args[0], else the server.
func dispatch(args []string) error {
	if len(args) == 0 || strings.HasPrefix(args[0], "-") {
		return runServer(args)
	}
	cmd, rest := args[0], args[1:]
	switch cmd {
	case "help":
		printHelp(os.Stdout)
		return nil
	case "version":
		return runVersion(rest, os.Stdout)
	case "grant-folder":
		err := runGrantFolder(rest, os.Stdout)
		if errors.Is(err, errGrantUnsupported) {
			fmt.Fprintln(os.Stderr, "not supported")
			os.Exit(2)
		}
		return err
	case "sharing":
		err := runSharing(rest, os.Stdout, newSharingSystem())
		if errors.Is(err, errSharingUnsupported) {
			fmt.Fprintln(os.Stderr, "uzume-server sharing is only available on Windows")
			os.Exit(2)
		}
		return err
	}
	// The commands read the settings of the service (on Linux /etc/uzume/server.env) like the service does.
	loadServerEnv()
	switch cmd {
	case "setup-admin":
		return runSetupAdmin(rest, os.Stdin, os.Stdout)
	case "setup-code":
		return runSetupCode(rest, os.Stdout)
	case "code":
		return runCode(rest, os.Stdout)
	case "reset-admin":
		return runResetAdmin(rest, os.Stdout)
	case "status":
		return runStatus(rest, os.Stdout)
	case "reset":
		return runReset(rest, os.Stdin, os.Stdout)
	case "renew-cert":
		return runRenewCert(rest, os.Stdout)
	case "import-cores":
		return runImportCores(rest, os.Stdout)
	case "import-cheats":
		return runImportCheats(rest, os.Stdout)
	case "import-plugins":
		return runImportPlugins(rest, os.Stdout)
	case "import-tools":
		return runImportTools(rest, os.Stdout)
	case "import-data":
		return runImportData(rest, os.Stdout)
	case "diagnostics":
		return runDiagnostics(rest, os.Stdout)
	case "backup":
		return runBackup(rest, os.Stdout)
	case "restore":
		return runRestore(rest, os.Stdout)
	case "rollback":
		return runRollback(rest, os.Stdin, os.Stdout)
	case "update":
		return runUpdateIO(rest, os.Stdin, os.Stdout)
	}
	return &usageError{msg: fmt.Sprintf("unknown command %q; run 'uzume-server help' for the list of commands", cmd)}
}

// openService opens the database and service in the data directory.
// serving is true only for the server process: it alone cleans the staging area in <data>/tmp, because the CLI
// sub-commands run next to the running service.
func openService(ctx context.Context, cfg *config.Config, serving bool) (*hub.Service, func(), error) {
	if err := os.MkdirAll(cfg.DataDir, 0o750); err != nil {
		if errors.Is(err, fs.ErrPermission) {
			return nil, nil, accessError(cfg.DataDir, err)
		}
		return nil, nil, err
	}
	if !serving {
		if err := probeWritable(cfg.DataDir); err != nil {
			return nil, nil, err
		}
	}
	db, err := store.Open(filepath.Join(cfg.DataDir, "uzume.db"))
	if err != nil {
		return nil, nil, err
	}
	keys, err := cfg.TrustedCoreKeys()
	if err != nil {
		db.Close()
		return nil, nil, err
	}
	svc, err := hub.Open(ctx, db, hub.Options{CleanStaging: serving, DataDir: cfg.DataDir, Name: cfg.Name, HubVersion: version.String(), ICEServers: cfg.ICEServers,
		CoreBuildbotURL: cfg.CoreBuildbotURL, CoreInfoURL: cfg.CoreInfoURL, CoreTrustKeys: keys, CheatDBURL: cfg.CheatDBURL, CheatDB3DSURL: cfg.CheatDB3DSURL,
		SaveKeepRecent: cfg.SaveKeepRecent, SaveKeepDaily: cfg.SaveKeepDaily, SaveKeepWeekly: cfg.SaveKeepWeekly,
		UpdateIndexURL: cfg.UpdateIndexURL, PluginIndexURL: cfg.PluginIndex(), ToolsSourceURL: cfg.ToolsSource(), UpdateRequestDir: cfg.UpdateRequestDir, UpdateChannel: version.Channel,
		BackupExtras: backupExtras(cfg), Child: cfg.Child})
	if err != nil {
		db.Close()
		return nil, nil, err
	}
	return svc, func() { db.Close() }, nil
}

// externalTURNServers reads the secrets of the configured external TURN servers (environment or file). A server
// whose secret is unavailable is kept without a secret (shown as "no secret", not handed to apps) with a
// warning, so that it can be completed in the app; secrets are never logged.
func externalTURNServers(cfg *config.Config, specs []config.ExternalTURNSpec, log *slog.Logger) []hub.ExternalTURN {
	var out []hub.ExternalTURN
	for i, sp := range specs {
		secret, err := cfg.ExternalTURNSecret(sp, i+1, os.Getenv)
		if err != nil {
			log.Warn("external TURN server stays off: secret unavailable", "name", sp.Name, "err", err)
			secret = ""
		}
		p := hub.ExternalTURN{Name: sp.Name, URLs: sp.URLs, Mode: hub.ExternalTURNSecret, Secret: secret}
		if sp.Mode == config.ExternalModeStatic {
			p = hub.ExternalTURN{Name: sp.Name, URLs: sp.URLs, Mode: hub.ExternalTURNStatic, Username: sp.Username, Password: secret}
		}
		out = append(out, p)
		if secret != "" {
			log.Info("external TURN server configured", "name", sp.Name, "mode", p.Mode, "urls", sp.URLs)
		}
	}
	return out
}

// startTURN starts the embedded STUN/TURN server (ADR 0012); it stops with Close on server shutdown.
func startTURN(ctx context.Context, cfg *config.Config, svc *hub.Service, log *slog.Logger) (*turnsrv.Server, error) {
	secret, err := svc.TURNSecret(ctx)
	if err != nil {
		return nil, fmt.Errorf("TURN secret: %w", err)
	}
	lo, hi, err := cfg.TURNRelayRange()
	if err != nil {
		return nil, err
	}
	ts, err := turnsrv.Start(ctx, turnsrv.Config{PublicHost: cfg.PublicHostName(), Port: cfg.TURNPort, RelayMin: lo, RelayMax: hi,
		RelayIP: net.ParseIP(cfg.TURNRelayIP), Secret: secret, DeviceOK: svc.DeviceActive, LANPeerOK: svc.ConnectedPlayerIP, Log: log})
	if err != nil {
		return nil, err
	}
	log.Info("TURN server started", "public_host", cfg.PublicHostName(), "port", ts.Port(), "relay_ports", cfg.TURNRelayPorts)
	return ts, nil
}

func runSetupAdmin(args []string, in io.Reader, out io.Writer) error {
	fs := flag.NewFlagSet("setup-admin", flag.ContinueOnError)
	cfg := config.Register(fs, os.Getenv)
	username := fs.String("username", "", "username of the admin")
	if err := fs.Parse(args); err != nil {
		return err
	}
	if *username == "" {
		return errors.New("-username is missing")
	}
	if err := cfg.Validate(); err != nil {
		return err
	}
	line, err := bufio.NewReader(in).ReadString('\n')
	if err != nil && !(errors.Is(err, io.EOF) && line != "") {
		return errors.New("read password from stdin: expected one line")
	}
	password := strings.TrimRight(line, "\r\n")
	ctx := context.Background()
	svc, closeFn, err := openService(ctx, cfg, false)
	if err != nil {
		return err
	}
	defer closeFn()
	u, err := svc.CreateAdmin(ctx, *username, password)
	if err != nil {
		return err
	}
	fmt.Fprintf(out, "Admin %q created.\n", u.Username)
	return nil
}

// runRenewCert replaces the self-generated certificate (the old pair is kept as *.prev).
// It refuses when an own certificate/key is configured. Restart the server afterwards.
func runRenewCert(args []string, out io.Writer) error {
	fs := flag.NewFlagSet("renew-cert", flag.ContinueOnError)
	cfg := config.Register(fs, os.Getenv)
	if err := fs.Parse(args); err != nil {
		return err
	}
	if err := cfg.Validate(); err != nil {
		return err
	}
	if cfg.TLSCert != "" || cfg.TLSKey != "" {
		return errors.New("an own TLS certificate/key is configured; the server never renews it, renew it where it comes from")
	}
	_, ren, err := tlsutil.RenewSelfSigned(cfg.DataDir, time.Now())
	if err != nil {
		return err
	}
	absDir, absErr := filepath.Abs(filepath.Join(cfg.DataDir, "tls"))
	if absErr != nil {
		absDir = filepath.Join(cfg.DataDir, "tls")
	}
	fmt.Fprintf(out, "Renewed certificate in %s\nOld SHA-256 fingerprint: %s\nNew SHA-256 fingerprint: %s\n", absDir, ren.OldFingerprint, ren.NewFingerprint)
	fmt.Fprintln(out, "The previous certificate and key were kept as *.prev next to them. Restart the server; apps must confirm the new fingerprint.")
	return nil
}

// runImportCores installs libretro buildbot core zips from a directory (<dir>/<platform>/<core>_libretro.<suffix>.zip,
// optional <dir>/info.zip) into the server (offline fallback, ADR 0020 D8). Restart is not needed.
func runImportCores(args []string, out io.Writer) error {
	// The directory may come before or after the flags.
	dir := ""
	if len(args) > 0 && !strings.HasPrefix(args[0], "-") {
		dir, args = args[0], args[1:]
	}
	fs := flag.NewFlagSet("import-cores", flag.ContinueOnError)
	cfg := config.Register(fs, os.Getenv)
	if err := fs.Parse(args); err != nil {
		return err
	}
	if dir == "" && fs.NArg() == 1 {
		dir = fs.Arg(0)
	} else if fs.NArg() > 0 {
		return errors.New("usage: uzume-server import-cores <dir> [flags]")
	}
	if dir == "" {
		return errors.New("usage: uzume-server import-cores <dir> [flags]")
	}
	if err := cfg.Validate(); err != nil {
		return err
	}
	ctx := context.Background()
	svc, closeFn, err := openService(ctx, cfg, false)
	if err != nil {
		return err
	}
	defer closeFn()
	sum, err := svc.ImportCores(ctx, dir)
	if err != nil {
		return err
	}
	fmt.Fprintf(out, "Imported cores from %s: %d installed, %d updated, %d unchanged, %d problem(s)\n",
		dir, sum.Installed, sum.Updated, sum.Unchanged, len(sum.Problems))
	for _, p := range sum.Problems {
		fmt.Fprintln(out, "Rejected:", p)
	}
	if len(sum.Problems) > 0 {
		return fmt.Errorf("%d core(s) could not be imported", len(sum.Problems))
	}
	return nil
}

// runImportCheats installs a libretro-database archive (zip with cht/ and metadat/) as the cheat database (offline
// fallback, like import-cores). Restart is not needed.
func runImportCheats(args []string, out io.Writer) error {
	file := ""
	if len(args) > 0 && !strings.HasPrefix(args[0], "-") {
		file, args = args[0], args[1:]
	}
	fs := flag.NewFlagSet("import-cheats", flag.ContinueOnError)
	cfg := config.Register(fs, os.Getenv)
	if err := fs.Parse(args); err != nil {
		return err
	}
	if file == "" && fs.NArg() == 1 {
		file = fs.Arg(0)
	} else if fs.NArg() > 0 {
		return errors.New("usage: uzume-server import-cheats <zip> [flags]")
	}
	if file == "" {
		return errors.New("usage: uzume-server import-cheats <zip> [flags]")
	}
	if err := cfg.Validate(); err != nil {
		return err
	}
	ctx := context.Background()
	svc, closeFn, err := openService(ctx, cfg, false)
	if err != nil {
		return err
	}
	defer closeFn()
	if err := svc.ImportCheats(ctx, file); err != nil {
		return err
	}
	st, err := svc.CheatDBStatus(ctx)
	if err != nil {
		return err
	}
	fmt.Fprintf(out, "Imported cheat database from %s: %d cheat list(s), %d DAT file(s)\n", file, st.Lists, st.Dats)
	return nil
}

// runImportPlugins offers every *.uzplugin file of a directory to apps (offline fallback, like import-cores).
// Running the command is the admin's confirmation of the requested permissions. Restart is not needed.
func runImportPlugins(args []string, out io.Writer) error {
	dir := ""
	if len(args) > 0 && !strings.HasPrefix(args[0], "-") {
		dir, args = args[0], args[1:]
	}
	fs := flag.NewFlagSet("import-plugins", flag.ContinueOnError)
	cfg := config.Register(fs, os.Getenv)
	if err := fs.Parse(args); err != nil {
		return err
	}
	if dir == "" && fs.NArg() == 1 {
		dir = fs.Arg(0)
	} else if fs.NArg() > 0 {
		return errors.New("usage: uzume-server import-plugins <dir> [flags]")
	}
	if dir == "" {
		return errors.New("usage: uzume-server import-plugins <dir> [flags]")
	}
	if err := cfg.Validate(); err != nil {
		return err
	}
	ctx := context.Background()
	svc, closeFn, err := openService(ctx, cfg, false)
	if err != nil {
		return err
	}
	defer closeFn()
	sum, err := svc.ImportPlugins(ctx, dir)
	if err != nil {
		return err
	}
	fmt.Fprintf(out, "Imported plugins from %s: %d added, %d unchanged, %d problem(s)\n", dir, sum.Added, sum.Unchanged, len(sum.Problems))
	for _, w := range sum.Warnings {
		fmt.Fprintln(out, "Warning:", w)
	}
	for _, p := range sum.Problems {
		fmt.Fprintln(out, "Rejected:", p)
	}
	if len(sum.Problems) > 0 {
		return fmt.Errorf("%d plugin file(s) could not be imported", len(sum.Problems))
	}
	return nil
}

// runImportTools adds randomizer engine release zips to the tools catalog (offline fallback, like import-cores). The
// version and the platform come from the file name (UPR_FVX-v1_6_2-Windows.zip, -Linux_x86.zip, -Linux_ARM.zip), the
// content is checked like a download and the SHA-256 is pinned. Whether apps may use the tool stays the admin's choice.
func runImportTools(args []string, out io.Writer) error {
	usage := errors.New("usage: uzume-server import-tools <zip>... [flags]")
	var files []string
	for len(args) > 0 && !strings.HasPrefix(args[0], "-") {
		files, args = append(files, args[0]), args[1:]
	}
	fs := flag.NewFlagSet("import-tools", flag.ContinueOnError)
	cfg := config.Register(fs, os.Getenv)
	if err := fs.Parse(args); err != nil {
		return err
	}
	files = append(files, fs.Args()...)
	if len(files) == 0 {
		return usage
	}
	if err := cfg.Validate(); err != nil {
		return err
	}
	ctx := context.Background()
	svc, closeFn, err := openService(ctx, cfg, false)
	if err != nil {
		return err
	}
	defer closeFn()
	problems := 0
	for _, r := range svc.ImportToolFiles(ctx, files) {
		if r.Status == "rejected" {
			problems++
			fmt.Fprintf(out, "Rejected %s: %s\n", r.File, r.Err)
			continue
		}
		fmt.Fprintf(out, "%s %s: %s %s %s\n", strings.ToUpper(r.Status[:1])+r.Status[1:], r.File, r.ToolID, r.Version, r.Platform)
	}
	if problems > 0 {
		return fmt.Errorf("%d file(s) could not be imported", problems)
	}
	return nil
}

// inProcessRestart makes a restart requested through the admin API run the server again inside this process
// instead of re-executing the binary (Windows has no exec; the service must keep its handle to the SCM).
var inProcessRestart = runtime.GOOS == "windows"

func runServer(args []string) error {
	fs := flag.NewFlagSet("uzume-server", flag.ContinueOnError)
	cfg := config.Register(fs, os.Getenv)
	showVersion := fs.Bool("version", false, "print version and exit")
	fs.Usage = func() {
		out := fs.Output()
		fmt.Fprintln(out, "Usage: uzume-server [flags]       start the server\n       uzume-server <command>     run 'uzume-server help' for the commands")
		fmt.Fprintln(out, "\nFlags:")
		fs.PrintDefaults()
	}
	if err := fs.Parse(args); err != nil {
		return err
	}
	if *showVersion {
		fmt.Println(version.String())
		return nil
	}
	if err := cfg.Validate(); err != nil {
		return err
	}
	return runMaybeService(hubServiceName, func(ctx context.Context) error {
		log, closeLog := newLogger(filepath.Join(cfg.DataDir, "logs", "hub.log"))
		defer closeLog()
		if cfg.Child {
			// The server never outlives the app: when the pipe from the app closes (or breaks) it shuts down
			// like on SIGTERM.
			var cancel context.CancelFunc
			ctx, cancel = context.WithCancel(ctx)
			defer cancel()
			go func() {
				watchParent(os.Stdin)
				log.Info("stdin closed: the app is gone, shutting down")
				cancel()
			}()
		}
		return serveLoop(ctx, cfg, log)
	})
}

// runMaybeService runs fn under the Windows service manager when started by it, else with a context that ends on
// Ctrl-C/SIGTERM.
func runMaybeService(name string, fn func(ctx context.Context) error) error {
	if isWindowsService() {
		return runAsService(name, fn)
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	return fn(ctx)
}

// serveLoop runs the server; a restart requested through the admin API runs it again when the platform restarts
// in-process, else it returns errRestart for main to re-execute the binary.
func serveLoop(ctx context.Context, cfg *config.Config, log *slog.Logger) error {
	for {
		err := serve(ctx, cfg, log)
		// A child process never re-executes the binary (the app may have replaced or is about to replace it).
		if !errors.Is(err, errRestart) || !(inProcessRestart || cfg.Child) {
			return err
		}
		if ctx.Err() != nil {
			return nil
		}
		log.Info("Restarting Uzume server")
	}
}

// serve is the server lifecycle: it starts everything, runs until ctx ends, the listener fails or a restart is
// requested, and shuts down gracefully. A requested restart returns errRestart after everything is closed.
func serve(ctx context.Context, cfg *config.Config, log *slog.Logger) error {
	if cfg.CoreIndexURL != "" {
		log.Warn("core-index-url is deprecated and ignored: cores come from the libretro buildbot (core-buildbot-url, ADR 0020)")
	}
	ctx, cancelAll := context.WithCancel(ctx)
	defer cancelAll()

	// Values from server.env and flags; the values saved through the admin API (Settings > Network in the app) win over them.
	base := *cfg
	base.ICEServers = config.CloneICE(cfg.ICEServers)

	// One server per data directory (also keeps `uzume-server restore` away from a running server). A restore that was
	// prepared before this start (admin API) is swapped in now, before the database is opened.
	if err := os.MkdirAll(cfg.DataDir, 0o750); err != nil {
		return err
	}
	releaseLock, err := backup.LockDataDir(cfg.DataDir)
	if err != nil {
		return fmt.Errorf("data directory %s: %w", cfg.DataDir, err)
	}
	defer releaseLock()
	if res, rerr := backup.ApplyPending(cfg.DataDir, time.Now().UTC()); res != nil {
		if rerr != nil {
			log.Error("restore failed and was rolled back; the server starts with its previous data", "backup", res.Backup, "err", rerr)
		} else {
			log.Warn("backup restored", "backup", res.Backup, "safety_backup", res.Safety)
		}
	}

	svc, closeDB, err := openService(ctx, cfg, true)
	if err != nil {
		return err
	}
	// Background loops are joined (bounded) and the Service closed before the database, on every exit path.
	var bg sync.WaitGroup
	defer func() {
		cancelAll()
		waitGroup(&bg, 10*time.Second)
		svc.Close()
		closeDB()
	}()
	svc.SetBackupRestart(requestRestart)
	// Network sharing: the flag only seeds the stored setting once; afterwards the stored value wins. A fresh data
	// folder of a child process is seeded off.
	seed := base.NetworkSharing
	if cfg.Child {
		seed = false
	}
	if err := svc.SeedNetworkSharing(ctx, seed); err != nil {
		return fmt.Errorf("store network sharing: %w", err)
	}
	svc.SetLocalDefaults(hub.LocalDefaults{NetworkSharing: base.NetworkSharing, ImportDir: base.ImportDir()})
	stored, err := svc.NetOverrides(ctx)
	if err != nil {
		return fmt.Errorf("read network settings: %w", err)
	}
	eff, issues := effectiveConfig(&base, stored, log)
	if cfg.Child {
		// Whatever is stored (it matters again when the server runs on its own), a child process is loopback only.
		eff.NetworkSharing = false
	}
	importDir := eff.ImportDir()
	if err := os.MkdirAll(importDir, 0o750); err != nil {
		if eff.ImportDir() == base.ImportDir() {
			return fmt.Errorf("create library import folder: %w", err)
		}
		log.Error("saved import folder cannot be used; using the default", "dir", importDir, "err", err)
		importDir = base.ImportDir()
		if err := os.MkdirAll(importDir, 0o750); err != nil {
			return fmt.Errorf("create library import folder: %w", err)
		}
	}
	svc.SetSaveRetention(eff.SaveKeepRecent, eff.SaveKeepDaily, eff.SaveKeepWeekly)
	svc.SetICEServers(eff.ICEServers)
	if has, err := svc.HasAdmin(ctx); err == nil && !has {
		log.Warn("no admin yet: set up the server from Uzume with the setup code printed below, or run 'uzume-server setup-admin -username <name>'")
	}

	trusted, err := clientip.ParseList(cfg.TrustedProxy)
	if err != nil {
		return err
	}
	hostCfg := serverhost.Config{UseTLS: eff.UseTLS()}
	for _, p := range trusted {
		hostCfg.TrustedProxy = append(hostCfg.TrustedProxy, p.String())
	}
	if len(trusted) > 0 {
		log.Info("trusted reverse proxy configured", "networks", hostCfg.TrustedProxy)
	}
	if eff.TURN && !eff.NetworkSharing {
		log.Info("built-in TURN relay stays off while Network sharing is off")
	} else if eff.TURN {
		ts, err := startTURN(ctx, &eff, svc, log)
		switch {
		case err == nil:
			defer ts.Close()
			svc.SetTURN(ts)
			defer svc.SetTURN(nil)
			hostCfg.TURN = ts
		case hasTURNOverride(stored):
			// Saved values must never keep the hub from starting: run without TURN and say so in Settings.
			log.Error("built-in TURN relay could not be started with the saved settings; starting without it", "err", err)
			issues = append(issues, serverhost.NetIssue{Key: config.NetTURN, Value: serverhost.NetSignature(&eff, config.NetTURN),
				Message: fmt.Sprintf("The built-in TURN relay could not be started with the saved settings: %v. The server runs without it until you change them.", err)})
			eff.TURN = false
		default:
			return err
		}
	}

	if cfg.CloudflareTURNKeyID != "" {
		if !eff.NetworkSharing {
			log.Info("Cloudflare TURN stays off while Network sharing is off")
		} else {
			token, err := cfg.CloudflareTURNToken(os.Getenv)
			if err != nil {
				log.Warn("Cloudflare TURN stays off: API token unavailable", "err", err)
			} else {
				svc.StartCloudflareTURN(ctx, hub.CloudflareTURNOptions{KeyID: cfg.CloudflareTURNKeyID, Token: token,
					BaseURL: cfg.CloudflareTURNBaseURL, Log: log})
				log.Info("Cloudflare TURN relay enabled", "key_id", cfg.CloudflareTURNKeyID)
			}
		}
	}

	// External TURN: the list saved in Settings wins over -turn-external; the service always starts so that the
	// app can edit it. While Network sharing is off the servers are configured but not handed out.
	specs, _ := cfg.ExternalTURNSpecs()
	if !eff.NetworkSharing && (len(specs) > 0) {
		log.Info("external TURN servers stay off while Network sharing is off")
	}
	svc.StartExternalTURN(ctx, externalTURNServers(cfg, specs, log), hub.ExternalTURNOptions{Disabled: !eff.NetworkSharing})

	var tlsConf *tls.Config
	securityCode := ""
	if cfg.UseTLS() {
		var cert tls.Certificate
		if cfg.TLSCert != "" {
			cert, err = tlsutil.Load(cfg.TLSCert, cfg.TLSKey)
		} else {
			var ren *tlsutil.Renewal
			cert, ren, err = tlsutil.EnsureSelfSignedRenewing(cfg.DataDir, time.Now())
			if err == nil && ren != nil {
				log.Warn("Self-generated TLS certificate was expired or about to expire and has been renewed; apps must confirm the new fingerprint",
					"old_sha256_fingerprint", ren.OldFingerprint, "new_sha256_fingerprint", ren.NewFingerprint,
					"old_not_after", ren.OldNotAfter.Format(time.RFC3339), "previous_files", "tls/cert.pem.prev, tls/key.pem.prev")
			}
		}
		if err != nil {
			return fmt.Errorf("TLS certificate: %w", err)
		}
		holder := newCertHolder(cert)
		tlsConf = &tls.Config{GetCertificate: holder.getCertificate, MinVersion: tls.VersionTLS12}
		hostCfg.CertSource = "Self-generated"
		if cfg.TLSCert != "" {
			hostCfg.CertSource = "Own cert/key"
		} else {
			hostCfg.RenewCert = func() (string, time.Time, error) { return holder.renew(cfg.DataDir, time.Now()) }
		}
		hostCfg.CertState = holder.state
		securityCode = tlsutil.SecurityCode(cert)
		log.Info("TLS certificate", "sha256_fingerprint", tlsutil.Fingerprint(cert), "security_code", securityCode)
	} else {
		log.Warn("Development mode: HTTP without TLS", "loopback", cfg.ListenIsLoopback())
	}

	// Listener: the effective (possibly saved) address, else the one from server.env/flags.
	listen := func(addr string) (net.Listener, error) { return net.Listen("tcp", addr) }
	if !eff.NetworkSharing {
		log.Info("Network sharing is off: listening on loopback only", "child", cfg.Child)
		listen = listenLoopback
	}
	ln, listenIssue, err := chooseListen(eff.Listen, base.Listen, listen)
	if err != nil {
		if cfg.Child && isAddrInUse(err) && !strings.Contains(err.Error(), "address in use") {
			err = fmt.Errorf("address in use: %w", err)
		}
		return err
	}
	if listenIssue != nil {
		log.Error("saved listen port could not be bound; using the address from server.env/flags", "err", listenIssue.cause, "fallback", base.Listen)
		issues = append(issues, listenIssue.issue(eff, base.Listen))
		eff.Listen = base.Listen
	}
	hostCfg.Listen = eff.Listen
	hostCfg.ImportDir = importDir
	hostCfg.PublicHost, hostCfg.PublicPort = publicAddress(eff)
	hostCfg.Net = serverhost.NetConfig{Base: base, Running: eff, Issues: issues, RequestRestart: requestRestart}

	host := serverhost.New(svc, hostCfg, log)
	mux := http.NewServeMux()
	httpapi.Register(mux, svc, log, httpapi.WithRestart(requestRestart), httpapi.WithStatus(host.StatusSource()), httpapi.WithHost(host))

	srv := &http.Server{
		Handler:           withReadDeadline(clientip.Middleware(trusted)(httpapi.LogRequests(log, mux))),
		ReadHeaderTimeout: 10 * time.Second,
		IdleTimeout:       120 * time.Second,
		ErrorLog:          slog.NewLogLogger(log.Handler(), slog.LevelWarn),
		TLSConfig:         tlsConf,
	}
	info := svc.Info()
	log.Info("Uzume server started", "version", version.String(), "hub_id", info.HubID, "name", info.Name,
		"listen", ln.Addr().String(), "network_sharing", eff.NetworkSharing, "tls", cfg.UseTLS(), "protocol_version", info.ProtocolVersion)
	if cfg.Child {
		// The app sets the server up through the local API; stdout carries only the line below.
		announceSetup(ctx, svc, eff, securityCode, log, io.Discard)
	} else {
		announceSetup(ctx, svc, eff, securityCode, log, stdout)
	}

	bg.Add(2)
	go func() {
		defer bg.Done()
		svc.RunCleanup(ctx, time.Minute, func(err error) { log.Error("cleanup", "err", err) })
	}()
	go func() {
		defer bg.Done()
		svc.RunSaveSweep(ctx, 24*time.Hour, func(err error) { log.Error("save history sweep", "err", err) })
	}()
	// Scheduled backups (Settings > Backups): runs until the server stops.
	bg.Add(1)
	go func() {
		defer bg.Done()
		svc.RunBackupSchedule(ctx, func(msg string, args ...any) { log.Warn(msg, args...) })
	}()
	// Core source sync: in the background, never blocks or fails startup; ends with ctx on shutdown.
	bg.Add(4)
	go func() {
		defer bg.Done()
		svc.RunToolSync(ctx, 24*time.Hour, func(toolID string, err error) {
			log.Warn("tools catalog refresh failed", "tool", toolID, "err", err)
		})
	}()
	go func() {
		defer bg.Done()
		svc.RunCoreSync(ctx, 24*time.Hour, func(r hub.CoreSyncReport, err error) {
			if err != nil {
				log.Warn("core catalog refresh failed", "err", err)
				return
			}
			log.Info("core catalog refreshed", "cores", r.Cores)
			for _, w := range r.Warnings {
				log.Warn("core catalog refresh incomplete", "warning", w)
			}
			for _, u := range r.Updated {
				log.Info("core auto-updated", "system", u.SystemID, "core", u.CoreID, "version", u.Version)
			}
			for _, e := range r.UpdateErrors {
				log.Warn("core auto-update failed", "err", e)
			}
		})
	}()

	// Cheat database: downloaded in the background like the core catalog; "none" = off.
	bg.Add(1)
	go func() {
		defer bg.Done()
		svc.RunCheatDBRefresh(ctx, func(err error) {
			if err != nil {
				log.Warn("cheat database refresh failed", "err", err)
			}
		})
	}()

	// Update check: in the background like the core sync, never blocks or fails startup.
	go func() {
		defer bg.Done()
		svc.RunUpdateChecks(ctx, func(r hub.UpdateCheckReport, err error) {
			switch {
			case err != nil:
				log.Warn("update check failed", "err", err)
			case r.Staged:
				log.Info("update staged, installation requested", "version", r.Available.Version)
			case r.Available != nil:
				log.Info("update available", "version", r.Available.Version, "channel", r.Available.Channel, "breaking_for_players", r.Available.Breaking)
			}
		})
	}()

	// Plugin index: in the background like the core sync, never blocks or fails startup; "none" = no index.
	go func() {
		defer bg.Done()
		svc.RunPluginIndexRefresh(ctx, hub.PluginIndexInterval, func(err error) {
			if err != nil {
				log.Warn("plugin index refresh failed", "err", err)
			}
		})
	}()

	errc := make(chan error, 1)
	if cfg.Child {
		// The listener is bound (the kernel queues connections already): tell the app the real port.
		fmt.Fprintf(stdout, "UZUME_READY %d\n", listenPort(ln.Addr()))
	}
	go func() {
		if cfg.UseTLS() {
			errc <- srv.ServeTLS(limitListener(ln, maxConns), "", "")
		} else {
			errc <- srv.Serve(limitListener(ln, maxConns))
		}
	}()
	restarting := false
	select {
	case err := <-errc:
		return err
	case <-restartCh:
		// Requested through the admin API: shut down like on SIGTERM, then main re-executes the binary.
		restarting = true
		cancelAll()
		log.Info("Restart requested")
	case <-ctx.Done():
	}
	log.Info("Shutting down Uzume server")
	shutCtx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if err := srv.Shutdown(shutCtx); err != nil {
		srv.Close()
	}
	cancelAll() // the loops stop with ctx and are joined before the database closes (deferred above)
	if restarting {
		return errRestart
	}
	return nil
}

// announceSetup creates a fresh setup code and prints the setup banner while the server has no admin. A restart
// replaces the code ("server restarted, new setup code needed"). The banner goes to stdout (terminal, journal,
// docker logs) and to the log; the code is written to the log deliberately and nowhere else.
func announceSetup(ctx context.Context, svc *hub.Service, eff config.Config, securityCode string, log *slog.Logger, out io.Writer) {
	has, err := svc.HasAdmin(ctx)
	if err != nil || has {
		return
	}
	code, err := svc.NewSetupCode(ctx)
	if err != nil {
		log.Error("cannot create the setup code", "err", err)
		return
	}
	addrs := serverAddresses(eff, inContainer(), net.InterfaceAddrs)
	fmt.Fprintln(out, setupBanner(version.String(), addrs, securityCode, code))
	log.Warn("waiting for setup", "addresses", addrs, "security_code", securityCode, "setup_code", code.Code,
		"setup_code_valid_until", code.Expires.Format(time.RFC3339), "hint", "enter the setup code in Uzume: Set up a server > On another device; new code: uzume-server setup-code")
}

// waitGroup waits for wg for at most d (a loop stuck in a download must not block the shutdown).
func waitGroup(wg *sync.WaitGroup, d time.Duration) {
	done := make(chan struct{})
	go func() { wg.Wait(); close(done) }()
	select {
	case <-done:
	case <-time.After(d):
	}
}
