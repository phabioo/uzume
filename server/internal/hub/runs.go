package hub

import (
	"bytes"
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"regexp"
	"strings"
	"sync"
	"time"
	"unicode/utf8"

	"github.com/google/uuid"
)

// Runs (0.11, ADR 0023 D9/D10, docs/architecture/12-plugins.md 16.11): a run is a shared object that plugins build
// on: participants with a game and a JSON state per plugin each, an append-only event log and a board tree per plugin.
// The server validates only shape and size and never runs plugin code. Run data never goes to another server.

// FeatureRunsV1 is the handshake feature flag for the runs REST API and the run_* WSS messages.
const FeatureRunsV1 = "runs_v1"

// FeatureRunsV2 is the additive handshake feature flag for runs without plugins and the run field save_slot.
const FeatureRunsV2 = "runs_v2"

// RunSaveSlot is the app's save slot of a run: "run-" plus the first 8 hex characters of the run id (lowercase, the
// dashes of the uuid ignored). Same rule as runSaveSlot in client/network/runtypes.cpp.
func RunSaveSlot(runID string) string {
	var b strings.Builder
	b.WriteString("run-")
	n := 0
	for _, c := range strings.ToLower(runID) {
		if c == '-' {
			continue
		}
		if n == 8 {
			break
		}
		b.WriteRune(c)
		n++
	}
	return b.String()
}

// FeatureRunRulesV1 is the handshake feature flag for the core run rules (`rules` on the run, PUT /runs/{run_id}/rules,
// the run event rules.changed) and for the optional cheats_used marker on save uploads and history versions.
const FeatureRunRulesV1 = "run_rules_v1"

// FeatureRunPluginUpdatesV1 is the handshake feature flag for switching a run's pinned plugin versions
// (PUT /runs/{run_id}/plugins, `previous_version` on the run plugin, run event plugins.updated).
const FeatureRunPluginUpdatesV1 = "run_plugin_updates_v1"

// Values of the run rules cheats and memory_write.
const (
	RuleOff    = "off"
	RuleLogged = "logged"
)

// RunRules are the core rules of a run (not plugin settings); the app enforces them. Without a run everything is allowed.
type RunRules struct {
	States      bool   `json:"states"`
	Rewind      bool   `json:"rewind"`
	Cheats      string `json:"cheats"`
	MemoryWrite string `json:"memory_write"`
}

// DefaultRunRules are the rules of a new run and of every run that existed before run_rules_v1.
func DefaultRunRules() RunRules { return RunRules{Cheats: RuleOff, MemoryWrite: RuleOff} }

// Validate checks the two enum fields.
func (r RunRules) Validate() error {
	for _, v := range []string{r.Cheats, r.MemoryWrite} {
		if v != RuleOff && v != RuleLogged {
			return badRequest("cheats and memory_write must be \"off\" or \"logged\"")
		}
	}
	return nil
}

// Run event types that the app itself writes for the core features (no plugin needed, see AppendRunEvent).
var coreRunEventTypes = map[string]bool{"cheat.enabled": true, "cheat.disabled": true, "memory.write": true, "state.loaded": true}

// TopicRuns is published when a run changes (the app refreshes its Runs page).
const TopicRuns = "runs"

// Run and participant states.
const (
	RunStarting  = "starting"
	RunActive    = "active"
	RunPaused    = "paused"
	RunFinished  = "finished"
	RunAbandoned = "abandoned"

	ParticipantInvited  = "invited"
	ParticipantJoined   = "joined"
	ParticipantDeclined = "declined"
	ParticipantLeft     = "left"

	EventAutomatic = "automatic"
	EventManual    = "manual"
	EventHub       = "hub"
)

// Limits (contract 0.11 package 3, section 5).
const (
	MaxRunParticipants  = 8
	MaxRunEvents        = 50000
	MaxRunNameLen       = 64
	MaxRunPlugins       = 8
	MaxRunSettingsBytes = 16 << 10
	MaxRunStateBytes    = 64 << 10
	MaxRunPayloadBytes  = 4 << 10
	MaxRunBoardBytes    = 64 << 10
	MaxRunEventText     = 280
	// RunCatchUpMax is how many missing events per run the server pushes after hello; with more missing it pushes none
	// and the app pages with GET /runs/{run_id}/events.
	RunCatchUpMax = 500
	// MaxRunEventsPage is the largest page of GET /runs/{run_id}/events.
	MaxRunEventsPage     = 500
	DefaultRunEventsPage = 200
)

// Token buckets per participant (contract): events 2/s burst 30, state writes 4/s burst 20, board writes 1/s burst 5.
const (
	runEventRate, runEventBurst = 2.0, 30.0
	runStateRate, runStateBurst = 4.0, 20.0
	runBoardRate, runBoardBurst = 1.0, 5.0
)

// Error codes of the runs API.
const (
	CodeRunNotActive  Code = "run_not_active"
	CodeStateConflict Code = "state_conflict"
	CodeTooLarge      Code = "too_large"
)

func tooLarge(format string, a ...any) *Error {
	e := badRequest(format, a...)
	e.Code = CodeTooLarge
	return e
}

var (
	// ErrRunNotActive: paused, finished and abandoned runs refuse events, state and board writes (409).
	ErrRunNotActive = &Error{Code: CodeRunNotActive, Message: "The run is not active (paused, finished or abandoned)"}
	errRunRate      = &Error{Code: CodeRateLimited, Message: "Too many requests for this run; slow down"}
)

var (
	runPluginIDRe  = regexp.MustCompile(`^[a-z0-9-]+(\.[a-z0-9-]+)+$`)
	runEventTypeRe = regexp.MustCompile(`^[a-z0-9_.-]{1,64}$`)
)

// PluginCatalog tells the runs service which plugin versions the server offers to apps. The plugins part
// implements it (Service.SetPluginCatalog); without a catalog no plugin is offered and no run can be created.
type PluginCatalog interface {
	// OfferedPlugin reports whether version of plugin id is offered and returns the SHA-256 of its file.
	OfferedPlugin(ctx context.Context, id, version string) (sha256 string, ok bool, err error)
}

// RunPlugin is a plugin version a run uses.
type RunPlugin struct {
	PluginID string `json:"plugin_id"`
	Version  string `json:"version"`
	SHA256   string `json:"sha256"`
	// PreviousVersion is the version pinned before the last switch (run_plugin_updates_v1), "" if never switched.
	PreviousVersion string `json:"previous_version,omitempty"`
	// PreviousSHA256 is the file hash of PreviousVersion (internal).
	PreviousSHA256 string `json:"-"`
}

// RunPluginRef is a plugin version requested at creation.
type RunPluginRef struct{ PluginID, Version string }

// RunPluginState is the state one participant published for one plugin of the run.
type RunPluginState struct {
	State     json.RawMessage `json:"state"`
	StateRev  int64           `json:"state_rev"`
	UpdatedAt time.Time       `json:"updated_at"`
}

// RunParticipant is one participant of a run with the states they published (per plugin id; only plugins with a row).
type RunParticipant struct {
	UserID   string                    `json:"user_id"`
	UserName string                    `json:"user_name"`
	Status   string                    `json:"status"`
	GameID   *string                   `json:"game_id"`
	Color    *int                      `json:"color"`
	States   map[string]RunPluginState `json:"states"`
	// PlaySeconds and LastPlayedAt are set by the REST reads only (nil in pushes): the participant's play time in
	// this run (plays whose presence named the run) and the end of the latest one (nil = never played).
	PlaySeconds  *int64     `json:"play_seconds,omitempty"`
	LastPlayedAt *time.Time `json:"last_played_at,omitempty"`
	// VariantKind ("run" or "own") and VariantState ("not_built", "building", "ready") describe the variant the person
	// plays in a randomizer run (nil in other runs and before the first join). variantID (an own variant) is private.
	VariantKind  *string `json:"variant_kind"`
	VariantState *string `json:"variant_state"`
	variantID    string
}

// Run is the run object as REST and WSS carry it (archived is web only and not part of the wire format).
type Run struct {
	ID           string           `json:"run_id"`
	Name         string           `json:"name"`
	Status       string           `json:"status"`
	CreatedBy    string           `json:"created_by"`
	CreatedAt    time.Time        `json:"created_at"`
	UpdatedAt    time.Time        `json:"updated_at"`
	StatusBy     string           `json:"status_by"`
	Settings     json.RawMessage  `json:"settings"`
	SaveSlot     string           `json:"save_slot"`
	Rules        RunRules         `json:"rules"`
	Plugins      []RunPlugin      `json:"plugins"`
	Participants []RunParticipant `json:"participants"`
	LastSeq      int64            `json:"last_seq"`
	// Randomizer is the public part of the run's randomizer recipe (never the settings or the seed).
	Randomizer RunRandomizer `json:"randomizer"`
	// PlaySeconds is the total play time of all people in this run; set by the REST reads only (nil in pushes).
	PlaySeconds *int64 `json:"play_seconds,omitempty"`
	Archived    bool   `json:"-"`
}

