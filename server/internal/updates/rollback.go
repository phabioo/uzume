package updates

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"time"
)

// Update rollback (ADR 0029 D4, finishes ADR 0011 D8). The functions here are pure selection and bookkeeping; the
// Server service, the root helper and the CLI build on them.

// Setting keys in the server settings table that the rollback uses. They live in the database so that every server
// binary (also an older one that does not know the rollback) sees `update_auto` = 0 after going back.
const (
	SettingAuto    = "update_auto"
	SettingPaused  = "update_paused"
	SettingSkip    = "update_skip"
	SettingHistory = "update_history"
)

// Limits.
const (
	// MaxRollbackCandidates is the number of earlier versions offered.
	MaxRollbackCandidates = 5
	// MaxHistory is the number of versions kept in the local history of installed versions.
	MaxHistory = 20
	// historyTouch is how often the last-used date of the running version is written.
	historyTouch = time.Hour
)

// HistoryEntry is one version that ran on this device, with the first and last time it was seen running.
type HistoryEntry struct {
	Version string    `json:"version"`
	First   time.Time `json:"first"`
	Last    time.Time `json:"last"`
	// Schema is the database schema version the server had while it ran this version (0 = unknown). It lets a later
	// rollback know the schema of a release whose index entry does not say.
	Schema int `json:"schema,omitempty"`
}

// RecordVersion notes that version is running now. It returns the new history and whether it changed (the last
// date is refreshed at most hourly, so the caller can skip a write). The history keeps the newest MaxHistory
// entries by last use.
func RecordVersion(h []HistoryEntry, version string, schema int, now time.Time) ([]HistoryEntry, bool) {
	if !ValidSemVer(version) {
		return h, false
	}
	now = now.UTC().Truncate(time.Second)
	out := append([]HistoryEntry(nil), h...)
	changed := false
	found := false
	for i := range out {
		if out[i].Version != version {
			continue
		}
		found = true
		if now.Sub(out[i].Last) >= historyTouch {
			out[i].Last, changed = now, true
		}
		if schema > 0 && out[i].Schema != schema {
			out[i].Schema, changed = schema, true
		}
	}
	if !found {
		out = append(out, HistoryEntry{Version: version, First: now, Last: now, Schema: schema})
		changed = true
	}
	if len(out) > MaxHistory {
		sort.SliceStable(out, func(i, j int) bool { return out[i].Last.After(out[j].Last) })
		out = out[:MaxHistory]
	}
	return out, changed
}

// ParseHistory decodes the stored history JSON; a damaged value is an empty history.
func ParseHistory(v string) []HistoryEntry {
	if v == "" {
		return nil
	}
	var h []HistoryEntry
	if json.Unmarshal([]byte(v), &h) != nil {
		return nil
	}
	return h
}

// MarshalHistory encodes a history for storage.
func MarshalHistory(h []HistoryEntry) string {
	b, _ := json.Marshal(h)
	return string(b)
}

func historyOf(h []HistoryEntry, version string) (HistoryEntry, bool) {
	for _, e := range h {
		if e.Version == version {
			return e, true
		}
	}
	return HistoryEntry{}, false
}

// Pause is the state after going back: automatic updates stay off until the admin resumes them. It is stored in
// the settings table next to `update_auto` = 0.
type Pause struct {
	// From is the version that was left (the "skipped" version), To the version rolled back to.
	From string    `json:"from"`
	To   string    `json:"to"`
	At   time.Time `json:"at"`
	By   string    `json:"by,omitempty"`
	// Backup is the safety backup archive made before going back; RestoredDB the automatic pre-update database
	// file that replaced the database (empty: the data was kept).
	Backup     string `json:"backup,omitempty"`
	RestoredDB string `json:"restored_db,omitempty"`
	// LostSince: changes after this time are only in the safety backup (set with RestoredDB).
	LostSince time.Time `json:"lost_since,omitzero"`
	// PrevAuto is the value of update_auto before ("", "1" or "0"); Resume puts it back.
	PrevAuto string `json:"prev_auto"`
}

