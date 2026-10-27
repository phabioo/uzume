package hub

import (
	"context"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"time"

	"github.com/google/uuid"
)

// MaxSaveBytes is the maximum size of one save (D1).
const MaxSaveBytes = 64 << 20

// Sync reasons of an upload (API: SaveSyncReason).
const (
	SyncCheckpoint      = "checkpoint"
	SyncFinal           = "final"
	SyncFinalSessionEnd = "final_session_end"
	SyncRestore         = "restore" // checkpoint reason only (restore of a history version)
	SyncUpload          = "upload"  // checkpoint reason only (save file uploaded via UploadSaveFile)
)

// PushConflictResolution is the save_updated reason when a conflict resolution changed the checkpoint.
const PushConflictResolution = "conflict_resolution"

// History reasons (API: SaveHistoryReason).
const (
	HistorySessionEnd       = "session_end"
	HistoryDeviceChange     = "device_change"
	HistoryBeforeResolution = "before_conflict_resolution"
	HistoryConflictUpload   = "conflict_upload"
	HistoryManualSnapshot   = "manual_snapshot"
	HistoryBeforeRestore    = "before_restore"
	HistoryBeforeUpload     = "before_upload"
)

// MaxSnapshotLabel is the maximum length of a snapshot label in characters.
const MaxSnapshotLabel = 120

// AdminDeviceID is the device ID recorded for save changes made in the app (the admin has no device).
func AdminDeviceID(userID string) string { return adminDevicePrefix + userID }

const (
	adminDevicePrefix = "web:"
	adminDeviceName   = "Server admin"
)

// deviceNameSQL is the display name of a device column: the device name, the web label or "Deleted device" (the history keeps the ID after a device was deleted).
func deviceNameSQL(alias, col string) string {
	return "COALESCE(" + alias + ".name, CASE WHEN " + col + " LIKE 'web:%' THEN '" + adminDeviceName + "' ELSE 'Deleted device' END)"
}

// Conflict status and resolutions.
const (
	ConflictOpen          = "open"
	ConflictResolvedHub   = "resolved_hub"
	ConflictResolvedLocal = "resolved_local"

	ResolveUseHub   = "use_hub"
	ResolveUseLocal = "use_local"
)

// MaxSlotName is the maximum length of a slot name.
const MaxSlotName = 32

var slotRe = regexp.MustCompile(fmt.Sprintf(`^[a-z0-9_-]{1,%d}$`, MaxSlotName))

// ValidSlotName checks a slot name (pattern of the API: SaveSlotName).
func ValidSlotName(s string) bool { return slotRe.MatchString(s) }

// reservedSlotRe matches Windows reserved device names, which cannot be directory names there. They match the
// API pattern, so existing slots stay readable; only the creation of new ones is refused. The pattern allows
// no dot or space, so a trailing "." or " " cannot occur.
var reservedSlotRe = regexp.MustCompile(`^(con|prn|aux|nul|com[1-9]|lpt[1-9])$`)

// ValidNewSlotName is ValidSlotName plus the rule that a new slot must not use a Windows reserved device name.
func ValidNewSlotName(s string) bool { return ValidSlotName(s) && !reservedSlotRe.MatchString(s) }

// SaveCheckpoint is the current checkpoint of a slot ("Rev N").
type SaveCheckpoint struct {
	Revision   int
	SHA256     string
	Size       int64
	DeviceID   string
	DeviceName string
	CreatedAt  time.Time
	Reason     string // sync reason
	// CheatsUsed: the app reported cheats or memory writes while this checkpoint was made (since run_rules_v1).
	CheatsUsed bool
}

// SaveVersion is a permanent history version ("vN").
type SaveVersion struct {
	Version      int
	Revision     int
	SHA256       string
	Size         int64
	DeviceID     string
	DeviceName   string
	CreatedAt    time.Time
	Reason       string // history reason
	BaseRevision *int   // only conflict_upload
	Label        *string
	CheatsUsed   bool
}

// SaveConflict is a recorded save conflict. For open conflicts server shows the live checkpoint,
// for resolved ones the state at resolution time.
type SaveConflict struct {
	ID         string
	UserID     string
	GameID     string
	Slot       string
	Status     string
	Hub        SaveConflictHub
	Secured    SaveConflictSecured
	CreatedAt  time.Time
	ResolvedAt *time.Time
	ResolvedBy string // "device:<id>" / "user:<id>"; empty while open
}

// SaveConflictHub is the server side of a conflict.
type SaveConflictHub struct {
	Revision   int
	SHA256     string
	DeviceID   string
	DeviceName string
	CreatedAt  time.Time
}

