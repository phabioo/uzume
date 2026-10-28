package hub

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/phabioo/uzume/server/internal/updates"
)

// Update settings and state keys in the settings table.
const (
	settingUpdateChannel = "update_channel" // "", stable or beta
	// settingUpdateChannelDefault is the resolved default of a beta build ("stable" or "beta"), stored once by the
	// first verified index load; it applies while no channel is selected explicitly.
	settingUpdateChannelDefault = "update_channel_default"
	settingUpdateAuto           = "update_auto" // "", "1" or "0"
	settingUpdateLastCheck      = "update_last_check"
	settingUpdateLastError      = "update_last_error"
	settingUpdateAvailable      = "update_available" // JSON of UpdateAvailable
)

// Update check intervals (S5).
const (
	updateIntervalTest   = time.Hour
	updateIntervalStable = 24 * time.Hour
	// playersSeenWindow is how long an app counts as in use for the "breaking for apps" warning (S4).
	playersSeenWindow = 30 * 24 * time.Hour
)

// Update channel setting values shown to admins. UpdateChannelOff is only an effective value.
const (
	UpdateChannelStable = updates.ChannelStable
	UpdateChannelBeta   = updates.ChannelBeta
	UpdateChannelOff    = "off"
)

// Errors of the update functions.
var (
	// ErrUpdatesOff: the updater is off (dev build without a selected channel, or a non-release version).
	ErrUpdatesOff = errors.New("updates are off")
	// ErrNotPackaged: the server does not run from the .deb package, so it cannot install updates itself.
	ErrNotPackaged = errors.New("this server was not installed from the .deb package; updates cannot be installed from here")
	// ErrNoUpdate: there is nothing newer to install.
	ErrNoUpdate = errors.New("no update is available")
	// ErrUpdateBreaking: the candidate needs a newer app than some app in use; confirmation required.
	ErrUpdateBreaking = errors.New("this update breaks apps that were seen recently; confirm to install it anyway")
)

type updateState struct {
	mu     sync.Mutex // serializes checks, staging and settings changes that start one
	kick   chan struct{}
	client *http.Client
}

func (u *updateState) init(o Options) { u.kick = make(chan struct{}, 1); u.client = o.CoreHTTPClient }

// updateConfig holds the resolved Options of the updater.
type updateConfig struct {
	indexURL, requestDir, compiledChannel, executable, platform string
}

func (s *Service) initUpdates(o Options) {
	s.upd.init(o)
	s.updCfg = updateConfig{indexURL: o.UpdateIndexURL, requestDir: o.UpdateRequestDir, compiledChannel: updates.NormalizeChannel(o.UpdateChannel),
		executable: o.Executable, platform: o.UpdatePlatform}
	if s.updCfg.indexURL == "" {
		s.updCfg.indexURL = updates.DefaultIndexURL
	}
	if s.updCfg.requestDir == "" {
		s.updCfg.requestDir = updates.DefaultRequestDir
	}
	if s.updCfg.executable == "" {
		s.updCfg.executable, _ = os.Executable()
	}
	if s.updCfg.platform == "" {
		s.updCfg.platform = updates.Platform()
	}
}

// ---- Settings ----

// UpdateSettings are the admin-visible update settings.
type UpdateSettings struct {
	CompiledChannel string // stable, beta or dev
	Channel         string // effective: stable, beta or off
	ChannelIsSet    bool   // an admin selected the channel (otherwise the default applies)
	DefaultChannel  string // channel used without a selection: the resolved default, else the compiled channel; stable, beta or off
	Auto            bool   // effective automatic install
}

// UpdateSettings returns the effective settings: channel = setting, else the resolved default of a beta build
// (see resolveChannelDefault), else the compiled channel (dev = off); automatic install = setting, else on for
// beta and off otherwise.
func (s *Service) UpdateSettings(ctx context.Context) (UpdateSettings, error) {
	st := UpdateSettings{CompiledChannel: s.updCfg.compiledChannel, Channel: UpdateChannelOff}
	if st.CompiledChannel == "" {
		st.CompiledChannel = updates.ChannelDev
	}
	if updates.ValidSelectableChannel(st.CompiledChannel) {
		st.Channel = st.CompiledChannel
	}
	if d, _, err := s.getSetting(ctx, settingUpdateChannelDefault); err != nil {
		return st, err
	} else if d = updates.NormalizeChannel(d); st.CompiledChannel == updates.ChannelBeta && updates.ValidSelectableChannel(d) {
		st.Channel = d
	}
	st.DefaultChannel = st.Channel
	v, _, err := s.getSetting(ctx, settingUpdateChannel)
	if err != nil {
		return st, err
	}
	if v = updates.NormalizeChannel(v); updates.ValidSelectableChannel(v) {
		st.Channel, st.ChannelIsSet = v, true
	}
	a, set, err := s.getSetting(ctx, settingUpdateAuto)
	if err != nil {
		return st, err
	}
	st.Auto = st.Channel == UpdateChannelBeta
	if set && (a == "1" || a == "0") {
		st.Auto = a == "1"
	}
	return st, nil
}

