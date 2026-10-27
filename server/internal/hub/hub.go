// Package hub is the service layer of the Uzume server (hub identity, users, devices, pairing,
// tokens, library). The app and HTTP API use the same functions.
package hub

import (
	"context"
	"crypto/ed25519"
	"database/sql"
	"errors"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"github.com/google/uuid"

	"github.com/phabioo/uzume/server/internal/auth"
	"github.com/phabioo/uzume/server/internal/backup"
	"github.com/phabioo/uzume/server/internal/clientip"
)

// Protocol constants in one place (ADR 0002); the info endpoint and handshake read them here.
const (
	ProtocolVersion    = 1
	MinProtocolVersion = 1
)

// Lifetimes and limits (ADR 0002 / orchestrator requirements).
const (
	AccessTokenTTL         = 15 * time.Minute
	PairingTTL             = 10 * time.Minute
	MaxOpenPairingRequests = 20 // anonymous pairing requests only
	// MaxOpenPairingPerSource caps the open anonymous requests of one IPv4 address or IPv6 /64, so a few sources
	// cannot fill the global budget.
	MaxOpenPairingPerSource  = 5
	MaxPairingPerIPPerMinute = 5 // per IPv4 address or IPv6 /64
	// MaxOpenInviteRequests caps pending requests created by invite redemptions (own budget, so anonymous
	// pairing requests cannot starve invited users and vice versa).
	MaxOpenInviteRequests = 50
	MinPasswordLen        = 12
)

// Options configure the service.
type Options struct {
	DataDir string
	// Name is the hub name on first start (empty: hostname, or "Uzume server").
	Name       string
	HubVersion string
	// Now is the clock (injectable for tests); default time.Now.
	Now func() time.Time
	// CleanStaging removes the staging leftovers in <data>/tmp and restores a firmware backup at Open. Only the
	// serving process sets it: CLI sub-commands open the data directory while the service may be running.
	CleanStaging bool
	// PasswordParams: Argon2id parameters; default auth.DefaultParams.
	PasswordParams *auth.Params
	// ProtocolVersion/MinProtocolVersion override the constants (tests only); 0 = default.
	ProtocolVersion    int
	MinProtocolVersion int
	// ICEServers are the stun: URLs delivered in hello_ack and the join response (nil = config.DefaultSTUNServer, empty = none, ADR 0006 D4).
	ICEServers []string
	// OwnerGrace is how long a Session survives the owner's dropped WSS connection (default 30 s; injectable for tests).
	OwnerGrace time.Duration
	// CoreBuildbotURL is the libretro buildbot nightly base URL (default https://buildbot.libretro.com/nightly), CoreInfoURL
	// the core info archive (default https://buildbot.libretro.com/assets/frontend/info.zip). Both https only (ADR 0020).
	CoreBuildbotURL, CoreInfoURL string
	// CoreMaxLibraryBytes caps the extracted library of one core zip (default 1 GiB; zip bomb guard) and
	// CoreMaxZipBytes the downloaded zip (default 512 MiB). Injectable for tests.
	CoreMaxLibraryBytes, CoreMaxZipBytes int64
	// CoreTrustKeys are the trusted signing keys of the updates index (corepkg.TrustedKeys). Empty = no index is accepted.
	CoreTrustKeys []ed25519.PublicKey
	// CoreHTTPClient fetches the core catalog, core zips and the updates index (injectable for tests); default:
	// client with timeouts. Redirects are followed, https only.
	CoreHTTPClient *http.Client
	// UpdateIndexURL is the signed updates index (default updates.DefaultIndexURL; https or file://), fetched with
	// CoreHTTPClient and verified with CoreTrustKeys.
	UpdateIndexURL string
	// UpdateRequestDir is where the update request file is created (default /run/uzume).
	UpdateRequestDir string
	// UpdateChannel is the compiled-in default channel: stable, beta or dev (dev = no automatic checks).
	UpdateChannel string
	// Child marks the server running as the app's child process: never self-updates, Network sharing cannot be
	// switched on (CodeChildMode), the local status reports sharing off.
	Child bool
	// Executable is the path of the running binary (default os.Executable); updates are installable only when it
	// is /usr/bin/uzume-server. UpdatePlatform defaults to linux-<GOARCH>. Both are injectable for tests.
	Executable     string
	UpdatePlatform string
	// SaveKeepRecent, SaveKeepDaily and SaveKeepWeekly are the history retention rules (ADR 0012 D7): newest versions,
	// days and weeks to keep one version of. 0 = unlimited for that rule (SaveKeepRecent 0 = no thinning at all).
	SaveKeepRecent, SaveKeepDaily, SaveKeepWeekly int
	// PluginIndexURL is the signed plugin index (https or file://; empty = no index, the server binary passes "none" as empty), fetched with CoreHTTPClient and
	// verified with CoreTrustKeys.
	PluginIndexURL string
	// ToolsSourceURL is the release list of the tools catalog (GitHub releases API of the randomizer engine; "none" or
	// "" = off), fetched with CoreHTTPClient. ToolMaxZipBytes caps one downloaded or imported platform zip (default 400 MiB).
	ToolsSourceURL  string
	ToolMaxZipBytes int64
	// BackupExtras are files outside the data directory that go into every backup (own TLS certificate and key, server.env).
	BackupExtras []backup.Extra
	// PlayTickInterval is how often the last_seen_at of open plays is refreshed (default 60 s; injectable for tests).
	PlayTickInterval time.Duration
	// BackupFree reports the free space of a folder (injectable for tests; default: the file system).
	BackupFree backup.FreeFunc
}