// SaveConflictSecured is the secured upload (a history version).
type SaveConflictSecured struct {
	Version      int
	SHA256       string
	BaseRevision int
	DeviceID     string
	DeviceName   string
	CreatedAt    time.Time
}

// SaveSlot is a slot with its current checkpoint and open conflicts.
type SaveSlot struct {
	UserID        string
	GameID        string
	Slot          string
	Current       SaveCheckpoint
	OpenConflicts []SaveConflict
}

// SaveSummary is a list row (API list and app page).
type SaveSummary struct {
	UserID            string
	Username          string
	GameID            string
	GameTitle         string
	Slot              string
	Current           SaveCheckpoint
	OpenConflictCount int
}

type dbq interface {
	QueryContext(context.Context, string, ...any) (*sql.Rows, error)
	QueryRowContext(context.Context, string, ...any) *sql.Row
	ExecContext(context.Context, string, ...any) (sql.Result, error)
}

func (s *Service) saveDir(userID, gameID, slot string) string {
	return filepath.Join(s.dataDir, "saves", userID, gameID, slot)
}

func (s *Service) saveFile(userID, gameID, slot, sha string) string {
	return filepath.Join(s.saveDir(userID, gameID, slot), sha)
}

var checkpointSQL = `SELECT s.revision, s.sha256, s.size, s.device_id, ` + deviceNameSQL("d", "s.device_id") + `, s.created_at, s.reason, s.cheats_used
	FROM save_slots s LEFT JOIN devices d ON d.id = s.device_id
	WHERE s.user_id = ? AND s.game_id = ? AND s.slot = ?`

func scanCheckpoint(r scanner) (SaveCheckpoint, error) {
	var c SaveCheckpoint
	var created int64
	var cheats int
	if err := r.Scan(&c.Revision, &c.SHA256, &c.Size, &c.DeviceID, &c.DeviceName, &created, &c.Reason, &cheats); err != nil {
		return SaveCheckpoint{}, err
	}
	c.CheatsUsed = cheats != 0
	c.CreatedAt = time.Unix(created, 0).UTC()
	return c, nil
}

func loadCheckpoint(ctx context.Context, q dbq, userID, gameID, slot string) (SaveCheckpoint, error) {
	c, err := scanCheckpoint(q.QueryRowContext(ctx, checkpointSQL, userID, gameID, slot))
	if errors.Is(err, sql.ErrNoRows) {
		return SaveCheckpoint{}, ErrNotFound
	}
	if err != nil {
		return SaveCheckpoint{}, internal(err)
	}
	return c, nil
}

var versionCols = `h.version, h.revision, h.sha256, h.size, h.device_id, ` + deviceNameSQL("d", "h.device_id") + `, h.created_at, h.reason, h.base_revision, h.label, h.cheats_used`

func scanVersion(r scanner) (SaveVersion, error) {
	var v SaveVersion
	var created int64
	var base sql.NullInt64
	var label sql.NullString
	var cheats int
	if err := r.Scan(&v.Version, &v.Revision, &v.SHA256, &v.Size, &v.DeviceID, &v.DeviceName, &created, &v.Reason, &base, &label, &cheats); err != nil {
		return SaveVersion{}, err
	}
	v.CheatsUsed = cheats != 0
	if label.Valid {
		v.Label = &label.String
	}
	v.CreatedAt = time.Unix(created, 0).UTC()
	if base.Valid {
		b := int(base.Int64)
		v.BaseRevision = &b
	}
	return v, nil
}

var conflictSQL = `SELECT c.id, c.user_id, c.game_id, c.slot, c.status,
	c.hub_revision, c.hub_sha256, c.hub_device_id, ` + deviceNameSQL("hd", "c.hub_device_id") + `, c.hub_created_at,
	c.secured_version, h.sha256, COALESCE(h.base_revision, 0), c.device_id, ` + deviceNameSQL("sd", "c.device_id") + `, h.created_at,
	c.created_at, c.resolved_at, c.resolved_by
	FROM save_conflicts c
	JOIN save_history h ON h.user_id = c.user_id AND h.game_id = c.game_id AND h.slot = c.slot AND h.version = c.secured_version
	LEFT JOIN devices hd ON hd.id = c.hub_device_id
	LEFT JOIN devices sd ON sd.id = c.device_id `