// SetUpdateSettings stores the channel ("" = use the default (resolved default or compiled channel), else stable or beta) and the automatic
// install switch. A new check is started in the background.
func (s *Service) SetUpdateSettings(ctx context.Context, channel string, auto bool) (err error) {
	defer s.publishOK(&err, TopicUpdates)
	if channel != "" && !updates.ValidSelectableChannel(channel) {
		return badRequest("Update channel must be stable or beta")
	}
	if err := s.setSetting(ctx, settingUpdateChannel, channel); err != nil {
		return err
	}
	v := "0"
	if auto {
		v = "1"
	}
	if err := s.setSetting(ctx, settingUpdateAuto, v); err != nil {
		return err
	}
	s.TriggerUpdateCheck()
	return nil
}

// ---- Status ----

// UpdateAvailable describes the newest selected release.
type UpdateAvailable struct {
	Version     string    `json:"version"`
	Channel     string    `json:"channel"`
	NotesURL    string    `json:"notes_url,omitempty"`
	Notes       string    `json:"notes,omitempty"`
	PublishedAt time.Time `json:"published_at"`
	Artifact    string    `json:"artifact"`
	URL         string    `json:"url"`
	Size        int64     `json:"size"`
	// Breaking: min_protocol_version is above the protocol version of an app seen in the last 30 days (S4).
	Breaking      bool `json:"breaking"`
	BreakingCount int  `json:"breaking_count"`
	// Skipped: going back skipped this version; it is listed as "skipped · available" and never installed
	// automatically. Computed when the status is read, not stored.
	Skipped bool `json:"skipped,omitempty"`
}

// Warning returns the S4 warning text, or "".
func (a UpdateAvailable) Warning() string {
	if !a.Breaking {
		return ""
	}
	return fmt.Sprintf("%d app device(s) seen in the last 30 days are too old for this server version and would lose access until they are updated. Automatic install skips this update.", a.BreakingCount)
}

// UpdateStatus is the state shown on the Settings screen of the app.
type UpdateStatus struct {
	Settings       UpdateSettings
	CurrentVersion string
	// Disabled explains why the updater does not run ("" = it runs).
	Disabled  string
	LastCheck *time.Time
	LastError string
	Available *UpdateAvailable
	// Packaged: the server can install updates itself (.deb install with a writable request directory).
	Packaged       bool
	Staged         *updates.Staged
	RequestPending bool
	LastResult     *updates.Result
	ActiveSessions int
	// ManualCommand is shown when the update cannot be installed from the page.
	ManualCommand string
	// Paused is set after going back: automatic updates stay off until they are resumed. Skipped is the version
	// that was left.
	Paused  *updates.Pause
	Skipped string
	// InContainer: the server runs in the official image and is rolled back by pulling the older image tag.
	InContainer bool
}