// ParsePause decodes the stored pause; an empty or damaged value is nil.
func ParsePause(v string) *Pause {
	if v == "" {
		return nil
	}
	var p Pause
	if json.Unmarshal([]byte(v), &p) != nil || p.From == "" {
		return nil
	}
	return &p
}

// MarshalPause encodes a pause for storage.
func MarshalPause(p Pause) string {
	b, _ := json.Marshal(p)
	return string(b)
}

// ---- candidates ----

// RollbackQuery selects the earlier versions that can be installed.
type RollbackQuery struct {
	Product, Platform, Kind string
	Current                 string // running version (SemVer)
	Channel                 string // the channel of the install (stable or beta)
	Protocol                int    // protocol_version of the running build; 0 = do not filter
	History                 []HistoryEntry
	Limit                   int // 0 = MaxRollbackCandidates
}

// Candidate is a release that can be rolled back to.
type Candidate struct {
	Release  Release
	Artifact Artifact
	// First and Last come from the local history (zero when the version never ran here).
	First, Last time.Time
	Ran         bool
}

// RollbackCandidates returns the releases strictly older than q.Current that have an artifact for the platform and
// kind, newest first, at most q.Limit. A release counts when it is in a channel of the install (ChannelsFor) or
// when this device ran the version before (the history). It must speak the running protocol version
// (min_protocol_version <= q.Protocol <= protocol_version), so the floor is the oldest version that does. An MSI
// is only offered when it can be installed over a newer version (Release.MSIDowngrade). Companion entries of
// the legacy server index are skipped. A version that exists in several channels appears once (stable first).
func RollbackCandidates(idx Index, q RollbackQuery) []Candidate {
	cur, err := ParseSemVer(q.Current)
	if err != nil {
		return nil
	}
	limit := q.Limit
	if limit <= 0 {
		limit = MaxRollbackCandidates
	}
	chans := ChannelsFor(q.Channel)
	type item struct {
		c Candidate
		v SemVer
	}
	byVersion := map[string]item{}
	for _, r := range idx.Releases {
		if r.Product != q.Product || IsLegacyCompanion(r.Version) {
			continue
		}
		h, ran := historyOf(q.History, r.Version)
		if !ran && !containsStr(chans, r.Channel) {
			continue
		}
		var art *Artifact
		for j := range r.Artifacts {
			if r.Artifacts[j].Platform == q.Platform && r.Artifacts[j].Kind == q.Kind {
				art = &r.Artifacts[j]
			}
		}
		if art == nil {
			continue
		}
		if q.Kind == KindMSI && !r.MSIDowngrade {
			continue
		}
		if q.Protocol > 0 && (r.MinProtocolVersion > q.Protocol || r.ProtocolVersion < q.Protocol) {
			continue
		}
		v, err := ParseSemVer(r.Version)
		if err != nil || v.Compare(cur) >= 0 {
			continue
		}
		if old, dup := byVersion[r.Version]; dup && old.c.Release.Channel == ChannelStable {
			continue
		}
		byVersion[r.Version] = item{Candidate{Release: r, Artifact: *art, First: h.First, Last: h.Last, Ran: ran}, v}
	}
	items := make([]item, 0, len(byVersion))
	for _, it := range byVersion {
		items = append(items, it)
	}
	sort.Slice(items, func(i, j int) bool { return items[i].v.Compare(items[j].v) > 0 })
	if len(items) > limit {
		items = items[:limit]
	}
	out := make([]Candidate, len(items))
	for i, it := range items {
		out[i] = it.c
	}
	return out
}

// ---- schema rule ----

// SchemaKnown returns the database schema version of a candidate: the index entry's value, else the one the
// history recorded while this version ran here, else 0 (unknown).
func SchemaKnown(c Candidate, history []HistoryEntry) int {
	if c.Release.SchemaVersion > 0 {
		return c.Release.SchemaVersion
	}
	if h, ok := historyOf(history, c.Release.Version); ok {
		return h.Schema
	}
	return 0
}

// SchemaAction is what going back does to the database.
type SchemaAction string