func scanConflict(r scanner) (SaveConflict, error) {
	var c SaveConflict
	var hubAt, secAt, created int64
	var resolved sql.NullInt64
	var by sql.NullString
	if err := r.Scan(&c.ID, &c.UserID, &c.GameID, &c.Slot, &c.Status,
		&c.Hub.Revision, &c.Hub.SHA256, &c.Hub.DeviceID, &c.Hub.DeviceName, &hubAt,
		&c.Secured.Version, &c.Secured.SHA256, &c.Secured.BaseRevision, &c.Secured.DeviceID, &c.Secured.DeviceName, &secAt,
		&created, &resolved, &by); err != nil {
		return SaveConflict{}, err
	}
	c.Hub.CreatedAt = time.Unix(hubAt, 0).UTC()
	c.Secured.CreatedAt = time.Unix(secAt, 0).UTC()
	c.CreatedAt = time.Unix(created, 0).UTC()
	c.ResolvedAt = ts(resolved)
	c.ResolvedBy = by.String
	return c, nil
}

// withLiveHub replaces the server side of an open conflict with the current checkpoint.
func withLiveHub(c SaveConflict, cur SaveCheckpoint) SaveConflict {
	if c.Status == ConflictOpen {
		c.Hub = SaveConflictHub{Revision: cur.Revision, SHA256: cur.SHA256, DeviceID: cur.DeviceID,
			DeviceName: cur.DeviceName, CreatedAt: cur.CreatedAt}
	}
	return c
}

func loadSlot(ctx context.Context, q dbq, userID, gameID, slot string) (SaveSlot, error) {
	cur, err := loadCheckpoint(ctx, q, userID, gameID, slot)
	if err != nil {
		return SaveSlot{}, err
	}
	rows, err := q.QueryContext(ctx, conflictSQL+`WHERE c.user_id = ? AND c.game_id = ? AND c.slot = ? AND c.status = 'open'
		ORDER BY c.created_at, c.id`, userID, gameID, slot)
	if err != nil {
		return SaveSlot{}, internal(err)
	}
	defer rows.Close()
	out := SaveSlot{UserID: userID, GameID: gameID, Slot: slot, Current: cur, OpenConflicts: []SaveConflict{}}
	for rows.Next() {
		c, err := scanConflict(rows)
		if err != nil {
			return SaveSlot{}, internal(err)
		}
		out.OpenConflicts = append(out.OpenConflicts, withLiveHub(c, cur))
	}
	return out, rows.Err()
}

// captured reports whether the checkpoint revision already has a history version (conflict uploads do not count).
func captured(ctx context.Context, q dbq, userID, gameID, slot string, revision int) (bool, error) {
	var n int
	err := q.QueryRowContext(ctx, `SELECT COUNT(*) FROM save_history WHERE user_id = ? AND game_id = ? AND slot = ?
		AND revision = ? AND reason != 'conflict_upload'`, userID, gameID, slot, revision).Scan(&n)
	return n > 0, err
}

type historyEntry struct {
	revision     int
	sha          string
	size         int64
	deviceID     string
	syncReason   string
	reason       string
	baseRevision *int
	createdAt    int64
	label        *string
	cheatsUsed   bool
}

func addHistory(ctx context.Context, q dbq, userID, gameID, slot string, e historyEntry) (int, error) {
	var v int
	if err := q.QueryRowContext(ctx, `SELECT MAX(COALESCE((SELECT MAX(version) FROM save_history WHERE user_id = ?1 AND game_id = ?2 AND slot = ?3), 0),
		COALESCE((SELECT history_high FROM save_slots WHERE user_id = ?1 AND game_id = ?2 AND slot = ?3), 0)) + 1`,
		userID, gameID, slot).Scan(&v); err != nil {
		return 0, err
	}
	var base any
	if e.baseRevision != nil {
		base = *e.baseRevision
	}
	var label any
	if e.label != nil {
		label = *e.label
	}
	_, err := q.ExecContext(ctx, `INSERT INTO save_history(user_id, game_id, slot, version, revision, sha256, size, device_id,
		sync_reason, reason, base_revision, created_at, label, cheats_used) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)`,
		userID, gameID, slot, v, e.revision, e.sha, e.size, e.deviceID, e.syncReason, e.reason, base, e.createdAt, label, b2i(e.cheatsUsed))
	return v, err
}

// captureCheckpoint copies the current checkpoint into history unless a version of it exists already.
func captureCheckpoint(ctx context.Context, q dbq, userID, gameID, slot string, cur SaveCheckpoint, reason string) error {
	done, err := captured(ctx, q, userID, gameID, slot, cur.Revision)
	if err != nil || done {
		return err
	}
	_, err = addHistory(ctx, q, userID, gameID, slot, historyEntry{revision: cur.Revision, sha: cur.SHA256, size: cur.Size,
		deviceID: cur.DeviceID, syncReason: cur.Reason, reason: reason, createdAt: cur.CreatedAt.Unix(), cheatsUsed: cur.CheatsUsed})
	return err
}