// UpdateStatus gathers the update state.
func (s *Service) UpdateStatus(ctx context.Context) (UpdateStatus, error) {
	var st UpdateStatus
	var err error
	if st.Settings, err = s.UpdateSettings(ctx); err != nil {
		return st, err
	}
	st.CurrentVersion = s.hubVer
	switch {
	case !updates.ValidSemVer(s.hubVer):
		st.Disabled = "This server runs a development build (" + s.hubVer + "), which has no release version to compare."
	case st.Settings.Channel == UpdateChannelOff:
		st.Disabled = "Automatic update checks are off for development builds. Select a channel to enable them."
	}
	if v, ok, err := s.getSetting(ctx, settingUpdateLastCheck); err != nil {
		return st, err
	} else if n, perr := strconv.ParseInt(v, 10, 64); ok && perr == nil {
		t := time.Unix(n, 0).UTC()
		st.LastCheck = &t
	}
	if st.LastError, _, err = s.getSetting(ctx, settingUpdateLastError); err != nil {
		return st, err
	}
	st.Available = s.storedAvailable(ctx)
	st.Paused = s.UpdatePause(ctx)
	st.Skipped = s.UpdateSkipped(ctx)
	st.InContainer = updates.InContainer()
	if st.Available != nil && skippedVersion(st.Available.Version, st.Skipped) {
		st.Available.Skipped = true
	}
	st.Packaged = s.packaged()
	st.Staged, _ = updates.ReadStaged(s.dataDir)
	st.RequestPending = updates.RequestPending(s.updCfg.requestDir)
	st.LastResult, _ = updates.ReadResult(s.dataDir)
	if r := st.LastResult; r != nil && !r.OK {
		s.recordUpdateFailure(ctx, r.Version, r.Time, r.Message)
	}
	st.ActiveSessions = s.countActiveSessions(ctx)
	if st.Available != nil {
		st.ManualCommand = manualCommand(s.updCfg.platform, st.Available)
	}
	return st, nil
}

// UpdateAvailableBadge reports whether a newer release is known (sidebar dot).
func (s *Service) UpdateAvailableBadge(ctx context.Context) bool {
	return s.storedAvailable(ctx) != nil
}

// storedAvailable returns the last found update if it is still newer than the running version.
func (s *Service) storedAvailable(ctx context.Context) *UpdateAvailable {
	v, ok, err := s.getSetting(ctx, settingUpdateAvailable)
	if err != nil || !ok || v == "" {
		return nil
	}
	var a UpdateAvailable
	if json.Unmarshal([]byte(v), &a) != nil {
		return nil
	}
	if c, err := updates.CompareVersions(a.Version, s.hubVer); err != nil || c <= 0 {
		return nil
	}
	return &a
}

// packaged reports whether the running binary is an installed package the updater may replace. A child process of the
// app never is: the app updates the binary (ADR 0030 D6), so nothing is staged or requested.
func (s *Service) packaged() bool {
	if s.child {
		return false
	}
	return updates.IsPackaged(s.updCfg.platform, s.updCfg.executable, s.updCfg.requestDir)
}

func (s *Service) countActiveSessions(ctx context.Context) int {
	var n int
	if err := s.db.QueryRowContext(ctx, `SELECT COUNT(*) FROM sessions WHERE ended_at IS NULL`).Scan(&n); err != nil {
		return 0
	}
	return n
}

// playersBelow counts apps seen in the last 30 days whose protocol version is below min.
func (s *Service) playersBelow(ctx context.Context, min int) int {
	var n int
	err := s.db.QueryRowContext(ctx, `SELECT COUNT(*) FROM device_reports r JOIN devices d ON d.id = r.device_id
		WHERE d.status = 'trusted' AND r.reported_at >= ? AND r.protocol_version < ?`,
		s.now().Add(-playersSeenWindow).Unix(), min).Scan(&n)
	if err != nil {
		return 0
	}
	return n
}

// ---- Check and install ----

func (s *Service) updateSource() updates.Source {
	return updates.Source{IndexURL: s.updCfg.indexURL, Client: s.upd.client}
}

// selectUpdate loads the signed index and selects the update for the effective channel.
func (s *Service) selectUpdate(ctx context.Context) (*updates.Fetched, updates.Selection, error) {
	set, err := s.UpdateSettings(ctx)
	if err != nil {
		return nil, updates.Selection{}, err
	}
	q := updates.Query{Product: updates.ProductHub, Channel: set.Channel, Platform: s.updCfg.platform, Kind: updates.KindFor(s.updCfg.platform), Current: s.hubVer}
	if _, err := updates.Select(updates.Index{}, q); err != nil { // off / not SemVer: do not even fetch
		return nil, updates.Selection{}, fmt.Errorf("%w: %v", ErrUpdatesOff, err)
	}
	f, err := s.updateSource().LoadIndex(ctx, s.cores.keys)
	if err != nil {
		return nil, updates.Selection{}, err
	}
	if s.resolveChannelDefault(ctx, set, f.Index) {
		if set, err = s.UpdateSettings(ctx); err != nil {
			return nil, updates.Selection{}, err
		}
		q.Channel = set.Channel
	}
	sel, err := updates.Select(f.Index, q)
	return f, sel, err
}