// Participant returns the participant of a user.
func (r Run) Participant(userID string) (RunParticipant, bool) {
	for _, p := range r.Participants {
		if p.UserID == userID {
			return p, true
		}
	}
	return RunParticipant{}, false
}

// Finished reports a run that is over.
func (r Run) Finished() bool { return r.Status == RunFinished || r.Status == RunAbandoned }

// RunEvent is one entry of the append-only log. UserID, DeviceID and PluginID are nil for server events.
type RunEvent struct {
	Seq       int64           `json:"seq"`
	CreatedAt time.Time       `json:"created_at"`
	UserID    *string         `json:"user_id"`
	UserName  string          `json:"user_name"`
	DeviceID  *string         `json:"device_id"`
	PluginID  *string         `json:"plugin_id"`
	Source    string          `json:"source"`
	Type      string          `json:"type"`
	Text      string          `json:"text"`
	Payload   json.RawMessage `json:"payload"`
	ClientID  string          `json:"client_id"`
}

// RunBoard is the board tree of one plugin of a run (shown in the app).
type RunBoard struct {
	PluginID  string          `json:"plugin_id"`
	Rev       int64           `json:"rev"`
	Tree      json.RawMessage `json:"tree"`
	UpdatedBy string          `json:"updated_by"`
	UpdatedAt time.Time       `json:"updated_at"`
}

// RunStateConflict is the current state returned with a state_conflict.
type RunStateConflict struct {
	StateRev int64
	State    json.RawMessage
}

// ---- service state ----

type tokenBucket struct {
	tokens float64
	at     time.Time
}

type runState struct {
	// mu serializes all run writes together with their fan-out, so messages leave in seq order. Lock order:
	// runs.mu, then sess.mu / rt.mu / db (never the other way round).
	mu      sync.Mutex
	buckets map[string]*tokenBucket
	catMu   sync.RWMutex
	catalog PluginCatalog
}

// SetPluginCatalog installs the plugin catalog consulted at run creation.
func (s *Service) SetPluginCatalog(c PluginCatalog) {
	s.runs.catMu.Lock()
	s.runs.catalog = c
	s.runs.catMu.Unlock()
}

func (s *Service) pluginCatalog() PluginCatalog {
	s.runs.catMu.RLock()
	defer s.runs.catMu.RUnlock()
	return s.runs.catalog
}

// runAllow takes one token of the bucket (kind, run, user). Callers hold runs.mu.
func (s *Service) runAllow(kind, runID, userID string, rate, burst float64) bool {
	if s.runs.buckets == nil {
		s.runs.buckets = map[string]*tokenBucket{}
	}
	key := kind + "|" + runID + "|" + userID
	now := s.now()
	b := s.runs.buckets[key]
	if b == nil {
		b = &tokenBucket{tokens: burst, at: now}
		s.runs.buckets[key] = b
	} else if d := now.Sub(b.at).Seconds(); d > 0 {
		b.tokens = min(burst, b.tokens+d*rate)
		b.at = now
	}
	if b.tokens < 1 {
		return false
	}
	b.tokens--
	return true
}

func (s *Service) dropRunBuckets(runID string) {
	for k := range s.runs.buckets {
		if strings.Contains(k, "|"+runID+"|") {
			delete(s.runs.buckets, k)
		}
	}
}

// OfferedPlugin implements PluginCatalog on top of the plugin store.
func (s *Service) OfferedPlugin(ctx context.Context, id, version string) (string, bool, error) {
	if !safePluginRef(id, version) {
		return "", false, nil
	}
	p, err := s.GetPlugin(ctx, id, version)
	if err != nil {
		var e *Error
		if errors.As(err, &e) && e.Code == CodeNotFound {
			return "", false, nil
		}
		return "", false, err
	}
	return p.SHA256, true, nil
}

// RunsUsingPluginVersion reports whether a run that is not finished or abandoned uses the plugin version, as pinned
// version or as previous_version (restorable)
// (the plugin store refuses to remove such a version).
func (s *Service) RunsUsingPluginVersion(ctx context.Context, id, version string) (bool, error) {
	var n int
	err := s.db.QueryRowContext(ctx, `SELECT EXISTS (SELECT 1 FROM run_plugins p JOIN runs r ON r.id = p.run_id
		WHERE p.plugin_id = ? AND (p.version = ? OR p.previous_version = ?) AND r.status NOT IN ('finished','abandoned'))`, id, version, version).Scan(&n)
	if err != nil {
		return false, internal(err)
	}
	return n == 1, nil
}

// ---- helpers ----

// objectJSON validates raw as one JSON object of at most max bytes (compact form) and returns it compacted.
// An empty value is the empty object. Opaque contents are kept byte-exact (numbers are not re-encoded).
func objectJSON(raw json.RawMessage, max int, what string) (json.RawMessage, error) {
	if len(bytes.TrimSpace(raw)) == 0 {
		return json.RawMessage("{}"), nil
	}
	var buf bytes.Buffer
	if err := json.Compact(&buf, raw); err != nil || buf.Bytes()[0] != '{' {
		return nil, badRequest("%s must be a JSON object", what)
	}
	if buf.Len() > max {
		return nil, tooLarge("%s is larger than %d KiB", what, max>>10)
	}
	return json.RawMessage(buf.Bytes()), nil
}

func unixTime(n int64) time.Time { return time.Unix(n, 0).UTC() }

func participantStatus(ctx context.Context, q dbq, runID, userID string) (string, error) {
	var st string
	err := q.QueryRowContext(ctx, `SELECT status FROM run_participants WHERE run_id = ? AND user_id = ?`, runID, userID).Scan(&st)
	if errors.Is(err, sql.ErrNoRows) {
		return "", ErrNotFound
	}
	if err != nil {
		return "", internal(err)
	}
	return st, nil
}

// joinedParticipant returns ErrNotFound for non-participants and ErrForbidden for participants who have not joined.
func joinedParticipant(ctx context.Context, q dbq, runID, userID string) error {
	st, err := participantStatus(ctx, q, runID, userID)
	if err != nil {
		return err
	}
	if st != ParticipantJoined {
		return &Error{Code: CodeForbidden, Message: "Join the run first"}
	}
	return nil
}

func userDisplayName(ctx context.Context, q dbq, userID string) string {
	var n string
	_ = q.QueryRowContext(ctx, `SELECT display_name FROM users WHERE id = ?`, userID).Scan(&n)
	return n
}

// runUsers lists the users of a run, optionally only the joined ones.
func runUsers(ctx context.Context, q dbq, runID string, joinedOnly bool) ([]string, error) {
	query := `SELECT user_id FROM run_participants WHERE run_id = ?`
	if joinedOnly {
		query += ` AND status = 'joined'`
	}
	rows, err := q.QueryContext(ctx, query, runID)
	if err != nil {
		return nil, internal(err)
	}
	defer rows.Close()
	var out []string
	for rows.Next() {
		var u string
		if err := rows.Scan(&u); err != nil {
			return nil, internal(err)
		}
		out = append(out, u)
	}
	return out, rows.Err()
}

// loadRun reads a run with plugins and participants. withState false leaves out the participants' states
// (states is empty): only for the admin list.
func loadRun(ctx context.Context, q dbq, id string, withState bool) (Run, error) {
	runs, err := loadRuns(ctx, q, []string{id}, withState)
	if err != nil {
		return Run{}, err
	}
	if len(runs) == 0 {
		return Run{}, ErrNotFound
	}
	return runs[0], nil
}

// loadRunsChunk bounds the ids of one IN (...) list (SQLite variable limit).
const loadRunsChunk = 400

// loadRuns reads several runs with plugins, participants and (withState) states in a constant number of queries per
// chunk of ids, not per run. The result keeps the order of ids; ids without a run are skipped.
func loadRuns(ctx context.Context, q dbq, ids []string, withState bool) ([]Run, error) {
	byID := make(map[string]*Run, len(ids))
	for start := 0; start < len(ids); start += loadRunsChunk {
		chunk := ids[start:min(start+loadRunsChunk, len(ids))]
		if err := loadRunsChunked(ctx, q, chunk, withState, byID); err != nil {
			return nil, err
		}
	}
	out := make([]Run, 0, len(ids))
	for _, id := range ids {
		if r, ok := byID[id]; ok {
			out = append(out, *r)
			delete(byID, id) // a duplicate id yields one run
		}
	}
	return out, nil
}

func inList(ids []string) (string, []any) {
	args := make([]any, len(ids))
	for i, id := range ids {
		args[i] = id
	}
	return "(" + strings.TrimSuffix(strings.Repeat("?,", len(ids)), ",") + ")", args
}

