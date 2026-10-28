package hub

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"regexp"
	"runtime"
	"strings"
	"sync"
	"time"
	"unicode/utf8"

	"github.com/phabioo/uzume/server/internal/errfp"
	"github.com/phabioo/uzume/server/internal/pluginpkg"
	"github.com/phabioo/uzume/server/internal/redact"
)

// Error events and error groups (ADR 0035 D1, D3, D4; handshake feature `reports_v2`). Apps send error events of known
// kinds, the server records its own. Events with one fingerprint form a group that admins list, link to a GitHub
// issue and delete. The fingerprint is computed by the sender (internal/errfp, shared test vectors); the server only
// checks its format.

// FeatureReportsV2 is the handshake feature flag for error events and error groups.
const FeatureReportsV2 = "reports_v2"

// Limits of error events.
const (
	ErrorBatchMax            = 20  // events per request
	ErrorEventsPerDevicePerH = 20  // events one device may send per hour; the rest is dropped
	ErrorServerEventsPerHour = 20  // events the server records about itself per hour
	ErrorEventsPerGroupMax   = 200 // events kept per group (oldest dropped)
	errorTitleMax            = 200
	errorMessageMax          = 2000
	errorVersionMax          = 40
	errorOSMax               = 80
	errorComponentMax        = 96
	errorVersionsShown       = 10
	errorDetailEvents        = 20
	errorServerKey           = "server" // device id and rate limit key of the server's own events
	errorServerName          = "Uzume server"
	errorRateWindow          = time.Hour
	// IssuesBaseUzume is the target of app and server errors; IssuesBaseAzahar the one of the patched Azahar core.
	IssuesBaseUzume  = "https://github.com/phabioo/uzume"
	IssuesBaseAzahar = "https://github.com/phabioo/azahar"
)

// Error event kinds.
const (
	ErrKindAppCrash      = "app_crash"
	ErrKindCoreFailure   = "core_failure"
	ErrKindPluginError   = "plugin_error"
	ErrKindSyncFailure   = "sync_failure"
	ErrKindUpdateFailure = "update_failure"
	ErrKindServerError   = "server_error"
)

var (
	errorKinds = map[string]bool{ErrKindAppCrash: true, ErrKindCoreFailure: true, ErrKindPluginError: true,
		ErrKindSyncFailure: true, ErrKindUpdateFailure: true, ErrKindServerError: true}
	errorComponentRe   = regexp.MustCompile(`^(app|server|core:[a-z0-9][a-z0-9_-]{0,63}|plugin:[A-Za-z0-9._-]{1,80})$`)
	errorFingerprintRe = regexp.MustCompile(`^uz-[0-9a-f]{12}$`)
	errorIssueURLRe    = regexp.MustCompile(`^https://github\.com/[A-Za-z0-9-]+/[A-Za-z0-9._-]+/issues/\d+$`)
)

// ValidFingerprint reports whether s has the format `uz-` and 12 lowercase hex digits.
func ValidFingerprint(s string) bool { return errorFingerprintRe.MatchString(s) }

// ErrorEventInput is one event as an app sends it.
type ErrorEventInput struct {
	Kind, Component, Fingerprint, Title, Message, AppVersion, OS string
	OccurredAt                                                   time.Time
}

// ErrorGroup is one group of events with the same fingerprint.
type ErrorGroup struct {
	Fingerprint, Kind, Component, Title string
	Count                               int64
	Devices                             int
	FirstSeen, LastSeen                 time.Time
	Versions                            []string // newest first, at most 10
	IssueURL                            *string
	Seen                                bool
	IssuesBase                          *string
}

// ErrorEventRow is one stored event as the admin sees it.
type ErrorEventRow struct {
	OccurredAt                                    time.Time
	AppVersion, OS, DeviceName, UserName, Message string
}

// ErrorGroupDetail is a group with its newest events and the text for a public issue.
type ErrorGroupDetail struct {
	ErrorGroup
	Events        []ErrorEventRow
	PublicTitle   string
	PublicSummary string
}

// ErrorGroupUpdate is a PATCH: IssueURLSet with a nil IssueURL forgets the link.
type ErrorGroupUpdate struct {
	IssueURLSet bool
	IssueURL    *string
	Seen        bool
}

type errorState struct {
	mu   sync.Mutex
	hits map[string][]time.Time // device id (or errorServerKey) -> times of accepted events within the window
}