// stageUpload streams r into a temp file (size limit while streaming, SHA-256 while writing, fsync).
// The caller removes the temp file.
func (s *Service) stageUpload(r io.Reader) (tmpName, sha string, size int64, err error) {
	tmp, err := os.CreateTemp(filepath.Join(s.dataDir, "tmp"), "save-*")
	if err != nil {
		return "", "", 0, internal(err)
	}
	tmpName = tmp.Name()
	h := sha256.New()
	size, err = io.Copy(io.MultiWriter(tmp, h), io.LimitReader(r, MaxSaveBytes+1))
	if err == nil && size > MaxSaveBytes {
		err = ErrPayloadTooLarge
	}
	if err == nil {
		err = tmp.Sync()
	}
	if cerr := tmp.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		var mbe *http.MaxBytesError
		switch {
		case errors.Is(err, ErrPayloadTooLarge), errors.As(err, &mbe):
			return tmpName, "", 0, ErrPayloadTooLarge
		}
		return tmpName, "", 0, internal(err)
	}
	return tmpName, hex.EncodeToString(h.Sum(nil)), size, nil
}

// placeContent moves the staged file to its content-addressed place. placed is false if the content already exists.
func placeContent(tmpName, dst string) (placed bool, err error) {
	if _, err := os.Stat(dst); err == nil {
		return false, nil
	}
	if err := os.MkdirAll(filepath.Dir(dst), 0o750); err != nil {
		return false, err
	}
	if err := os.Chmod(tmpName, 0o640); err != nil {
		return false, err
	}
	if err := os.Rename(tmpName, dst); err != nil {
		return false, err
	}
	syncDir(filepath.Dir(dst))
	return true, nil
}

// syncDir persists a directory entry (best effort).
func syncDir(dir string) {
	if d, err := os.Open(dir); err == nil {
		d.Sync()
		d.Close()
	}
}

// PutSaveInput is an upload (D3). UserID is the owner of the authenticated device.
type PutSaveInput struct {
	UserID, DeviceID, GameID, Slot string
	BaseRevision                   int
	SHA256                         string // claimed hash, verified
	Reason                         string // sync reason
	CheatsUsed                     bool   // optional marker: cheats or memory writes were active (run_rules_v1)
	Body                           io.Reader
}

// PutSaveResult: Conflict != nil means the upload was secured as a conflict (the slot is unchanged).
type PutSaveResult struct {
	Slot     SaveSlot
	Conflict *SaveConflict
}

func newConflictID() string { return "c_" + strings.ReplaceAll(uuid.NewString(), "-", "")[:16] }