func loadRunsChunked(ctx context.Context, q dbq, ids []string, withState bool, byID map[string]*Run) error {
	in, args := inList(ids)
	rows, err := q.QueryContext(ctx, `SELECT id, name, status, archived, created_by, created_at, updated_at, status_by, settings, last_seq, rules
		FROM runs WHERE id IN `+in, args...)
	if err != nil {
		return internal(err)
	}
	for rows.Next() {
		r := &Run{}
		var created, updated int64
		var archived int
		var settings, rules string
		if err := rows.Scan(&r.ID, &r.Name, &r.Status, &archived, &r.CreatedBy, &created, &updated, &r.StatusBy, &settings, &r.LastSeq, &rules); err != nil {
			rows.Close()
			return internal(err)
		}
		r.Rules = DefaultRunRules()
		_ = json.Unmarshal([]byte(rules), &r.Rules) // a damaged value keeps the defaults
		r.Archived, r.CreatedAt, r.UpdatedAt, r.Settings = archived != 0, unixTime(created), unixTime(updated), json.RawMessage(settings)
		r.SaveSlot = RunSaveSlot(r.ID)
		r.Plugins, r.Participants = []RunPlugin{}, []RunParticipant{}
		r.Randomizer = RunRandomizer{Mode: RandomizerOff}
		byID[r.ID] = r
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return internal(err)
	}
	if len(byID) == 0 {
		return nil
	}

	rows, err = q.QueryContext(ctx, `SELECT run_id, plugin_id, version, sha256, COALESCE(previous_version, ''), COALESCE(previous_sha256, '') FROM run_plugins WHERE run_id IN `+in+` ORDER BY run_id, position`, args...)
	if err != nil {
		return internal(err)
	}
	for rows.Next() {
		var rid string
		var p RunPlugin
		if err := rows.Scan(&rid, &p.PluginID, &p.Version, &p.SHA256, &p.PreviousVersion, &p.PreviousSHA256); err != nil {
			rows.Close()
			return internal(err)
		}
		if r := byID[rid]; r != nil {
			r.Plugins = append(r.Plugins, p)
		}
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return internal(err)
	}

	rows, err = q.QueryContext(ctx, `SELECT p.run_id, p.user_id, u.display_name, p.status, p.game_id, p.color, p.variant_kind, p.variant_id, p.variant_state
		FROM run_participants p JOIN users u ON u.id = p.user_id WHERE p.run_id IN `+in+` ORDER BY p.run_id, p.invited_at, p.rowid`, args...)
	if err != nil {
		return internal(err)
	}
	for rows.Next() {
		var rid string
		var p RunParticipant
		var game sql.NullString
		var color sql.NullInt64
		var vkind, vid, vstate sql.NullString
		if err := rows.Scan(&rid, &p.UserID, &p.UserName, &p.Status, &game, &color, &vkind, &vid, &vstate); err != nil {
			rows.Close()
			return internal(err)
		}
		if game.Valid {
			p.GameID = &game.String
		}
		if color.Valid {
			c := int(color.Int64)
			p.Color = &c
		}
		if vkind.Valid {
			p.VariantKind = &vkind.String
		}
		if vstate.Valid {
			p.VariantState = &vstate.String
		}
		p.variantID = vid.String
		p.States = map[string]RunPluginState{}
		if r := byID[rid]; r != nil {
			r.Participants = append(r.Participants, p)
		}
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return internal(err)
	}
	if err := loadRunRandomizers(ctx, q, in, args, byID); err != nil {
		return err
	}
	if !withState {
		return nil
	}

	rows, err = q.QueryContext(ctx, `SELECT run_id, user_id, plugin_id, state, rev, updated_at FROM run_states WHERE run_id IN `+in, args...)
	if err != nil {
		return internal(err)
	}
	defer rows.Close()
	for rows.Next() {
		var rid, uid, pid, state string
		var rev, upd int64
		if err := rows.Scan(&rid, &uid, &pid, &state, &rev, &upd); err != nil {
			return internal(err)
		}
		r := byID[rid]
		if r == nil {
			continue
		}
		for i := range r.Participants {
			if r.Participants[i].UserID == uid {
				r.Participants[i].States[pid] = RunPluginState{State: json.RawMessage(state), StateRev: rev, UpdatedAt: unixTime(upd)}
			}
		}
	}
	return rows.Err()
}

// viewFor returns the run as userID may see it. Participants who have not joined (invited, declined, left) and
// everybody else see the title, game and participant list only: the plugin states of all participants are removed.
// Events are protected separately (ListRunEvents, WSS pushes). The input run is not modified.
func viewFor(run Run, userID string) Run {
	if p, ok := run.Participant(userID); ok && p.Status == ParticipantJoined {
		return run
	}
	ps := make([]RunParticipant, len(run.Participants))
	for i, p := range run.Participants {
		p.States = map[string]RunPluginState{}
		ps[i] = p
	}
	run.Participants = ps
	return run
}

// insertEvent appends an event to the run (seq is the next number of the run; gapless because the counter and the
// row change in the same transaction).
func insertEvent(ctx context.Context, q dbq, runID string, e RunEvent, now time.Time) (RunEvent, error) {
	if _, err := q.ExecContext(ctx, `UPDATE runs SET last_seq = last_seq + 1 WHERE id = ?`, runID); err != nil {
		return RunEvent{}, internal(err)
	}
	if err := q.QueryRowContext(ctx, `SELECT last_seq FROM runs WHERE id = ?`, runID).Scan(&e.Seq); err != nil {
		return RunEvent{}, internal(err)
	}
	e.CreatedAt = now
	if e.Payload == nil {
		e.Payload = json.RawMessage("{}")
	}
	_, err := q.ExecContext(ctx, `INSERT INTO run_events(run_id, seq, created_at, user_id, user_name, device_id, plugin_id, source, type, text, payload, client_id)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`, runID, e.Seq, now.Unix(), e.UserID, e.UserName, e.DeviceID, e.PluginID, e.Source, e.Type, e.Text,
		string(e.Payload), e.ClientID)
	if err != nil {
		return RunEvent{}, internal(err)
	}
	return e, nil
}

// hubEvent builds a server event (no user, no device, no plugin); who it is about goes into the payload.
func hubEvent(typ string, payload map[string]any) RunEvent {
	b, _ := json.Marshal(payload)
	return RunEvent{Source: EventHub, Type: typ, Payload: b, ClientID: uuid.NewString()}
}

func scanEvent(sc interface{ Scan(...any) error }) (RunEvent, error) {
	var e RunEvent
	var created int64
	var uid, dev, plug sql.NullString
	var payload string
	if err := sc.Scan(&e.Seq, &created, &uid, &e.UserName, &dev, &plug, &e.Source, &e.Type, &e.Text, &payload, &e.ClientID); err != nil {
		return RunEvent{}, err
	}
	e.CreatedAt, e.Payload = unixTime(created), json.RawMessage(payload)
	if uid.Valid {
		e.UserID = &uid.String
	}
	if dev.Valid {
		e.DeviceID = &dev.String
	}
	if plug.Valid {
		e.PluginID = &plug.String
	}
	return e, nil
}

const eventCols = `seq, created_at, user_id, user_name, device_id, plugin_id, source, type, text, payload, client_id`

func loadEvents(ctx context.Context, q dbq, runID string, after, upTo int64, limit int) ([]RunEvent, error) {
	rows, err := q.QueryContext(ctx, `SELECT `+eventCols+` FROM run_events WHERE run_id = ? AND seq > ? AND seq <= ? ORDER BY seq LIMIT ?`,
		runID, after, upTo, limit)
	if err != nil {
		return nil, internal(err)
	}
	defer rows.Close()
	out := []RunEvent{}
	for rows.Next() {
		e, err := scanEvent(rows)
		if err != nil {
			return nil, internal(err)
		}
		out = append(out, e)
	}
	return out, rows.Err()
}

// ---- fan-out ----

// pushRunUpdated sends run_updated to every connection of every participant (all statuses). Joined participants get
// the plugin states, the others the run without states (viewFor).
func (s *Service) pushRunUpdated(run Run) {
	var joinedUsers []string
	for _, p := range run.Participants {
		if p.Status == ParticipantJoined {
			joinedUsers = append(joinedUsers, p.UserID)
		}
	}
	if run.Status == RunPaused || run.Finished() || run.Archived { // left the active state: ready check and pause end
		s.endTogether(scopeRun, run.ID, joinedUsers)
	} else {
		s.reconcileTogether(scopeRun, run.ID, joinedUsers)
	}
	joined := map[string]bool{}
	users := map[string]bool{}
	for _, p := range run.Participants {
		users[p.UserID] = true
		joined[p.UserID] = p.Status == ParticipantJoined
	}
	var full, plain []byte
	for _, c := range s.clients() {
		if !users[c.userID] {
			continue
		}
		if joined[c.userID] {
			if full == nil {
				full = encode("run_updated", "", map[string]any{"run": run})
			}
			c.sendRun(full)
			continue
		}
		if plain == nil {
			plain = encode("run_updated", "", map[string]any{"run": viewFor(run, "")})
		}
		c.sendRun(plain)
	}
}