// errorTake reserves up to n events of the key within the hourly limit and returns how many were granted.
func (s *Service) errorTake(key string, n, limit int) int {
	now := s.Now()
	s.errs.mu.Lock()
	defer s.errs.mu.Unlock()
	if s.errs.hits == nil {
		s.errs.hits = map[string][]time.Time{}
	}
	keep := s.errs.hits[key][:0]
	for _, t := range s.errs.hits[key] {
		if now.Sub(t) < errorRateWindow {
			keep = append(keep, t)
		}
	}
	granted := limit - len(keep)
	if granted < 0 {
		granted = 0
	}
	if granted > n {
		granted = n
	}
	for i := 0; i < granted; i++ {
		keep = append(keep, now)
	}
	s.errs.hits[key] = keep
	return granted
}

func validateErrorEvent(e ErrorEventInput) error {
	switch {
	case !errorKinds[e.Kind]:
		return badRequest("Unknown error kind")
	case len(e.Component) > errorComponentMax || !errorComponentRe.MatchString(e.Component):
		return badRequest("Invalid component")
	case !errorFingerprintRe.MatchString(e.Fingerprint):
		return badRequest("Invalid fingerprint")
	case strings.TrimSpace(e.Title) == "" || utf8.RuneCountInString(e.Title) > errorTitleMax || strings.ContainsAny(e.Title, "\r\n") || !utf8.ValidString(e.Title):
		return badRequest("The title must be one line of 1 to 200 characters")
	case utf8.RuneCountInString(e.Message) > errorMessageMax || !utf8.ValidString(e.Message):
		return badRequest("The message is longer than 2000 characters")
	case utf8.RuneCountInString(e.AppVersion) > errorVersionMax || !utf8.ValidString(e.AppVersion):
		return badRequest("The app version is longer than 40 characters")
	case utf8.RuneCountInString(e.OS) > errorOSMax || !utf8.ValidString(e.OS):
		return badRequest("The system is longer than 80 characters")
	case e.OccurredAt.IsZero():
		return badRequest("occurred_at is missing")
	}
	return nil
}

// RecordErrorEvents stores the events of an authenticated device. All events are validated first (one bad event
// fails the request, ErrBadRequest). A device may send ErrorEventsPerDevicePerH events per hour; the surplus is dropped
// and counted, never an error.
func (s *Service) RecordErrorEvents(ctx context.Context, p Principal, evs []ErrorEventInput) (accepted, dropped int, err error) {
	if len(evs) < 1 || len(evs) > ErrorBatchMax {
		return 0, 0, badRequest("Send 1 to %d events", ErrorBatchMax)
	}
	for _, e := range evs {
		if err := validateErrorEvent(e); err != nil {
			return 0, 0, err
		}
	}
	n := s.errorTake(p.Device.ID, len(evs), ErrorEventsPerDevicePerH)
	if n > 0 {
		if err := s.storeErrorEvents(ctx, evs[:n], p.Device.ID, p.Device.Name, p.User.DisplayName); err != nil {
			return 0, 0, err
		}
	}
	return n, len(evs) - n, nil
}

// RecordServerError records an event of the server itself (component server). kind is ErrKindServerError or
// ErrKindUpdateFailure; at most ErrorServerEventsPerHour per hour, the rest is dropped. Best effort: it never fails
// the caller.
func (s *Service) RecordServerError(ctx context.Context, kind, title, message string) {
	if kind != ErrKindServerError && kind != ErrKindUpdateFailure {
		return
	}
	title = cleanLine(title, errorTitleMax)
	if title == "" {
		title = "Server error"
	}
	if r := []rune(message); len(r) > errorMessageMax {
		message = string(r[:errorMessageMax])
	}
	e := ErrorEventInput{Kind: kind, Component: "server", Fingerprint: errfp.Fingerprint(kind, "server", message), Title: title,
		Message: strings.ToValidUTF8(message, "�"), AppVersion: cleanLine(s.hubVer, errorVersionMax),
		OS: runtime.GOOS + "/" + runtime.GOARCH, OccurredAt: s.Now()}
	if validateErrorEvent(e) != nil || s.errorTake(errorServerKey, 1, ErrorServerEventsPerHour) == 0 {
		return
	}
	_ = s.storeErrorEvents(context.WithoutCancel(ctx), []ErrorEventInput{e}, errorServerKey, errorServerName, "")
}