// PutSave implements the upload rules of D3 in one transaction per slot.
func (s *Service) PutSave(ctx context.Context, in PutSaveInput) (_ PutSaveResult, err error) {
	defer s.publishOK(&err, TopicSaves, TopicLibrary)
	switch in.Reason {
	case SyncCheckpoint, SyncFinal, SyncFinalSessionEnd:
	default:
		return PutSaveResult{}, badRequest("Invalid sync reason")
	}
	if !ValidSlotName(in.Slot) {
		return PutSaveResult{}, badRequest("Invalid slot name")
	}
	if !ValidSHA256(in.SHA256) {
		return PutSaveResult{}, badRequest("Invalid content hash")
	}
	if in.BaseRevision < 0 {
		return PutSaveResult{}, badRequest("Invalid base revision")
	}
	if _, err := s.GetGame(ctx, in.GameID); err != nil {
		return PutSaveResult{}, err
	}
	tmpName, sha, size, err := s.stageUpload(in.Body)
	defer os.Remove(tmpName) // no effect after a successful rename
	if err != nil {
		return PutSaveResult{}, err
	}
	if sha != in.SHA256 {
		return PutSaveResult{}, badRequest("Content hash does not match the body")
	}

	s.saveMu.Lock()
	defer s.saveMu.Unlock()
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return PutSaveResult{}, internal(err)
	}
	defer tx.Rollback()
	dst := s.saveFile(in.UserID, in.GameID, in.Slot, sha)
	committed, placed := false, false
	defer func() {
		if !committed && placed {
			os.Remove(dst)
		}
	}()
	u, g, sl := in.UserID, in.GameID, in.Slot
	now := s.now().Unix()

	cur, err := loadCheckpoint(ctx, tx, u, g, sl)
	exists := err == nil
	if err != nil && !errors.Is(err, ErrNotFound) {
		return PutSaveResult{}, err
	}
	if !exists && !ValidNewSlotName(sl) {
		return PutSaveResult{}, badRequest("Invalid slot name")
	}
	res := PutSaveResult{}
	var dropSHA string // content of the replaced checkpoint, deleted after commit unless referenced
	changed := false   // the checkpoint changed (save_updated push)

	switch {
	case exists && cur.SHA256 == sha: // 1. idempotent retry
		if in.Reason == SyncFinalSessionEnd {
			if err := captureCheckpoint(ctx, tx, u, g, sl, cur, HistorySessionEnd); err != nil {
				return PutSaveResult{}, internal(err)
			}
		}
	case (!exists && in.BaseRevision == 0) || (exists && in.BaseRevision == cur.Revision): // 2. accept
		if placed, err = placeContent(tmpName, dst); err != nil {
			return PutSaveResult{}, internal(err)
		}
		changed = true
		rev := 1
		if exists {
			rev = cur.Revision + 1
			if cur.DeviceID != in.DeviceID {
				if err := captureCheckpoint(ctx, tx, u, g, sl, cur, HistoryDeviceChange); err != nil {
					return PutSaveResult{}, internal(err)
				}
			}
			dropSHA = cur.SHA256
		}
		if _, err := tx.ExecContext(ctx, `INSERT INTO save_slots(user_id, game_id, slot, revision, sha256, size, device_id, reason, created_at, cheats_used)
			VALUES (?,?,?,?,?,?,?,?,?,?) ON CONFLICT(user_id, game_id, slot) DO UPDATE SET revision = excluded.revision,
			sha256 = excluded.sha256, size = excluded.size, device_id = excluded.device_id, reason = excluded.reason,
			created_at = excluded.created_at, cheats_used = excluded.cheats_used`, u, g, sl, rev, sha, size, in.DeviceID, in.Reason, now, b2i(in.CheatsUsed)); err != nil {
			return PutSaveResult{}, internal(err)
		}
		if in.Reason == SyncFinalSessionEnd {
			if _, err := addHistory(ctx, tx, u, g, sl, historyEntry{revision: rev, sha: sha, size: size, deviceID: in.DeviceID,
				syncReason: in.Reason, reason: HistorySessionEnd, createdAt: now, cheatsUsed: in.CheatsUsed}); err != nil {
				return PutSaveResult{}, internal(err)
			}
		}
	case !exists:
		return PutSaveResult{}, badRequest("Base revision does not match: the slot does not exist yet (use 0)")
	default: // 3. stale base: secure the upload as a conflict
		c, err := s.secureConflict(ctx, tx, in, cur, tmpName, dst, sha, size, now, &placed)
		if err != nil {
			return PutSaveResult{}, err
		}
		res.Conflict = &c
	}
	if res.Slot, err = loadSlot(ctx, tx, u, g, sl); err != nil {
		return PutSaveResult{}, err
	}
	if err := tx.Commit(); err != nil {
		return PutSaveResult{}, internal(err)
	}
	committed = true
	if dropSHA != "" {
		s.dropIfUnreferenced(ctx, u, g, sl, dropSHA)
	}
	s.thinSlot(ctx, u, g, sl)
	if changed {
		s.notifySaveUpdated(u, in.DeviceID, g, sl, res.Slot.Current, "")
	}
	return res, nil
}