// pushRunEvents sends run_event for each event to every connection of the joined participants and of extra users
// (the subject of a leave/decline, who is no longer joined). Callers hold runs.mu.
func (s *Service) pushRunEvents(ctx context.Context, runID string, evs []RunEvent, extra ...string) {
	joined, err := runUsers(ctx, s.db, runID, true)
	if err != nil {
		return
	}
	users := map[string]bool{}
	for _, u := range append(joined, extra...) {
		users[u] = true
	}
	var msgs [][]byte
	for _, e := range evs {
		msgs = append(msgs, encode("run_event", "", map[string]any{"run_id": runID, "event": e}))
	}
	for _, c := range s.clients() {
		if users[c.userID] {
			for _, m := range msgs {
				c.sendRun(m)
			}
		}
	}
}

// pushRunState sends run_state to the joined participants' connections except the writing device.
func (s *Service) pushRunState(ctx context.Context, runID, userID, pluginID, deviceID string, rev int64, state json.RawMessage) {
	joined, err := runUsers(ctx, s.db, runID, true)
	if err != nil {
		return
	}
	users := map[string]bool{}
	for _, u := range joined {
		users[u] = true
	}
	msg := encode("run_state", "", map[string]any{"run_id": runID, "user_id": userID, "plugin_id": pluginID, "state_rev": rev, "state": state})
	for _, c := range s.clients() {
		if users[c.userID] && c.deviceID != deviceID {
			c.sendRun(msg)
		}
	}
}

// ---- create / read ----

// CreateRunInput is the request to create a run. UserID/DeviceID are the creator.
type CreateRunInput struct {
	UserID        string
	Name          string
	Plugins       []RunPluginRef
	GameID        string
	InviteUserIDs []string
	Settings      json.RawMessage
	// Randomizer is the optional randomizer recipe (nil = mode off), see RunRandomizerInput.
	Randomizer *RunRandomizerInput
	Rules      *RunRules // nil = DefaultRunRules
}

// CreateRun creates a run in status starting. The creator joins with GameID; invited users get run_updated.
// Every plugin version must be offered by the plugin catalog.
func (s *Service) CreateRun(ctx context.Context, in CreateRunInput) (_ Run, err error) {
	defer s.publishOK(&err, TopicRuns)
	name := strings.TrimSpace(in.Name)
	if n := utf8.RuneCountInString(name); n < 1 || n > MaxRunNameLen {
		return Run{}, badRequest("Name must be 1 to %d characters long", MaxRunNameLen)
	}
	if len(in.Plugins) > MaxRunPlugins {
		return Run{}, badRequest("A run has at most %d plugins", MaxRunPlugins)
	}
	settings, err := objectJSON(in.Settings, MaxRunSettingsBytes, "settings")
	if err != nil {
		return Run{}, err
	}
	rules := DefaultRunRules()
	if in.Rules != nil {
		rules = *in.Rules
		if err := rules.Validate(); err != nil {
			return Run{}, err
		}
	}
	rulesJSON, _ := json.Marshal(rules)
	seen := map[string]bool{}
	plugins := make([]RunPlugin, 0, len(in.Plugins))
	cat := s.pluginCatalog()
	for _, p := range in.Plugins {
		if !runPluginIDRe.MatchString(p.PluginID) || len(p.PluginID) > 128 || p.Version == "" || len(p.Version) > 64 {
			return Run{}, badRequest("Invalid plugin reference")
		}
		if seen[p.PluginID] {
			return Run{}, badRequest("Plugin %s is listed twice", p.PluginID)
		}
		seen[p.PluginID] = true
		if cat == nil {
			return Run{}, badRequest("Plugin %s %s is not offered by this server", p.PluginID, p.Version)
		}
		sha, ok, err := cat.OfferedPlugin(ctx, p.PluginID, p.Version)
		if err != nil {
			return Run{}, err
		}
		if !ok {
			return Run{}, badRequest("Plugin %s %s is not offered by this server", p.PluginID, p.Version)
		}
		plugins = append(plugins, RunPlugin{PluginID: p.PluginID, Version: p.Version, SHA256: sha})
	}
	game, err := s.GetGameFor(ctx, in.UserID, in.GameID)
	if err != nil {
		if errors.Is(err, ErrNotFound) {
			return Run{}, badRequest("Unknown game")
		}
		return Run{}, err
	}
	rz, err := s.newRunRandomizer(ctx, in.Randomizer, game)
	if err != nil {
		return Run{}, err
	}
	if rz != nil {
		s.tools.usageMu.RLock() // against RemoveToolVersion between the catalog check and the insert
		defer s.tools.usageMu.RUnlock()
	}
	creator, err := s.GetUser(ctx, in.UserID)
	if err != nil {
		return Run{}, err
	}
	var invites []string
	dup := map[string]bool{in.UserID: true}
	for _, id := range in.InviteUserIDs {
		if dup[id] {
			continue
		}
		dup[id] = true
		u, err := s.GetUser(ctx, id)
		if errors.Is(err, ErrNotFound) || (err == nil && u.Disabled()) {
			return Run{}, badRequest("Unknown user %q", id)
		}
		if err != nil {
			return Run{}, err
		}
		invites = append(invites, id)
	}
	if 1+len(invites) > MaxRunParticipants {
		return Run{}, badRequest("A run has at most %d participants", MaxRunParticipants)
	}

	s.runs.mu.Lock()
	defer s.runs.mu.Unlock()
	// Lock order is runs.mu, then plug.mu (as in setRunStatus and DeleteRun, which prune plugins). Holding plug.mu
	// from the catalog re-check to the commit means no prune or remove can delete a pinned version in between:
	// they check "in use" under the same lock and see the committed run.
	s.plug.mu.Lock()
	defer s.plug.mu.Unlock()
	for _, p := range plugins {
		sha, ok, err := cat.OfferedPlugin(ctx, p.PluginID, p.Version)
		if err != nil {
			return Run{}, err
		}
		if !ok || sha != p.SHA256 {
			return Run{}, badRequest("Plugin %s %s is not offered by this server", p.PluginID, p.Version)
		}
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return Run{}, internal(err)
	}
	defer tx.Rollback()
	now := s.now().UTC()
	id := uuid.NewString()
	if _, err := tx.ExecContext(ctx, `INSERT INTO runs(id, name, status, archived, created_by, created_at, updated_at, status_by, settings, last_seq, rules)
		VALUES (?, ?, 'starting', 0, ?, ?, ?, ?, ?, 0, ?)`, id, name, in.UserID, now.Unix(), now.Unix(), in.UserID, string(settings), string(rulesJSON)); err != nil {
		return Run{}, internal(err)
	}
	for i, p := range plugins {
		if _, err := tx.ExecContext(ctx, `INSERT INTO run_plugins(run_id, plugin_id, version, sha256, position) VALUES (?, ?, ?, ?, ?)`,
			id, p.PluginID, p.Version, p.SHA256, i); err != nil {
			return Run{}, internal(err)
		}
	}
	if _, err := tx.ExecContext(ctx, `INSERT INTO run_participants(run_id, user_id, status, game_id, color, invited_at) VALUES (?, ?, 'joined', ?, 0, ?)`,
		id, in.UserID, in.GameID, now.Unix()); err != nil {
		return Run{}, internal(err)
	}
	if rz != nil {
		if err := insertRunRandomizer(ctx, tx, id, in.UserID, rz); err != nil {
			return Run{}, err
		}
	}
	for _, u := range invites {
		if _, err := tx.ExecContext(ctx, `INSERT INTO run_participants(run_id, user_id, status, invited_at) VALUES (?, ?, 'invited', ?)`,
			id, u, now.Unix()); err != nil {
			return Run{}, internal(err)
		}
	}
	var evs []RunEvent
	for _, e := range []RunEvent{
		hubEvent("run.created", map[string]any{"user_id": in.UserID, "user_name": creator.DisplayName, "name": name}),
		hubEvent("run.joined", map[string]any{"user_id": in.UserID, "user_name": creator.DisplayName, "game_id": in.GameID}),
	} {
		e, err := insertEvent(ctx, tx, id, e, now)
		if err != nil {
			return Run{}, err
		}
		evs = append(evs, e)
	}
	if err := tx.Commit(); err != nil {
		return Run{}, internal(err)
	}
	run, err := loadRun(ctx, s.db, id, true)
	if err != nil {
		return Run{}, err
	}
	s.pushRunUpdated(run)
	s.pushRunEvents(ctx, id, evs)
	return run, nil
}