func (s *Service) storeErrorEvents(ctx context.Context, evs []ErrorEventInput, deviceID, deviceName, userName string) error {
	now := s.Now()
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return internal(err)
	}
	defer tx.Rollback()
	for _, e := range evs {
		occ := e.OccurredAt.UTC()
		if occ.After(now) { // a device clock in the future must not pin last_seen
			occ = now
		}
		title := redact.RedactString(e.Title)
		msg := redact.RedactString(e.Message)
		if _, err := tx.ExecContext(ctx, `INSERT INTO error_groups(fingerprint, kind, component, title, count, first_seen, last_seen, rev, seen_rev)
			VALUES (?,?,?,?,1,?,?,1,0)
			ON CONFLICT(fingerprint) DO UPDATE SET count = count + 1, rev = rev + 1,
				first_seen = MIN(first_seen, excluded.first_seen), last_seen = MAX(last_seen, excluded.last_seen)`,
			e.Fingerprint, e.Kind, e.Component, title, occ.Unix(), occ.Unix()); err != nil {
			return internal(err)
		}
		if _, err := tx.ExecContext(ctx, `INSERT INTO error_events(fingerprint, device_id, device_name, user_name, app_version, os, message, occurred_at, received_at)
			VALUES (?,?,?,?,?,?,?,?,?)`, e.Fingerprint, deviceID, deviceName, userName, e.AppVersion, e.OS, msg, occ.Unix(), now.Unix()); err != nil {
			return internal(err)
		}
		if _, err := tx.ExecContext(ctx, `INSERT OR IGNORE INTO error_group_devices(fingerprint, device_id) VALUES (?,?)`, e.Fingerprint, deviceID); err != nil {
			return internal(err)
		}
		if _, err := tx.ExecContext(ctx, `DELETE FROM error_events WHERE fingerprint = ? AND id NOT IN
			(SELECT id FROM error_events WHERE fingerprint = ? ORDER BY id DESC LIMIT ?)`, e.Fingerprint, e.Fingerprint, ErrorEventsPerGroupMax); err != nil {
			return internal(err)
		}
	}
	if err := tx.Commit(); err != nil {
		return internal(err)
	}
	s.Publish(TopicReports)
	return nil
}

const errorGroupCols = `g.fingerprint, g.kind, g.component, g.title, g.count, g.first_seen, g.last_seen, g.issue_url, g.rev, g.seen_rev,
	(SELECT COUNT(*) FROM error_group_devices d WHERE d.fingerprint = g.fingerprint)`

func scanErrorGroup(r scanner) (ErrorGroup, error) {
	var g ErrorGroup
	var first, last, rev, seenRev int64
	var issue sql.NullString
	if err := r.Scan(&g.Fingerprint, &g.Kind, &g.Component, &g.Title, &g.Count, &first, &last, &issue, &rev, &seenRev, &g.Devices); err != nil {
		return g, err
	}
	g.FirstSeen, g.LastSeen = time.Unix(first, 0).UTC(), time.Unix(last, 0).UTC()
	if issue.Valid && issue.String != "" {
		g.IssueURL = &issue.String
	}
	g.Seen = rev == seenRev
	g.Versions = []string{}
	return g, nil
}

func (s *Service) fillErrorGroup(ctx context.Context, g *ErrorGroup, bases map[string]*string) error {
	rows, err := s.db.QueryContext(ctx, `SELECT app_version FROM error_events WHERE fingerprint = ? ORDER BY id DESC`, g.Fingerprint)
	if err != nil {
		return internal(err)
	}
	defer rows.Close()
	seen := map[string]bool{}
	for rows.Next() && len(g.Versions) < errorVersionsShown {
		var v string
		if err := rows.Scan(&v); err != nil {
			return internal(err)
		}
		if v != "" && !seen[v] {
			seen[v] = true
			g.Versions = append(g.Versions, v)
		}
	}
	if err := rows.Err(); err != nil {
		return internal(err)
	}
	b, ok := bases[g.Component]
	if !ok {
		if b, err = s.errorIssuesBase(ctx, g.Component); err != nil {
			return err
		}
		bases[g.Component] = b
	}
	g.IssuesBase = b
	return nil
}

// errorIssuesBase is the repository for a new issue of the component (ADR 0035 D4), nil when there is none.
func (s *Service) errorIssuesBase(ctx context.Context, component string) (*string, error) {
	var out string
	switch {
	case component == "app" || component == "server":
		out = IssuesBaseUzume
	case component == "core:azahar":
		out = IssuesBaseAzahar
	case strings.HasPrefix(component, "plugin:"):
		rows, err := s.db.QueryContext(ctx, `SELECT version, manifest FROM plugins WHERE plugin_id = ?`, strings.TrimPrefix(component, "plugin:"))
		if err != nil {
			return nil, internal(err)
		}
		defer rows.Close()
		best, bestRaw := "", ""
		for rows.Next() {
			var v, raw string
			if err := rows.Scan(&v, &raw); err != nil {
				return nil, internal(err)
			}
			if bestRaw == "" || semverLess(best, v) {
				best, bestRaw = v, raw
			}
		}
		if err := rows.Err(); err != nil {
			return nil, internal(err)
		}
		if bestRaw == "" {
			return nil, nil
		}
		m, _ := pluginpkg.ParseManifest([]byte(bestRaw))
		out = m.IssuesURL
	}
	if out == "" {
		return nil, nil
	}
	return &out, nil
}