// Service is the service layer. Times are stored in SQLite as Unix seconds (UTC).
type Service struct {
	db       *sql.DB
	dataDir  string
	hubVer   string
	now      func() time.Time
	params   auth.Params
	protoVer int
	minProto int

	saveMu   sync.Mutex // serializes save uploads/resolutions (and content cleanup)
	saveKeep saveRetention
	fwMu     sync.Mutex // serializes ProvideFirmware (file swap + metadata upsert)
	sess     sessionState
	mu       sync.RWMutex
	hubID    string
	name     string
	dummyO   sync.Once
	dummy    string

	scan   scanState
	cores  coreState
	tools  toolState
	upd    updateState
	plug   pluginState
	updCfg updateConfig
	turn   turnState
	cf     cloudflareState
	ext    externalState
	bus    eventBus
	runs   runState
	bk     backupState
	rep    reportState
	play   playState

	child       bool       // running as the app's child process (Options.Child)
	localMu     sync.Mutex // local setup defaults
	localDef    LocalDefaults
	redeemMu    sync.Mutex // invite redemption rate limits
	setupMu     sync.Mutex // setup code redemption (one at a time: a code is used once)
	redeemHits  map[string][]time.Time
	redeemFails []redeemFail // failed attempts of the last minute, oldest first

	acct accountState // sign-in rate limits

	upMu   sync.Mutex // resumable uploads: sessions with a chunk in flight
	upBusy map[string]bool
}

type redeemFail struct {
	t   time.Time
	key string // rate limit key of the source
}

// Open initializes the service on a migrated database and creates the hub identity and
// data directories.
func Open(ctx context.Context, db *sql.DB, o Options) (*Service, error) {
	s := &Service{db: db, dataDir: o.DataDir, hubVer: o.HubVersion, now: o.Now, params: auth.DefaultParams,
		protoVer: ProtocolVersion, minProto: MinProtocolVersion}
	if s.now == nil {
		s.now = time.Now
	}
	if o.PasswordParams != nil {
		s.params = *o.PasswordParams
	}
	if o.ProtocolVersion != 0 {
		s.protoVer = o.ProtocolVersion
	}
	if o.MinProtocolVersion != 0 {
		s.minProto = o.MinProtocolVersion
	}
	s.saveKeep = saveRetention{recent: o.SaveKeepRecent, daily: o.SaveKeepDaily, weekly: o.SaveKeepWeekly}
	s.sess.init(o)
	s.initPlay(o)
	s.cores.init(o)
	s.initTools(o)
	s.child = o.Child
	s.initUpdates(o)
	s.initPlugins(o)
	s.initBackup(o)
	// The plugin store is the run plugin catalog, and a version used by a running run cannot be removed.
	s.SetPluginCatalog(s)
	s.SetPluginInUseCheck(s.RunsUsingPluginVersion)
	s.PruneOldPlugins(ctx) // clean up servers that already hold superseded versions
	if o.CleanStaging {
		s.cleanReportStaging()
		s.cleanStaging()
	}
	for _, d := range []string{"roms", "tmp", "screenshots"} {
		if err := os.MkdirAll(filepath.Join(s.dataDir, d), 0o750); err != nil {
			return nil, internal(err)
		}
	}
	if err := reconcileSystems(ctx, db); err != nil {
		return nil, internal(err)
	}
	err := db.QueryRowContext(ctx, `SELECT id, name FROM hub LIMIT 1`).Scan(&s.hubID, &s.name)
	if errors.Is(err, sql.ErrNoRows) {
		name := strings.TrimSpace(o.Name)
		if name == "" {
			name, _ = os.Hostname()
		}
		if name == "" {
			name = "Uzume server"
		}
		s.hubID, s.name = uuid.NewString(), name
		_, err = db.ExecContext(ctx, `INSERT INTO hub(id, name) VALUES (?, ?)`, s.hubID, s.name)
	}
	if err != nil {
		return nil, internal(err)
	}
	if err := s.removeLegacyCores(ctx); err != nil {
		return nil, err
	}
	s.BackfillGameCodes(ctx) // header reads only; failures are logged, never fatal
	s.BackfillGameVersions(ctx)
	s.recoverSessions(ctx)
	if o.CleanStaging { // the serving process only (see startPlay)
		s.startPlay(ctx)
	}
	return s, nil
}