// GetRun returns a run the user participates in (any status); everybody else gets ErrNotFound.
func (s *Service) GetRun(ctx context.Context, userID, runID string) (Run, error) {
	run, err := loadRun(ctx, s.db, runID, true)
	if err != nil {
		return Run{}, err
	}
	if _, ok := run.Participant(userID); !ok {
		return Run{}, ErrNotFound
	}
	rs := []Run{viewFor(run, userID)}
	if err := s.fillRunPlayTimes(ctx, rs); err != nil {
		return Run{}, err
	}
	return rs[0], nil
}

// ListRuns returns the non-archived runs the user participates in, most recently changed first.
func (s *Service) ListRuns(ctx context.Context, userID string) ([]Run, error) {
	ids, err := s.runIDsOf(ctx, userID)
	if err != nil {
		return nil, err
	}
	out, err := loadRuns(ctx, s.db, ids, true) // runs deleted in between are skipped
	if err != nil {
		return nil, err
	}
	for i := range out {
		out[i] = viewFor(out[i], userID)
	}
	if err := s.fillRunPlayTimes(ctx, out); err != nil {
		return nil, err
	}
	return out, nil
}

func (s *Service) runIDsOf(ctx context.Context, userID string) ([]string, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT r.id FROM runs r JOIN run_participants p ON p.run_id = r.id
		WHERE p.user_id = ? AND r.archived = 0 ORDER BY r.updated_at DESC, r.created_at DESC, r.id`, userID)
	if err != nil {
		return nil, internal(err)
	}
	defer rows.Close()
	var ids []string
	for rows.Next() {
		var id string
		if err := rows.Scan(&id); err != nil {
			return nil, internal(err)
		}
		ids = append(ids, id)
	}
	return ids, rows.Err()
}

// ListRunEvents returns the events with seq > after (at most limit, in seq order) and the run's last_seq.
func (s *Service) ListRunEvents(ctx context.Context, userID, runID string, after int64, limit int) ([]RunEvent, int64, error) {
	if after < 0 || limit < 1 || limit > MaxRunEventsPage {
		return nil, 0, badRequest("after must be >= 0 and limit between 1 and %d", MaxRunEventsPage)
	}
	if err := joinedParticipant(ctx, s.db, runID, userID); err != nil { // invited, declined and left users: 403
		return nil, 0, err
	}
	var last int64
	if err := s.db.QueryRowContext(ctx, `SELECT last_seq FROM runs WHERE id = ?`, runID).Scan(&last); err != nil {
		return nil, 0, internal(err)
	}
	evs, err := loadEvents(ctx, s.db, runID, after, last, limit)
	return evs, last, err
}

// ---- membership ----

// JoinRun joins the user with a library game (from invited, declined or left). Joining again with the same game
// is a no-op; with another game it is a conflict.
func (s *Service) JoinRun(ctx context.Context, userID, runID, gameID string) (Run, error) {
	return s.JoinRunWith(ctx, userID, runID, gameID, nil)
}

// JoinRunWith is JoinRun with the variant the person plays in a randomizer run (nil = the run variant). A variant
// choice in a run without randomizer is refused (400). Kind "own" needs allow_own (409 own_variants_off) and a variant of
// the person for the same game with the run's tool and version (409 own_variant_mismatch; 404 for an unknown one).
func (s *Service) JoinRunWith(ctx context.Context, userID, runID, gameID string, jv *JoinVariant) (_ Run, err error) {
	defer s.publishOK(&err, TopicRuns)
	if _, err := s.GetGameFor(ctx, userID, gameID); err != nil {
		if errors.Is(err, ErrNotFound) {
			return Run{}, badRequest("Unknown game")
		}
		return Run{}, err
	}
	name := userDisplayName(ctx, s.db, userID)
	return s.changeMembership(ctx, userID, runID, func(ctx context.Context, tx dbq, run Run, st string) (*RunEvent, bool, error) {
		if st == ParticipantJoined {
			p, _ := run.Participant(userID)
			if p.GameID != nil && *p.GameID == gameID {
				return nil, false, nil
			}
			return nil, false, conflict("Already joined with another game; leave the run first")
		}
		color, err := nextRunColor(ctx, tx, runID, userID)
		if err != nil {
			return nil, false, err
		}
		kind, variantID, err := s.checkJoinVariant(ctx, tx, run, userID, gameID, jv)
		if err != nil {
			return nil, false, err
		}
		if _, err := tx.ExecContext(ctx, `UPDATE run_participants SET status = 'joined', game_id = ?, color = COALESCE(color, ?) WHERE run_id = ? AND user_id = ?`,
			gameID, color, runID, userID); err != nil {
			return nil, false, internal(err)
		}
		if kind != "" {
			if _, err := tx.ExecContext(ctx, `UPDATE run_participants SET variant_kind = ?, variant_id = ?, variant_state = 'not_built' WHERE run_id = ? AND user_id = ?`,
				kind, nullIfEmpty(variantID), runID, userID); err != nil {
				return nil, false, internal(err)
			}
		}
		e := hubEvent("run.joined", map[string]any{"user_id": userID, "user_name": name, "game_id": gameID})
		return &e, true, nil
	})
}

// nextRunColor returns the lowest colour (0-7) no other participant of the run holds.
func nextRunColor(ctx context.Context, q dbq, runID, userID string) (int, error) {
	rows, err := q.QueryContext(ctx, `SELECT color FROM run_participants WHERE run_id = ? AND color IS NOT NULL AND user_id <> ?`, runID, userID)
	if err != nil {
		return 0, internal(err)
	}
	defer rows.Close()
	used := map[int]bool{}
	for rows.Next() {
		var c int
		if err := rows.Scan(&c); err != nil {
			return 0, internal(err)
		}
		used[c] = true
	}
	for c := 0; c < MaxRunParticipants; c++ {
		if !used[c] {
			return c, rows.Err()
		}
	}
	return 0, conflict("No free player colour")
}

// DeclineRun declines an invite (only from invited; declining again is a no-op).
func (s *Service) DeclineRun(ctx context.Context, userID, runID string) (_ Run, err error) {
	defer s.publishOK(&err, TopicRuns)
	name := userDisplayName(ctx, s.db, userID)
	return s.changeMembership(ctx, userID, runID, func(ctx context.Context, tx dbq, run Run, st string) (*RunEvent, bool, error) {
		switch st {
		case ParticipantDeclined:
			return nil, false, nil
		case ParticipantInvited:
		default:
			return nil, false, conflict("Only an invite can be declined; leave the run instead")
		}
		if _, err := tx.ExecContext(ctx, `UPDATE run_participants SET status = 'declined' WHERE run_id = ? AND user_id = ?`, runID, userID); err != nil {
			return nil, false, internal(err)
		}
		e := hubEvent("run.declined", map[string]any{"user_id": userID, "user_name": name})
		return &e, true, nil
	})
}

// LeaveRun leaves a run the user joined (leaving again is a no-op). The player colour and the state stay.
func (s *Service) LeaveRun(ctx context.Context, userID, runID string) (_ Run, err error) {
	defer s.publishOK(&err, TopicRuns)
	name := userDisplayName(ctx, s.db, userID)
	return s.changeMembership(ctx, userID, runID, func(ctx context.Context, tx dbq, run Run, st string) (*RunEvent, bool, error) {
		switch st {
		case ParticipantLeft:
			return nil, false, nil
		case ParticipantJoined:
		default:
			return nil, false, conflict("You have not joined this run")
		}
		if _, err := tx.ExecContext(ctx, `UPDATE run_participants SET status = 'left' WHERE run_id = ? AND user_id = ?`, runID, userID); err != nil {
			return nil, false, internal(err)
		}
		e := hubEvent("run.left", map[string]any{"user_id": userID, "user_name": name})
		return &e, true, nil
	})
}

// InviteRun invites further users to a run that is starting, active or paused. Any joined participant may invite.
// Users who are already invited or joined are skipped; users who declined or left are invited again. If nobody is
// newly invited nothing is written. More than MaxRunParticipants participants in total are refused (409).
func (s *Service) InviteRun(ctx context.Context, userID, runID string, userIDs []string) (_ Run, err error) {
	defer s.publishOK(&err, TopicRuns)
	if len(userIDs) < 1 || len(userIDs) > MaxRunParticipants {
		return Run{}, badRequest("Invite 1 to %d users", MaxRunParticipants)
	}
	names := map[string]string{}
	var ids []string
	for _, id := range userIDs {
		if _, dup := names[id]; dup {
			continue
		}
		u, err := s.GetUser(ctx, id)
		if errors.Is(err, ErrNotFound) || (err == nil && u.Disabled()) {
			return Run{}, badRequest("Unknown user %q", id)
		}
		if err != nil {
			return Run{}, err
		}
		names[id] = u.DisplayName
		ids = append(ids, id)
	}
	inviter := userDisplayName(ctx, s.db, userID)
	return s.changeMembership(ctx, userID, runID, func(ctx context.Context, tx dbq, run Run, st string) (*RunEvent, bool, error) {
		if st != ParticipantJoined {
			return nil, false, &Error{Code: CodeForbidden, Message: "Join the run first"}
		}
		now := s.now().UTC()
		total := len(run.Participants)
		var added []map[string]any
		for _, id := range ids {
			if p, ok := run.Participant(id); ok {
				if p.Status != ParticipantDeclined && p.Status != ParticipantLeft {
					continue
				}
				if _, err := tx.ExecContext(ctx, `UPDATE run_participants SET status = 'invited', invited_at = ? WHERE run_id = ? AND user_id = ?`,
					now.Unix(), runID, id); err != nil {
					return nil, false, internal(err)
				}
			} else {
				total++
				if total > MaxRunParticipants {
					return nil, false, conflict(fmt.Sprintf("A run has at most %d participants", MaxRunParticipants))
				}
				if _, err := tx.ExecContext(ctx, `INSERT INTO run_participants(run_id, user_id, status, invited_at) VALUES (?, ?, 'invited', ?)`,
					runID, id, now.Unix()); err != nil {
					return nil, false, internal(err)
				}
			}
			added = append(added, map[string]any{"user_id": id, "user_name": names[id]})
		}
		if len(added) == 0 {
			return nil, false, nil
		}
		e := hubEvent("run.invited", map[string]any{"user_id": userID, "user_name": inviter, "invited": added})
		return &e, true, nil
	})
}

// changeMembership runs f in a transaction for a participant of a run that is not over. f returns the server event to
// append and whether anything changed (false: nothing is written, nothing pushed).
func (s *Service) changeMembership(ctx context.Context, userID, runID string,
	f func(ctx context.Context, tx dbq, run Run, st string) (*RunEvent, bool, error)) (Run, error) {
	s.runs.mu.Lock()
	defer s.runs.mu.Unlock()
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return Run{}, internal(err)
	}
	defer tx.Rollback()
	run, err := loadRun(ctx, tx, runID, true)
	if err != nil {
		return Run{}, err
	}
	p, ok := run.Participant(userID)
	if !ok {
		return Run{}, ErrNotFound
	}
	if run.Finished() {
		return Run{}, conflict("The run is over")
	}
	e, changed, err := f(ctx, tx, run, p.Status)
	if err != nil {
		return Run{}, err
	}
	if !changed {
		return viewFor(run, userID), nil
	}
	now := s.now().UTC()
	if _, err := tx.ExecContext(ctx, `UPDATE runs SET updated_at = ? WHERE id = ?`, now.Unix(), runID); err != nil {
		return Run{}, internal(err)
	}
	ev, err := insertEvent(ctx, tx, runID, *e, now)
	if err != nil {
		return Run{}, err
	}
	if err := tx.Commit(); err != nil {
		return Run{}, internal(err)
	}
	run, err = loadRun(ctx, s.db, runID, true)
	if err != nil {
		return Run{}, err
	}
	s.pushRunUpdated(run)
	s.pushRunEvents(ctx, runID, []RunEvent{ev}, userID)
	return viewFor(run, userID), nil
}

// ---- status ----

// SetRunStatus changes the status of a run as a participant (see the transition table in setRunStatus).
func (s *Service) SetRunStatus(ctx context.Context, userID, runID, status string) (Run, error) {
	return s.setRunStatus(ctx, userID, runID, status, false, false)
}

// SetRunStatusAdmin changes the status as the server admin (admin API), without being a participant: pause,
// resume, start (from "starting") and finish or abandon a run that is not over.
func (s *Service) SetRunStatusAdmin(ctx context.Context, adminID, runID, status string) (Run, error) {
	return s.setRunStatus(ctx, adminID, runID, status, true, false)
}

// StartRunAdmin starts a run that is still "starting" as the server admin ("Start now" in the app). With
// withoutInvited, participants who are only invited and have not joined are removed first ("Start without Sam"),
// in the same transaction as the status change.
func (s *Service) StartRunAdmin(ctx context.Context, adminID, runID string, withoutInvited bool) (Run, error) {
	return s.setRunStatus(ctx, adminID, runID, RunActive, true, withoutInvited)
}

// setRunStatus applies the transitions of the contract: active (from starting by the creator, from paused by any
// joined participant), paused (from active, any joined participant), finished|abandoned (creator, from any run that
// is not over). Other transitions are conflicts; a wrong actor is forbidden.
func (s *Service) setRunStatus(ctx context.Context, actorID, runID, to string, admin, dropInvited bool) (_ Run, err error) {
	defer s.publishOK(&err, TopicRuns)
	switch to {
	case RunActive, RunPaused, RunFinished, RunAbandoned:
	default:
		return Run{}, badRequest("status must be active, paused, finished or abandoned")
	}
	actorName := userDisplayName(ctx, s.db, actorID)
	s.runs.mu.Lock()
	defer s.runs.mu.Unlock()
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return Run{}, internal(err)
	}
	defer tx.Rollback()
	run, err := loadRun(ctx, tx, runID, true)
	if err != nil {
		return Run{}, err
	}
	joined := false
	if admin {
		joined = true
	} else {
		p, ok := run.Participant(actorID)
		if !ok {
			return Run{}, ErrNotFound
		}
		joined = p.Status == ParticipantJoined
	}
	from := run.Status
	creator := run.CreatedBy == actorID
	var allowed, valid bool
	switch {
	case to == RunActive && from == RunStarting:
		valid, allowed = true, creator || admin
	case to == RunActive && from == RunPaused, to == RunPaused && from == RunActive:
		valid, allowed = true, joined
	case (to == RunFinished || to == RunAbandoned) && !run.Finished():
		valid, allowed = true, creator || admin
	}
	if !valid {
		return Run{}, conflict("The run cannot change from " + from + " to " + to)
	}
	if !allowed {
		return Run{}, &Error{Code: CodeForbidden, Message: "Not allowed to change the run to " + to}
	}
	now := s.now().UTC()
	var evs []RunEvent
	if dropInvited {
		for _, p := range run.Participants {
			if p.Status != ParticipantInvited {
				continue
			}
			if _, err := tx.ExecContext(ctx, `DELETE FROM run_participants WHERE run_id = ? AND user_id = ? AND status = 'invited'`, runID, p.UserID); err != nil {
				return Run{}, internal(err)
			}
			de, err := insertEvent(ctx, tx, runID, hubEvent("run.invite_removed", map[string]any{
				"user_id": actorID, "user_name": actorName, "target_user_id": p.UserID, "target_user_name": p.UserName}), now)
			if err != nil {
				return Run{}, err
			}
			evs = append(evs, de)
		}
	}
	if _, err := tx.ExecContext(ctx, `UPDATE runs SET status = ?, status_by = ?, updated_at = ? WHERE id = ?`, to, actorID, now.Unix(), runID); err != nil {
		return Run{}, internal(err)
	}
	ev, err := insertEvent(ctx, tx, runID, hubEvent("run.status", map[string]any{
		"status": to, "previous": from, "by_user_id": actorID, "by_user_name": actorName}), now)
	if err != nil {
		return Run{}, err
	}
	evs = append(evs, ev)
	if err := tx.Commit(); err != nil {
		return Run{}, internal(err)
	}
	if to == RunFinished || to == RunAbandoned {
		s.PruneOldPlugins(ctx) // the run no longer pins its plugin versions
	}
	run, err = loadRun(ctx, s.db, runID, true)
	if err != nil {
		return Run{}, err
	}
	s.pushRunUpdated(run)
	s.pushRunEvents(ctx, runID, evs)
	return viewFor(run, actorID), nil
}

// ---- state, events, boards ----

// PutRunState replaces the caller's own state of one plugin of the run if baseRev is that plugin state's current
// state_rev (0 when none was written). A different base returns the current state as conflict (and nil error) so the
// caller can merge and retry.
func (s *Service) PutRunState(ctx context.Context, userID, deviceID, runID, pluginID string, baseRev int64, state json.RawMessage) (int64, *RunStateConflict, error) {
	s.runs.mu.Lock()
	defer s.runs.mu.Unlock()
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return 0, nil, internal(err)
	}
	defer tx.Rollback()
	if err := joinedParticipant(ctx, tx, runID, userID); err != nil {
		return 0, nil, err
	}
	var np int
	if err := tx.QueryRowContext(ctx, `SELECT COUNT(*) FROM run_plugins WHERE run_id = ? AND plugin_id = ?`, runID, pluginID).Scan(&np); err != nil {
		return 0, nil, internal(err)
	}
	if np == 0 {
		return 0, nil, ErrNotFound
	}
	if !s.runAllow("state", runID, userID, runStateRate, runStateBurst) {
		return 0, nil, errRunRate
	}
	body, err := objectJSON(state, MaxRunStateBytes, "state")
	if err != nil {
		return 0, nil, err
	}
	if baseRev < 0 {
		return 0, nil, badRequest("base_rev must be >= 0")
	}
	if err := requireActive(ctx, tx, runID); err != nil {
		return 0, nil, err
	}
	var cur int64
	curState := "{}"
	err = tx.QueryRowContext(ctx, `SELECT rev, state FROM run_states WHERE run_id = ? AND user_id = ? AND plugin_id = ?`, runID, userID, pluginID).Scan(&cur, &curState)
	if err != nil && !errors.Is(err, sql.ErrNoRows) {
		return 0, nil, internal(err)
	}
	if cur != baseRev {
		return 0, &RunStateConflict{StateRev: cur, State: json.RawMessage(curState)}, nil
	}
	now := s.now().UTC()
	if _, err := tx.ExecContext(ctx, `INSERT INTO run_states(run_id, user_id, plugin_id, state, rev, updated_at) VALUES (?, ?, ?, ?, ?, ?)
		ON CONFLICT(run_id, user_id, plugin_id) DO UPDATE SET state = excluded.state, rev = excluded.rev, updated_at = excluded.updated_at`,
		runID, userID, pluginID, string(body), cur+1, now.Unix()); err != nil {
		return 0, nil, internal(err)
	}
	if err := tx.Commit(); err != nil {
		return 0, nil, internal(err)
	}
	s.pushRunState(ctx, runID, userID, pluginID, deviceID, cur+1, body)
	s.Publish(TopicRuns)
	return cur + 1, nil, nil
}

// requireActive returns ErrRunNotActive unless the run is starting or active.
func requireActive(ctx context.Context, q dbq, runID string) error {
	var st string
	if err := q.QueryRowContext(ctx, `SELECT status FROM runs WHERE id = ?`, runID).Scan(&st); err != nil {
		return internal(err)
	}
	if st != RunStarting && st != RunActive {
		return ErrRunNotActive
	}
	return nil
}

// AppendRunEventInput is a plugin event posted by a joined participant's device.
type AppendRunEventInput struct {
	UserID, DeviceID, RunID string
	ClientID                string
	PluginID                string
	Source                  string
	Type                    string
	Text                    string
	Payload                 json.RawMessage
}

// AppendRunEvent appends an event. The same client_id again returns the original seq with created=false.
func (s *Service) AppendRunEvent(ctx context.Context, in AppendRunEventInput) (seq int64, created bool, err error) {
	s.runs.mu.Lock()
	defer s.runs.mu.Unlock()
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return 0, false, internal(err)
	}
	defer tx.Rollback()
	if err := joinedParticipant(ctx, tx, in.RunID, in.UserID); err != nil {
		return 0, false, err
	}
	var prevSeq int64
	var prevUser sql.NullString
	err = tx.QueryRowContext(ctx, `SELECT seq, user_id FROM run_events WHERE run_id = ? AND client_id = ?`, in.RunID, in.ClientID).Scan(&prevSeq, &prevUser)
	switch {
	case err == nil:
		if prevUser.String != in.UserID {
			return 0, false, conflict("client_id is already used in this run")
		}
		return prevSeq, false, nil
	case !errors.Is(err, sql.ErrNoRows):
		return 0, false, internal(err)
	}
	if !s.runAllow("event", in.RunID, in.UserID, runEventRate, runEventBurst) {
		return 0, false, errRunRate
	}
	if _, err := uuid.Parse(in.ClientID); err != nil {
		return 0, false, badRequest("client_id must be a UUID")
	}
	if in.Source != EventAutomatic && in.Source != EventManual {
		return 0, false, badRequest("source must be automatic or manual")
	}
	if !runEventTypeRe.MatchString(in.Type) {
		return 0, false, badRequest("type must match ^[a-z0-9_.-]{1,64}$")
	}
	core := coreRunEventTypes[in.Type]
	if in.PluginID == "" && !core {
		return 0, false, badRequest("plugin_id is required")
	}
	if in.PluginID != "" && (!runPluginIDRe.MatchString(in.PluginID) || len(in.PluginID) > 128) {
		return 0, false, badRequest("Invalid plugin reference")
	}
	if utf8.RuneCountInString(in.Text) > MaxRunEventText {
		return 0, false, badRequest("text is longer than %d characters", MaxRunEventText)
	}
	payload, err := objectJSON(in.Payload, MaxRunPayloadBytes, "payload")
	if err != nil {
		return 0, false, err
	}
	if !core { // the app's own events (cheats, memory writes, state loads) may come without a plugin or from a plugin that is not part of the run
		var n int
		if err := tx.QueryRowContext(ctx, `SELECT COUNT(*) FROM run_plugins WHERE run_id = ? AND plugin_id = ?`, in.RunID, in.PluginID).Scan(&n); err != nil {
			return 0, false, internal(err)
		}
		if n == 0 {
			return 0, false, badRequest("plugin_id is not a plugin of this run")
		}
	}
	if err := requireActive(ctx, tx, in.RunID); err != nil {
		return 0, false, err
	}
	var last int64
	if err := tx.QueryRowContext(ctx, `SELECT last_seq FROM runs WHERE id = ?`, in.RunID).Scan(&last); err != nil {
		return 0, false, internal(err)
	}
	if last >= MaxRunEvents {
		return 0, false, conflict("The run has reached its limit of events")
	}
	user, dev := in.UserID, in.DeviceID
	var plug *string
	if in.PluginID != "" {
		plug = &in.PluginID
	}
	ev, err := insertEvent(ctx, tx, in.RunID, RunEvent{UserID: &user, UserName: userDisplayName(ctx, tx, in.UserID), DeviceID: &dev, PluginID: plug,
		Source: in.Source, Type: in.Type, Text: in.Text, Payload: payload, ClientID: in.ClientID}, s.now().UTC())
	if err != nil {
		return 0, false, err
	}
	if err := tx.Commit(); err != nil {
		return 0, false, internal(err)
	}
	s.pushRunEvents(ctx, in.RunID, []RunEvent{ev})
	s.Publish(TopicRuns)
	return ev.Seq, true, nil
}

// PutRunBoard stores the board tree of a plugin of the run (the app renders it) and returns the new rev.
func (s *Service) PutRunBoard(ctx context.Context, userID, runID, pluginID string, tree json.RawMessage) (int64, error) {
	s.runs.mu.Lock()
	defer s.runs.mu.Unlock()
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return 0, internal(err)
	}
	defer tx.Rollback()
	if err := joinedParticipant(ctx, tx, runID, userID); err != nil {
		return 0, err
	}
	var n int
	if err := tx.QueryRowContext(ctx, `SELECT COUNT(*) FROM run_plugins WHERE run_id = ? AND plugin_id = ?`, runID, pluginID).Scan(&n); err != nil {
		return 0, internal(err)
	}
	if n == 0 {
		return 0, ErrNotFound
	}
	if !s.runAllow("board", runID, userID, runBoardRate, runBoardBurst) {
		return 0, errRunRate
	}
	body, err := objectJSON(tree, MaxRunBoardBytes, "tree")
	if err != nil {
		return 0, err
	}
	if err := requireActive(ctx, tx, runID); err != nil {
		return 0, err
	}
	if _, err := tx.ExecContext(ctx, `INSERT INTO run_boards(run_id, plugin_id, rev, tree, updated_by, updated_at) VALUES (?, ?, 1, ?, ?, ?)
		ON CONFLICT(run_id, plugin_id) DO UPDATE SET rev = rev + 1, tree = excluded.tree, updated_by = excluded.updated_by, updated_at = excluded.updated_at`,
		runID, pluginID, string(body), userID, s.now().Unix()); err != nil {
		return 0, internal(err)
	}
	var rev int64
	if err := tx.QueryRowContext(ctx, `SELECT rev FROM run_boards WHERE run_id = ? AND plugin_id = ?`, runID, pluginID).Scan(&rev); err != nil {
		return 0, internal(err)
	}
	if err := tx.Commit(); err != nil {
		return 0, internal(err)
	}
	s.Publish(TopicRuns)
	return rev, nil
}

// ---- admin ----

// ActiveRunCount is the number of non-archived runs in status active (sidebar badge).
func (s *Service) ActiveRunCount(ctx context.Context) (int, error) {
	var n int
	if err := s.db.QueryRowContext(ctx, `SELECT COUNT(*) FROM runs WHERE status = 'active' AND archived = 0`).Scan(&n); err != nil {
		return 0, internal(err)
	}
	return n, nil
}

// ListRunsAdmin returns every run (states left out), most recently changed first.
func (s *Service) ListRunsAdmin(ctx context.Context, includeArchived bool) ([]Run, error) {
	q := `SELECT id FROM runs`
	if !includeArchived {
		q += ` WHERE archived = 0`
	}
	rows, err := s.db.QueryContext(ctx, q+` ORDER BY updated_at DESC, created_at DESC, id`)
	if err != nil {
		return nil, internal(err)
	}
	var ids []string
	for rows.Next() {
		var id string
		if err := rows.Scan(&id); err != nil {
			rows.Close()
			return nil, internal(err)
		}
		ids = append(ids, id)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return nil, internal(err)
	}
	out := make([]Run, 0, len(ids))
	for _, id := range ids {
		r, err := loadRun(ctx, s.db, id, false)
		if errors.Is(err, ErrNotFound) {
			continue
		}
		if err != nil {
			return nil, err
		}
		out = append(out, r)
	}
	if err := s.fillRunPlayTimes(ctx, out); err != nil {
		return nil, err
	}
	return out, nil
}

// GetRunAdmin returns any run (without participants' state bodies).
func (s *Service) GetRunAdmin(ctx context.Context, runID string) (Run, error) {
	r, err := loadRun(ctx, s.db, runID, false)
	if err != nil {
		return Run{}, err
	}
	rs := []Run{r}
	if err := s.fillRunPlayTimes(ctx, rs); err != nil {
		return Run{}, err
	}
	return rs[0], nil
}

// RunBoards returns the board trees of a run, in plugin order of the run.
func (s *Service) RunBoards(ctx context.Context, runID string) ([]RunBoard, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT b.plugin_id, b.rev, b.tree, COALESCE(u.display_name, ''), b.updated_at
		FROM run_boards b JOIN run_plugins p ON p.run_id = b.run_id AND p.plugin_id = b.plugin_id
		LEFT JOIN users u ON u.id = b.updated_by WHERE b.run_id = ? ORDER BY p.position`, runID)
	if err != nil {
		return nil, internal(err)
	}
	defer rows.Close()
	var out []RunBoard
	for rows.Next() {
		var b RunBoard
		var tree string
		var at int64
		if err := rows.Scan(&b.PluginID, &b.Rev, &tree, &b.UpdatedBy, &at); err != nil {
			return nil, internal(err)
		}
		b.Tree, b.UpdatedAt = json.RawMessage(tree), unixTime(at)
		out = append(out, b)
	}
	return out, rows.Err()
}