// ListErrorGroups returns the groups, last seen first, and how many are not yet seen.
func (s *Service) ListErrorGroups(ctx context.Context) ([]ErrorGroup, int, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT `+errorGroupCols+` FROM error_groups g ORDER BY g.last_seen DESC, g.fingerprint`)
	if err != nil {
		return nil, 0, internal(err)
	}
	var out []ErrorGroup
	for rows.Next() {
		g, err := scanErrorGroup(rows)
		if err != nil {
			rows.Close()
			return nil, 0, internal(err)
		}
		out = append(out, g)
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return nil, 0, internal(err)
	}
	rows.Close()
	bases := map[string]*string{}
	unseen := 0
	for i := range out {
		if err := s.fillErrorGroup(ctx, &out[i], bases); err != nil {
			return nil, 0, err
		}
		if !out[i].Seen {
			unseen++
		}
	}
	return out, unseen, nil
}

// NewErrorGroupCount is the number of groups with an event since the last "seen" mark.
func (s *Service) NewErrorGroupCount(ctx context.Context) (int, error) {
	var n int
	if err := s.db.QueryRowContext(ctx, `SELECT COUNT(*) FROM error_groups WHERE rev <> seen_rev`).Scan(&n); err != nil {
		return 0, internal(err)
	}
	return n, nil
}

func (s *Service) getErrorGroup(ctx context.Context, fp string) (ErrorGroup, error) {
	g, err := scanErrorGroup(s.db.QueryRowContext(ctx, `SELECT `+errorGroupCols+` FROM error_groups g WHERE g.fingerprint = ?`, fp))
	if errors.Is(err, sql.ErrNoRows) {
		return g, ErrNotFound
	}
	if err != nil {
		return g, internal(err)
	}
	return g, s.fillErrorGroup(ctx, &g, map[string]*string{})
}

// GetErrorGroup returns one group with its newest events and the public text. Reading does not mark it seen.
func (s *Service) GetErrorGroup(ctx context.Context, fp string) (ErrorGroupDetail, error) {
	g, err := s.getErrorGroup(ctx, fp)
	if err != nil {
		return ErrorGroupDetail{}, err
	}
	d := ErrorGroupDetail{ErrorGroup: g, Events: []ErrorEventRow{}}
	rows, err := s.db.QueryContext(ctx, `SELECT occurred_at, app_version, os, device_name, user_name, message FROM error_events
		WHERE fingerprint = ? ORDER BY id DESC LIMIT ?`, fp, errorDetailEvents)
	if err != nil {
		return d, internal(err)
	}
	defer rows.Close()
	for rows.Next() {
		var e ErrorEventRow
		var occ int64
		if err := rows.Scan(&occ, &e.AppVersion, &e.OS, &e.DeviceName, &e.UserName, &e.Message); err != nil {
			return d, internal(err)
		}
		e.OccurredAt = time.Unix(occ, 0).UTC()
		d.Events = append(d.Events, e)
	}
	if err := rows.Err(); err != nil {
		return d, internal(err)
	}
	known, err := s.publicKnown(ctx, d.Events)
	if err != nil {
		return d, err
	}
	d.PublicTitle = redact.Public(g.Title, known)
	var b strings.Builder
	if len(d.Events) > 0 {
		if msg := strings.TrimSpace(redact.Public(d.Events[0].Message, known)); msg != "" {
			b.WriteString(msg + "\n\n")
		}
	}
	times := "times"
	if g.Count == 1 {
		times = "time"
	}
	devices := "devices"
	if g.Devices == 1 {
		devices = "device"
	}
	fmt.Fprintf(&b, "Seen %d %s on %d %s", g.Count, times, g.Devices, devices)
	if len(g.Versions) > 0 {
		vs := make([]string, len(g.Versions))
		for i, v := range g.Versions {
			vs[i] = redact.Public(v, known)
		}
		b.WriteString(", versions " + strings.Join(vs, ", "))
	}
	b.WriteString(".")
	d.PublicSummary = b.String()
	return d, nil
}

// publicKnown collects what a public text must not contain: every user, display, device and server name, the names
// stored with the events, and the titles and ROM file names of the library.
func (s *Service) publicKnown(ctx context.Context, evs []ErrorEventRow) (redact.Known, error) {
	var k redact.Known
	collect := func(q string, dst *[]string) error {
		rows, err := s.db.QueryContext(ctx, q)
		if err != nil {
			return internal(err)
		}
		defer rows.Close()
		cols, _ := rows.Columns()
		for rows.Next() {
			vals := make([]string, len(cols))
			ptrs := make([]any, len(cols))
			for i := range vals {
				ptrs[i] = &vals[i]
			}
			if err := rows.Scan(ptrs...); err != nil {
				return internal(err)
			}
			*dst = append(*dst, vals...)
		}
		return rows.Err()
	}
	for _, q := range []struct {
		sql string
		dst *[]string
	}{
		{`SELECT username, display_name FROM users`, &k.Names},
		{`SELECT name FROM devices`, &k.Names},
		{`SELECT device_name, user_name FROM error_events`, &k.Names},
		{`SELECT title, filename FROM games`, &k.Games},
		{`SELECT filename FROM game_roms`, &k.Games},
	} {
		if err := collect(q.sql, q.dst); err != nil {
			return k, err
		}
	}
	k.Names = append(k.Names, s.Info().Name)
	for _, e := range evs {
		k.Names = append(k.Names, e.DeviceName, e.UserName)
	}
	return k, nil
}

// UpdateErrorGroup remembers or forgets the GitHub issue of a group and/or marks it seen. Errors: ErrNotFound,
// ErrBadRequest (issue_url not https://github.com/<owner>/<repo>/issues/<number>, or nothing to change).
func (s *Service) UpdateErrorGroup(ctx context.Context, fp string, u ErrorGroupUpdate) (ErrorGroup, error) {
	if !u.IssueURLSet && !u.Seen {
		return ErrorGroup{}, badRequest("Nothing to change")
	}
	if u.IssueURLSet && u.IssueURL != nil && (len(*u.IssueURL) > 200 || !errorIssueURLRe.MatchString(*u.IssueURL)) {
		return ErrorGroup{}, badRequest("issue_url must look like https://github.com/<owner>/<repo>/issues/<number>")
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return ErrorGroup{}, internal(err)
	}
	defer tx.Rollback()
	var exists int
	if err := tx.QueryRowContext(ctx, `SELECT 1 FROM error_groups WHERE fingerprint = ?`, fp).Scan(&exists); errors.Is(err, sql.ErrNoRows) {
		return ErrorGroup{}, ErrNotFound
	} else if err != nil {
		return ErrorGroup{}, internal(err)
	}
	if u.IssueURLSet {
		var v any
		if u.IssueURL != nil {
			v = *u.IssueURL
		}
		if _, err := tx.ExecContext(ctx, `UPDATE error_groups SET issue_url = ? WHERE fingerprint = ?`, v, fp); err != nil {
			return ErrorGroup{}, internal(err)
		}
	}
	if u.Seen {
		if _, err := tx.ExecContext(ctx, `UPDATE error_groups SET seen_rev = rev WHERE fingerprint = ?`, fp); err != nil {
			return ErrorGroup{}, internal(err)
		}
	}
	if err := tx.Commit(); err != nil {
		return ErrorGroup{}, internal(err)
	}
	s.Publish(TopicReports)
	return s.getErrorGroup(ctx, fp)
}

// DeleteErrorGroup removes a group with its events.
func (s *Service) DeleteErrorGroup(ctx context.Context, fp string) error {
	res, err := s.db.ExecContext(ctx, `DELETE FROM error_groups WHERE fingerprint = ?`, fp)
	if err != nil {
		return internal(err)
	}
	if n, _ := res.RowsAffected(); n == 0 {
		return ErrNotFound
	}
	s.Publish(TopicReports)
	return nil
}

// recordUpdateFailure records the failed self-update of the last result once (dedupe by its time), so the helper
// process that applies updates needs no database access.
func (s *Service) recordUpdateFailure(ctx context.Context, version string, at time.Time, message string) {
	key := fmt.Sprintf("%d", at.Unix())
	if v, ok, err := s.getSetting(ctx, settingErrorUpdateRecorded); err != nil || (ok && v == key) {
		return
	}
	if s.setSetting(ctx, settingErrorUpdateRecorded, key) != nil {
		return
	}
	s.RecordServerError(ctx, ErrKindUpdateFailure, "Update to "+version+" failed", "Update to "+version+" failed: "+message)
}

const settingErrorUpdateRecorded = "error_update_recorded"