// resolveChannelDefault stores the default channel of a beta build once: with no channel selected and none
// resolved yet, "stable" if the verified index lists the running version as a stable server release (it was promoted
// without a rebuild), else "beta". It never changes afterwards. It reports whether it stored a value.
func (s *Service) resolveChannelDefault(ctx context.Context, set UpdateSettings, idx updates.Index) bool {
	if set.ChannelIsSet || set.CompiledChannel != updates.ChannelBeta {
		return false
	}
	if _, stored, err := s.getSetting(ctx, settingUpdateChannelDefault); err != nil || stored {
		return false
	}
	def := UpdateChannelBeta
	for _, r := range idx.Releases {
		if r.Product == updates.ProductHub && r.Channel == updates.ChannelStable && r.Version == s.hubVer {
			def = UpdateChannelStable
			break
		}
	}
	return s.setSetting(ctx, settingUpdateChannelDefault, def) == nil
}

// UpdateCheckReport summarizes a check.
type UpdateCheckReport struct {
	Available *UpdateAvailable
	Staged    bool // the update was staged and requested automatically
	Skipped   int  // releases of the index that failed validation
}

// CheckUpdates fetches the signed updates index, selects the newest release and, with automatic install on, stages
// it and creates the request file (not during an active Session, not when it breaks recent apps, not when a
// previous attempt for the same version failed). The result is recorded for the Settings screen of the app.
func (s *Service) CheckUpdates(ctx context.Context) (_ UpdateCheckReport, err error) {
	defer s.publishOK(&err, TopicUpdates)
	s.upd.mu.Lock()
	defer s.upd.mu.Unlock()
	var rep UpdateCheckReport
	f, sel, err := s.selectUpdate(ctx)
	if errors.Is(err, ErrUpdatesOff) {
		_ = s.setSetting(ctx, settingUpdateAvailable, "")
		_ = s.setSetting(ctx, settingUpdateLastError, "")
		return rep, err
	}
	_ = s.setSetting(ctx, settingUpdateLastCheck, strconv.FormatInt(s.now().Unix(), 10))
	if err != nil {
		_ = s.setSetting(ctx, settingUpdateLastError, cleanText(err.Error(), 500))
		return rep, err
	}
	_ = s.setSetting(ctx, settingUpdateLastError, "")
	s.recordRunning(ctx)
	rep.Skipped = len(f.Skipped)
	s.dropObsoleteStage()
	if sel.Release == nil {
		_ = s.setSetting(ctx, settingUpdateAvailable, "")
		return rep, nil
	}
	a := &UpdateAvailable{Version: sel.Release.Version, Channel: sel.Release.Channel, NotesURL: sel.Release.NotesURL, Notes: sel.Release.Notes,
		PublishedAt: sel.Release.PublishedAt, Artifact: sel.Artifact.Name, URL: sel.Artifact.URL, Size: sel.Artifact.Size}
	if n := s.playersBelow(ctx, sel.Release.MinProtocolVersion); n > 0 {
		a.Breaking, a.BreakingCount = true, n
	}
	b, _ := json.Marshal(a)
	_ = s.setSetting(ctx, settingUpdateAvailable, string(b))
	rep.Available = a

	set, _ := s.UpdateSettings(ctx)
	if !set.Auto || a.Breaking || !s.packaged() || s.countActiveSessions(ctx) > 0 {
		return rep, nil
	}
	// After going back, automatic updates are paused, and the skipped version is never installed automatically.
	if s.UpdatePause(ctx) != nil || skippedVersion(a.Version, s.UpdateSkipped(ctx)) {
		return rep, nil
	}
	if r, _ := updates.ReadResult(s.dataDir); r != nil && !r.OK && r.Version == a.Version {
		return rep, nil // a previous attempt failed; an admin has to retry
	}
	if st, _ := updates.ReadStaged(s.dataDir); st != nil && st.Version == a.Version && updates.RequestPending(s.updCfg.requestDir) {
		return rep, nil
	}
	if _, err := s.stageAndRequest(ctx, f, sel); err != nil {
		_ = s.setSetting(ctx, settingUpdateLastError, cleanText("install update: "+err.Error(), 500))
		return rep, err
	}
	rep.Staged = true
	return rep, nil
}