// ListRunEventsAdmin returns events with seq > after (limit 1 to 5000) and the run's last_seq.
func (s *Service) ListRunEventsAdmin(ctx context.Context, runID string, after int64, limit int) ([]RunEvent, int64, error) {
	if limit < 1 || limit > 5000 || after < 0 {
		return nil, 0, badRequest("Invalid range")
	}
	var last int64
	err := s.db.QueryRowContext(ctx, `SELECT last_seq FROM runs WHERE id = ?`, runID).Scan(&last)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, 0, ErrNotFound
	}
	if err != nil {
		return nil, 0, internal(err)
	}
	evs, err := loadEvents(ctx, s.db, runID, after, last, limit)
	return evs, last, err
}

// ArchiveRun hides a finished or abandoned run from the lists (web only; apps do not see archived runs).
func (s *Service) ArchiveRun(ctx context.Context, runID string, archived bool) (err error) {
	defer s.publishOK(&err, TopicRuns)
	s.runs.mu.Lock()
	defer s.runs.mu.Unlock()
	run, err := loadRun(ctx, s.db, runID, false)
	if err != nil {
		return err
	}
	if !run.Finished() {
		return conflict("Only a finished or abandoned run can be archived")
	}
	if _, err := s.db.ExecContext(ctx, `UPDATE runs SET archived = ? WHERE id = ?`, archived, runID); err != nil {
		return internal(err)
	}
	return nil
}