// Schema actions.
const (
	// SchemaKeep: the target knows the current schema; the data is kept as it is.
	SchemaKeep SchemaAction = "keep"
	// SchemaRestore: the target needs an older schema; the database is replaced by the automatic pre-update
	// backup and the changes since then are only in the safety backup.
	SchemaRestore SchemaAction = "restore"
	// SchemaRefuse: going back is not safe.
	SchemaRefuse SchemaAction = "refuse"
)

// SchemaPlan is the outcome of PlanSchema.
type SchemaPlan struct {
	Action        SchemaAction
	Reason        string // why it is refused
	Backup        *DBBackup
	TargetSchema  int
	CurrentSchema int
}

// PlanSchema applies the schema rule. Migrations are forward-only, so a target whose binary knows the current
// schema (targetSchema >= currentSchema) keeps the data. A target that needs an older schema can only run on the
// automatic database backup made before a migration (newest one whose source schema the target understands). An
// unknown target schema, or no such backup, refuses: data is never dropped silently.
func PlanSchema(targetSchema, currentSchema int, backups []DBBackup) SchemaPlan {
	p := SchemaPlan{TargetSchema: targetSchema, CurrentSchema: currentSchema}
	switch {
	case targetSchema <= 0:
		p.Action = SchemaRefuse
		p.Reason = "The release index does not say which database schema this version uses, so going back to it could leave the server unable to start. Use the manual steps in https://github.com/phabioo/uzume/wiki/Updates."
	case currentSchema <= 0 || targetSchema >= currentSchema:
		p.Action = SchemaKeep
	default:
		var best *DBBackup
		for i := range backups {
			b := backups[i]
			if b.From <= targetSchema && b.From < currentSchema && (best == nil || b.Time.After(best.Time)) {
				best = &b
			}
		}
		if best == nil {
			p.Action = SchemaRefuse
			p.Reason = fmt.Sprintf("This version uses database schema %d and the server is on schema %d. Going back needs the automatic database backup made before the update, and none exists (the server keeps the newest %d in backups/).",
				targetSchema, currentSchema, dbBackupsKept)
		} else {
			p.Action, p.Backup = SchemaRestore, best
		}
	}
	return p
}

// ---- automatic pre-update database backups (ADR 0011 D6) ----

// DBBackupDir is the folder of the automatic pre-migration database backups inside the data directory.
const DBBackupDir = "backups"

var dbBackupRe = regexp.MustCompile(`^hub-(\d+)-to-(\d+)-(\d{8}T\d{6}Z)\.db$`)

// DBBackup is one hub-<from>-to-<to>-<UTC timestamp>.db file.
type DBBackup struct {
	Name     string
	From, To int
	Time     time.Time
}

// ParseDBBackupName parses a pre-migration backup file name.
func ParseDBBackupName(name string) (DBBackup, bool) {
	m := dbBackupRe.FindStringSubmatch(name)
	if m == nil {
		return DBBackup{}, false
	}
	from, e1 := strconv.Atoi(m[1])
	to, e2 := strconv.Atoi(m[2])
	t, e3 := time.Parse("20060102T150405Z", m[3])
	if e1 != nil || e2 != nil || e3 != nil {
		return DBBackup{}, false
	}
	return DBBackup{Name: name, From: from, To: to, Time: t}, true
}

// ListDBBackups lists <dataDir>/backups/hub-*.db, newest first.
func ListDBBackups(dataDir string) []DBBackup {
	ents, err := os.ReadDir(filepath.Join(dataDir, DBBackupDir))
	if err != nil {
		return nil
	}
	var out []DBBackup
	for _, e := range ents {
		if b, ok := ParseDBBackupName(e.Name()); ok && e.Type().IsRegular() {
			out = append(out, b)
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Time.After(out[j].Time) })
	return out
}

// dbBackupsKept is how many automatic database backups the store keeps (store.BackupsKept).
const dbBackupsKept = 5

// ErrRollbackRefused wraps the reason a rollback is not possible.
var ErrRollbackRefused = errors.New("going back is not possible")