// dropObsoleteStage removes a staged update that is not newer than the running version (already installed).
func (s *Service) dropObsoleteStage() {
	st, err := updates.ReadStaged(s.dataDir)
	if err != nil || st == nil {
		return
	}
	if c, err := updates.CompareVersions(st.Version, s.hubVer); err != nil || c <= 0 {
		_ = updates.ClearStaged(s.dataDir)
	}
}

func (s *Service) stageAndRequest(ctx context.Context, f *updates.Fetched, sel updates.Selection) (updates.Staged, error) {
	if _, err := updates.Stage(ctx, f, *sel.Release, *sel.Artifact, s.dataDir); err != nil {
		return updates.Staged{}, err
	}
	st, _ := updates.ReadStaged(s.dataDir)
	if st == nil {
		return updates.Staged{}, errors.New("staging failed")
	}
	if err := updates.WriteRequest(s.updCfg.requestDir, st.Version); err != nil {
		return *st, err
	}
	return *st, nil
}

// InstallUpdate stages the newest update and requests the root helper (the Settings screen of the app "Install update").
// It needs a packaged install. An update that breaks recent apps needs confirmBreaking.
func (s *Service) InstallUpdate(ctx context.Context, confirmBreaking bool) (_ updates.Staged, err error) {
	defer s.publishOK(&err, TopicUpdates)
	s.upd.mu.Lock()
	defer s.upd.mu.Unlock()
	if !s.packaged() {
		return updates.Staged{}, ErrNotPackaged
	}
	f, sel, err := s.selectUpdate(ctx)
	if err != nil {
		return updates.Staged{}, err
	}
	if sel.Release == nil {
		return updates.Staged{}, ErrNoUpdate
	}
	if n := s.playersBelow(ctx, sel.Release.MinProtocolVersion); n > 0 && !confirmBreaking {
		return updates.Staged{}, ErrUpdateBreaking
	}
	st, err := s.stageAndRequest(ctx, f, sel)
	if err == nil {
		// "Update to X" after going back: moving forward again ends the pause and the skip.
		if p := s.UpdatePause(ctx); p != nil {
			s.endPause(ctx, p, true)
		} else {
			_ = s.setSetting(ctx, updates.SettingSkip, "")
		}
	}
	return st, err
}

// TriggerUpdateCheck asks the background loop for a check now; it never blocks.
func (s *Service) TriggerUpdateCheck() {
	select {
	case s.upd.kick <- struct{}{}:
	default:
	}
}

// updateInterval is the check interval of the effective channel.
func (s *Service) updateInterval(ctx context.Context) time.Duration {
	if set, err := s.UpdateSettings(ctx); err == nil && set.Channel == UpdateChannelBeta {
		return updateIntervalTest
	}
	return updateIntervalStable
}

// RunUpdateChecks checks at once (in the caller's goroutine, so start it with go), then every hour on the test
// channel and every 24 hours otherwise, and on TriggerUpdateCheck, until ctx ends. report (may be nil) receives
// each result; a failure never stops the loop.
func (s *Service) RunUpdateChecks(ctx context.Context, report func(UpdateCheckReport, error)) {
	do := func() {
		rep, err := s.CheckUpdates(ctx)
		if report != nil && ctx.Err() == nil && !errors.Is(err, ErrUpdatesOff) {
			report(rep, err)
		}
	}
	do()
	for {
		t := time.NewTimer(s.updateInterval(ctx))
		select {
		case <-ctx.Done():
			t.Stop()
			return
		case <-t.C:
			do()
		case <-s.upd.kick:
			t.Stop()
			do()
		}
	}
}

// shellQuote single-quotes s for a POSIX shell (HC-13: the command is shown for copy and paste).
func shellQuote(s string) string {
	return "'" + strings.ReplaceAll(s, "'", `'\''`) + "'"
}

// manualCommand is what an admin does by hand when the server cannot install the update itself.
func manualCommand(platform string, a *UpdateAvailable) string {
	if updates.InContainer() {
		return "This server runs in a container. Update by pulling the new image and recreating the container (docker compose pull && docker compose up -d); see docs/guides/server-docker.md."
	}
	if updates.KindFor(platform) == updates.KindMSI {
		return "Download " + a.URL + " and run " + a.Artifact + " as Administrator."
	}
	return "curl -fLO " + shellQuote(a.URL) + " && sudo apt install ./" + a.Artifact // the name is validated (nameRe)
}