// cleanStaging removes the staging leftovers of a crashed run. Only the serving process may call it: CLI
// sub-commands open the data directory next to the running service, whose uploads live in tmp.
func (s *Service) cleanStaging() {
	tmpDir := filepath.Join(s.dataDir, "tmp")
	if entries, err := os.ReadDir(tmpDir); err == nil {
		for _, e := range entries {
			if e.Name() == "uploads" {
				continue // resumable upload sessions survive restarts (CleanupUploads removes the idle ones)
			}
			os.RemoveAll(filepath.Join(tmpDir, e.Name()))
		}
	}
	// A firmware backup left behind by a crash between the renames of ProvideFirmware: when the target file is
	// missing the backup is the only copy and is restored, otherwise the leftover is removed.
	if baks, err := filepath.Glob(filepath.Join(s.dataDir, "firmware", "*", "*.bak")); err == nil {
		for _, b := range baks {
			target := strings.TrimSuffix(b, ".bak")
			if _, err := os.Stat(target); errors.Is(err, os.ErrNotExist) {
				if os.Rename(b, target) == nil {
					continue
				}
			}
			os.Remove(b)
		}
	}
}

// DataDir returns the data directory.
func (s *Service) DataDir() string { return s.dataDir }

// Now returns the service's current time (clock injectable).
func (s *Service) Now() time.Time { return s.now().UTC() }

// Info describes the hub for the info endpoint.
type Info struct {
	HubID              string
	Name               string
	HubVersion         string
	ProtocolVersion    int
	MinProtocolVersion int
}

// Info returns the hub identity and protocol versions.
func (s *Service) Info() Info {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return Info{HubID: s.hubID, Name: s.name, HubVersion: s.hubVer,
		ProtocolVersion: s.protoVer, MinProtocolVersion: s.minProto}
}

// SetHubName changes the hub name.
func (s *Service) SetHubName(ctx context.Context, name string) error {
	name = strings.TrimSpace(name)
	if name == "" || len(name) > 100 {
		return badRequest("Name must be 1 to 100 characters long")
	}
	if _, err := s.db.ExecContext(ctx, `UPDATE hub SET name = ?`, name); err != nil {
		return internal(err)
	}
	s.mu.Lock()
	s.name = name
	s.mu.Unlock()
	return nil
}

// Cleanup removes expired access tokens, old pairing requests and idle upload sessions.
func (s *Service) Cleanup(ctx context.Context) error {
	now := s.now().Unix()
	for _, q := range []struct {
		sql string
		arg int64
	}{
		{`DELETE FROM access_tokens WHERE expires_at <= ?`, now},
		// Expired requests stay visible for 1 h so the player can still poll "expired".
		{`DELETE FROM pairing_requests WHERE expires_at <= ?`, now - 3600},
		// Invite history is kept for 90 days (users created by a redemption stay).
		// Identities of removed games are kept only while saves of them exist (HC-2).
		{`DELETE FROM removed_games WHERE game_id NOT IN (SELECT game_id FROM save_slots) AND game_id NOT IN (SELECT game_id FROM save_history)
			AND game_id NOT IN (SELECT game_id FROM save_conflicts) AND removed_at <= ?`, now},
		{`DELETE FROM invites WHERE created_at <= ?`, now - int64(inviteHistoryKeep.Seconds())},
	} {
		if _, err := s.db.ExecContext(ctx, q.sql, q.arg); err != nil {
			return internal(err)
		}
	}
	return s.CleanupUploads(ctx)
}

// RunCleanup calls Cleanup periodically until ctx ends. Errors go to onErr (may be nil).
func (s *Service) RunCleanup(ctx context.Context, every time.Duration, onErr func(error)) {
	t := time.NewTicker(every)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			if err := s.Cleanup(ctx); err != nil && onErr != nil && ctx.Err() == nil {
				onErr(err)
			}
		}
	}
}

func ts(t sql.NullInt64) *time.Time {
	if !t.Valid {
		return nil
	}
	v := time.Unix(t.Int64, 0).UTC()
	return &v
}

func isUnique(err error) bool {
	return err != nil && strings.Contains(err.Error(), "UNIQUE constraint failed")
}

// IsLocalRequest reports whether r comes from the machine the server runs on (see clientip.Local): never through a
// trusted proxy; otherwise a loopback peer without reverse-proxy headers and with a loopback Host header.
func IsLocalRequest(r *http.Request) bool { return clientip.Local(r) }