func (s *Service) secureConflict(ctx context.Context, tx dbq, in PutSaveInput, cur SaveCheckpoint, tmpName, dst, sha string, size, now int64, placed *bool) (SaveConflict, error) {
	u, g, sl := in.UserID, in.GameID, in.Slot
	// Existing open conflict of this slot+device?
	var id string
	var securedSHA string
	err := tx.QueryRowContext(ctx, `SELECT c.id, h.sha256 FROM save_conflicts c
		JOIN save_history h ON h.user_id = c.user_id AND h.game_id = c.game_id AND h.slot = c.slot AND h.version = c.secured_version
		WHERE c.user_id = ? AND c.game_id = ? AND c.slot = ? AND c.device_id = ? AND c.status = 'open'`,
		u, g, sl, in.DeviceID).Scan(&id, &securedSHA)
	if err != nil && !errors.Is(err, sql.ErrNoRows) {
		return SaveConflict{}, internal(err)
	}
	have := err == nil
	if !have || securedSHA != sha { // a retry of the same secured upload adds nothing
		var perr error
		if *placed, perr = placeContent(tmpName, dst); perr != nil {
			return SaveConflict{}, internal(perr)
		}
		base := in.BaseRevision
		ver, err := addHistory(ctx, tx, u, g, sl, historyEntry{revision: cur.Revision, sha: sha, size: size, deviceID: in.DeviceID,
			syncReason: in.Reason, reason: HistoryConflictUpload, baseRevision: &base, createdAt: now, cheatsUsed: in.CheatsUsed})
		if err != nil {
			return SaveConflict{}, internal(err)
		}
		if have {
			_, err = tx.ExecContext(ctx, `UPDATE save_conflicts SET secured_version = ?, hub_revision = ?, hub_sha256 = ?,
				hub_device_id = ?, hub_created_at = ? WHERE id = ?`, ver, cur.Revision, cur.SHA256, cur.DeviceID, cur.CreatedAt.Unix(), id)
		} else {
			id = newConflictID()
			_, err = tx.ExecContext(ctx, `INSERT INTO save_conflicts(id, user_id, game_id, slot, status, device_id, secured_version,
				hub_revision, hub_sha256, hub_device_id, hub_created_at, created_at) VALUES (?,?,?,?,'open',?,?,?,?,?,?,?)`,
				id, u, g, sl, in.DeviceID, ver, cur.Revision, cur.SHA256, cur.DeviceID, cur.CreatedAt.Unix(), now)
		}
		if err != nil {
			return SaveConflict{}, internal(err)
		}
	}
	c, err := scanConflict(tx.QueryRowContext(ctx, conflictSQL+`WHERE c.id = ?`, id))
	if err != nil {
		return SaveConflict{}, internal(err)
	}
	return withLiveHub(c, cur), nil
}

// dropIfUnreferenced deletes a content file that neither the current checkpoint nor any history version uses.
// The caller holds saveMu. Errors are ignored: a leftover file is harmless.
func (s *Service) dropIfUnreferenced(ctx context.Context, userID, gameID, slot, sha string) {
	var n int
	err := s.db.QueryRowContext(ctx, `SELECT
		(SELECT COUNT(*) FROM save_slots WHERE user_id = ?1 AND game_id = ?2 AND slot = ?3 AND sha256 = ?4) +
		(SELECT COUNT(*) FROM save_history WHERE user_id = ?1 AND game_id = ?2 AND slot = ?3 AND sha256 = ?4)`,
		userID, gameID, slot, sha).Scan(&n)
	if err == nil && n == 0 {
		os.Remove(s.saveFile(userID, gameID, slot, sha))
	}
}