// DeleteRun deletes a run with participants, events and boards for good (web only).
func (s *Service) DeleteRun(ctx context.Context, runID string) (err error) {
	defer s.publishOK(&err, TopicRuns)
	s.runs.mu.Lock()
	defer s.runs.mu.Unlock()
	var joined []string // the members learn that a shared pause ended with the run
	if run, lerr := loadRun(ctx, s.db, runID, false); lerr == nil {
		for _, p := range run.Participants {
			if p.Status == ParticipantJoined {
				joined = append(joined, p.UserID)
			}
		}
	}
	res, err := s.db.ExecContext(ctx, `DELETE FROM runs WHERE id = ?`, runID)
	if err != nil {
		return internal(err)
	}
	if n, _ := res.RowsAffected(); n == 0 {
		return ErrNotFound
	}
	s.dropRunBuckets(runID)
	s.endTogether(scopeRun, runID, joined)
	s.PruneOldPlugins(ctx)
	return nil
}

// ---- catch-up after hello ----

// RunHello is one entry of the optional hello field runs: the last event the app has seen of a run.
type RunHello struct {
	RunID   string `json:"run_id"`
	LastSeq int64  `json:"last_seq"`
}

// maxRunHello bounds the runs list of a hello (entries beyond it are ignored).
const maxRunHello = 256

// runCatchUp builds the messages sent after hello_ack: run_updated for every non-archived run the user participates
// in, then for the runs listed in hello where the user is joined the run_events with seq > last_seq; when more than
// RunCatchUpMax are missing none (the app pages with GET /runs/{run_id}/events). Callers hold runs.mu.
func (s *Service) runCatchUp(ctx context.Context, userID string, listed []RunHello) [][]byte {
	ids, err := s.runIDsOf(ctx, userID)
	if err != nil || len(ids) == 0 {
		return nil
	}
	runs, err := loadRuns(ctx, s.db, ids, true) // one batch, not several queries per run
	if err != nil {
		return nil
	}
	last := map[string]int64{}
	for i, h := range listed {
		if i >= maxRunHello {
			break
		}
		if h.LastSeq >= 0 {
			last[h.RunID] = h.LastSeq
		}
	}
	var msgs, evMsgs [][]byte
	for _, run := range runs {
		id := run.ID
		p, _ := run.Participant(userID)
		msgs = append(msgs, encode("run_updated", "", map[string]any{"run": viewFor(run, userID)}))
		seen, ok := last[id]
		if !ok || p.Status != ParticipantJoined || seen >= run.LastSeq || run.LastSeq-seen > RunCatchUpMax {
			continue
		}
		evs, err := loadEvents(ctx, s.db, id, seen, run.LastSeq, RunCatchUpMax)
		if err != nil {
			continue
		}
		for _, e := range evs {
			evMsgs = append(evMsgs, encode("run_event", "", map[string]any{"run_id": id, "event": e}))
		}
	}
	return append(msgs, evMsgs...)
}

// maxRunBuffered bounds the live run messages held back while a connection's catch-up is delivered.
const maxRunBuffered = 20000

// runDeliverTimeout is how long the catch-up delivery waits for a connection that does not read.
const runDeliverTimeout = time.Minute

// sendRun queues a run message. While the catch-up of this connection is being delivered, live run messages wait
// behind it so the app sees them in order (state and events newer than the snapshot come after it).
func (c *Client) sendRun(b []byte) {
	c.mu.Lock()
	if c.runGate {
		if len(c.runBuf) >= maxRunBuffered {
			c.mu.Unlock()
			c.closeWith(CloseCodePolicy, "slow consumer")
			return
		}
		c.runBuf = append(c.runBuf, b)
		c.mu.Unlock()
		return
	}
	c.mu.Unlock()
	c.sendRaw(b)
}

// startRunCatchUp registers the client, builds its catch-up and starts delivering it. runs.mu is held from before
// the registration until the snapshot is built, so no run message can fall between snapshot and live delivery.
func (s *Service) startRunCatchUp(c *Client, listed []RunHello) {
	s.runs.mu.Lock()
	s.attach(c)
	msgs := s.runCatchUp(context.Background(), c.userID, listed)
	if len(msgs) > 0 {
		c.mu.Lock()
		c.runGate = true
		c.mu.Unlock()
	}
	s.runs.mu.Unlock()
	if len(msgs) > 0 {
		go c.deliverRun(msgs)
	}
}

// deliverRun writes the catch-up messages with back pressure (the out queue is small), then the live messages that
// were held back, and opens the gate.
func (c *Client) deliverRun(msgs [][]byte) {
	timer := time.NewTimer(runDeliverTimeout)
	defer timer.Stop()
	send := func(b []byte) bool {
		select {
		case c.out <- b:
			return true
		case <-c.done:
			return false
		case <-timer.C:
			c.closeWith(CloseCodePolicy, "slow consumer")
			return false
		}
	}
	for _, b := range msgs {
		if !send(b) {
			return
		}
	}
	for {
		c.mu.Lock()
		buf := c.runBuf
		c.runBuf = nil
		if len(buf) == 0 {
			c.runGate = false
			c.mu.Unlock()
			return
		}
		c.mu.Unlock()
		for _, b := range buf {
			if !send(b) {
				return
			}
		}
	}
}