// ListSaveSlots lists slots; userID "" = all users (admin app page). Conflicts first, then newest checkpoint.
func (s *Service) ListSaveSlots(ctx context.Context, userID string) ([]SaveSummary, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT s.user_id, u.username, s.game_id, COALESCE(g.title, '(removed game)'), s.slot,
		s.revision, s.sha256, s.size, s.device_id, `+deviceNameSQL("d", "s.device_id")+`, s.created_at, s.reason, s.cheats_used,
		(SELECT COUNT(*) FROM save_conflicts c WHERE c.user_id = s.user_id AND c.game_id = s.game_id AND c.slot = s.slot AND c.status = 'open') AS oc
		FROM save_slots s JOIN users u ON u.id = s.user_id
		LEFT JOIN games g ON g.id = s.game_id LEFT JOIN devices d ON d.id = s.device_id
		WHERE (?1 = '' OR s.user_id = ?1)
		ORDER BY (oc > 0) DESC, s.created_at DESC, g.title COLLATE NOCASE`, userID)
	if err != nil {
		return nil, internal(err)
	}
	defer rows.Close()
	out := []SaveSummary{}
	for rows.Next() {
		var r SaveSummary
		var created int64
		var cheats int
		c := &r.Current
		if err := rows.Scan(&r.UserID, &r.Username, &r.GameID, &r.GameTitle, &r.Slot, &c.Revision, &c.SHA256, &c.Size,
			&c.DeviceID, &c.DeviceName, &created, &c.Reason, &cheats, &r.OpenConflictCount); err != nil {
			return nil, internal(err)
		}
		c.CheatsUsed = cheats != 0
		c.CreatedAt = time.Unix(created, 0).UTC()
		out = append(out, r)
	}
	return out, rows.Err()
}

// OpenConflictCount returns the number of open conflicts across all users (nav badge).
func (s *Service) OpenConflictCount(ctx context.Context) (int, error) {
	var n int
	if err := s.db.QueryRowContext(ctx, `SELECT COUNT(*) FROM save_conflicts WHERE status = 'open'`).Scan(&n); err != nil {
		return 0, internal(err)
	}
	return n, nil
}

// GetSaveSlot returns a slot of the user; ErrNotFound if it does not exist (also for other users' slots).
func (s *Service) GetSaveSlot(ctx context.Context, userID, gameID, slot string) (SaveSlot, error) {
	if !ValidSlotName(slot) {
		return SaveSlot{}, ErrNotFound
	}
	return loadSlot(ctx, s.db, userID, gameID, slot)
}

// ListSaveHistory returns the history versions of a slot (newest first).
func (s *Service) ListSaveHistory(ctx context.Context, userID, gameID, slot string) ([]SaveVersion, error) {
	if _, err := s.GetSaveSlot(ctx, userID, gameID, slot); err != nil {
		return nil, err
	}
	rows, err := s.db.QueryContext(ctx, `SELECT `+versionCols+` FROM save_history h LEFT JOIN devices d ON d.id = h.device_id
		WHERE h.user_id = ? AND h.game_id = ? AND h.slot = ? ORDER BY h.version DESC`, userID, gameID, slot)
	if err != nil {
		return nil, internal(err)
	}
	defer rows.Close()
	out := []SaveVersion{}
	for rows.Next() {
		v, err := scanVersion(rows)
		if err != nil {
			return nil, internal(err)
		}
		out = append(out, v)
	}
	return out, rows.Err()
}

func (s *Service) openContent(userID, gameID, slot, sha string) (*os.File, error) {
	f, err := os.Open(s.saveFile(userID, gameID, slot, sha))
	if err != nil {
		return nil, internal(fmt.Errorf("save content missing: %w", err))
	}
	return f, nil
}

// OpenSaveContent opens the content of the current checkpoint; the caller closes it.
func (s *Service) OpenSaveContent(ctx context.Context, userID, gameID, slot string) (*os.File, SaveCheckpoint, error) {
	if !ValidSlotName(slot) {
		return nil, SaveCheckpoint{}, ErrNotFound
	}
	cur, err := loadCheckpoint(ctx, s.db, userID, gameID, slot)
	if err != nil {
		return nil, SaveCheckpoint{}, err
	}
	f, err := s.openContent(userID, gameID, slot, cur.SHA256)
	return f, cur, err
}

// OpenSaveVersion opens the content of a history version; the caller closes it.
func (s *Service) OpenSaveVersion(ctx context.Context, userID, gameID, slot string, version int) (*os.File, SaveVersion, error) {
	if !ValidSlotName(slot) {
		return nil, SaveVersion{}, ErrNotFound
	}
	v, err := scanVersion(s.db.QueryRowContext(ctx, `SELECT `+versionCols+` FROM save_history h LEFT JOIN devices d ON d.id = h.device_id
		WHERE h.user_id = ? AND h.game_id = ? AND h.slot = ? AND h.version = ?`, userID, gameID, slot, version))
	if errors.Is(err, sql.ErrNoRows) {
		return nil, SaveVersion{}, ErrNotFound
	}
	if err != nil {
		return nil, SaveVersion{}, internal(err)
	}
	f, err := s.openContent(userID, gameID, slot, v.SHA256)
	return f, v, err
}

// ResolveInput resolves an open conflict (D3). ResolvedBy is "device:<id>" or "user:<id>".
type ResolveInput struct {
	UserID, GameID, Slot, ConflictID string
	Resolution                       string
	ExpectedRevision                 int
	ResolvedBy                       string
}

// ResolveSaveConflict resolves a conflict. A wrong expected_revision or an already resolved conflict
// returns ErrSaveConflictStale and changes nothing. Before applying, the current checkpoint goes to history.
func (s *Service) ResolveSaveConflict(ctx context.Context, in ResolveInput) (_ SaveSlot, err error) {
	defer s.publishOK(&err, TopicSaves, TopicLibrary)
	if in.Resolution != ResolveUseHub && in.Resolution != ResolveUseLocal {
		return SaveSlot{}, badRequest("Invalid resolution")
	}
	if !ValidSlotName(in.Slot) {
		return SaveSlot{}, ErrNotFound
	}
	if in.ExpectedRevision < 1 {
		return SaveSlot{}, badRequest("Invalid expected revision")
	}
	s.saveMu.Lock()
	defer s.saveMu.Unlock()
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return SaveSlot{}, internal(err)
	}
	defer tx.Rollback()
	u, g, sl := in.UserID, in.GameID, in.Slot
	cur, err := loadCheckpoint(ctx, tx, u, g, sl)
	if err != nil {
		return SaveSlot{}, err
	}
	c, err := scanConflict(tx.QueryRowContext(ctx, conflictSQL+`WHERE c.id = ? AND c.user_id = ? AND c.game_id = ? AND c.slot = ?`,
		in.ConflictID, u, g, sl))
	if errors.Is(err, sql.ErrNoRows) {
		return SaveSlot{}, ErrNotFound
	}
	if err != nil {
		return SaveSlot{}, internal(err)
	}
	if c.Status != ConflictOpen || in.ExpectedRevision != cur.Revision {
		return SaveSlot{}, ErrSaveConflictStale
	}
	if err := captureCheckpoint(ctx, tx, u, g, sl, cur, HistoryBeforeResolution); err != nil {
		return SaveSlot{}, internal(err)
	}
	status := ConflictResolvedHub
	if in.Resolution == ResolveUseLocal {
		status = ConflictResolvedLocal
		var size int64
		var reason string
		var cheats int
		if err := tx.QueryRowContext(ctx, `SELECT size, sync_reason, cheats_used FROM save_history WHERE user_id = ? AND game_id = ? AND slot = ? AND version = ?`,
			u, g, sl, c.Secured.Version).Scan(&size, &reason, &cheats); err != nil {
			return SaveSlot{}, internal(err)
		}
		if _, err := tx.ExecContext(ctx, `UPDATE save_slots SET revision = ?, sha256 = ?, size = ?, device_id = ?, reason = ?, created_at = ?, cheats_used = ?
			WHERE user_id = ? AND game_id = ? AND slot = ?`, cur.Revision+1, c.Secured.SHA256, size, c.Secured.DeviceID, reason,
			s.now().Unix(), cheats, u, g, sl); err != nil {
			return SaveSlot{}, internal(err)
		}
	}
	if _, err := tx.ExecContext(ctx, `UPDATE save_conflicts SET status = ?, resolved_at = ?, resolved_by = ?, hub_revision = ?, hub_sha256 = ?,
		hub_device_id = ?, hub_created_at = ? WHERE id = ?`, status, s.now().Unix(), in.ResolvedBy, cur.Revision, cur.SHA256,
		cur.DeviceID, cur.CreatedAt.Unix(), c.ID); err != nil {
		return SaveSlot{}, internal(err)
	}
	out, err := loadSlot(ctx, tx, u, g, sl)
	if err != nil {
		return SaveSlot{}, err
	}
	if err := tx.Commit(); err != nil {
		return SaveSlot{}, internal(err)
	}
	s.thinSlot(ctx, u, g, sl)
	if in.Resolution == ResolveUseLocal {
		origin, _ := strings.CutPrefix(in.ResolvedBy, "device:")
		if origin == in.ResolvedBy {
			origin = "" // resolved in the app: all devices of the user are told
		}
		s.notifySaveUpdated(u, origin, g, sl, out.Current, PushConflictResolution)
	}
	return out, nil
}

// FeatureSavesV1 is the handshake feature flag for the save sync API.
const FeatureSavesV1 = "saves_v1"

// FeatureSavesV3 is the handshake feature flag for deleting manual snapshots.
const FeatureSavesV3 = "saves_v3"

// FeatureSavesV4 is the handshake feature flag for uploading a save file into a slot.
const FeatureSavesV4 = "saves_v4"

// FeatureSavesV5 is the handshake feature flag for renaming and deleting slots and the upload lock during a Session.
const FeatureSavesV5 = "saves_v5"

// FeatureSavesV6 is the handshake feature flag for deleting any history version and the main slot.
const FeatureSavesV6 = "saves_v6"

// DefaultSlot is the slot every app uses unless the user picks another one. It cannot be renamed (it can be deleted since saves_v6).
const DefaultSlot = "default"

// FeatureSavesV2 is the handshake feature flag for restore, snapshots, history labels and the save_updated push.
const FeatureSavesV2 = "saves_v2"

// GetSaveConflict returns one conflict of a slot (open or resolved).
func (s *Service) GetSaveConflict(ctx context.Context, userID, gameID, slot, id string) (SaveConflict, error) {
	c, err := scanConflict(s.db.QueryRowContext(ctx, conflictSQL+`WHERE c.id = ? AND c.user_id = ? AND c.game_id = ? AND c.slot = ?`, id, userID, gameID, slot))
	if errors.Is(err, sql.ErrNoRows) {
		return SaveConflict{}, ErrNotFound
	}
	if err != nil {
		return SaveConflict{}, internal(err)
	}
	if c.Status == ConflictOpen {
		cur, err := loadCheckpoint(ctx, s.db, userID, gameID, slot)
		if err != nil {
			return SaveConflict{}, err
		}
		c = withLiveHub(c, cur)
	}
	return c, nil
}

func b2i(b bool) int {
	if b {
		return 1
	}
	return 0
}
