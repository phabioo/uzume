package hub

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/google/uuid"
)

// Save comfort (ADR 0012 D7): restore, manual snapshots, retention and the save_updated push.

// saveRetention holds the retention rules (0 = unlimited for that rule; recent 0 keeps every version).
type saveRetention struct {
	recent, daily, weekly int
}

// RestoreInput restores a history version as the new checkpoint. DeviceID is the caller's device, or
// AdminDeviceID(user) for the app.
type RestoreInput struct {
	UserID, DeviceID, GameID, Slot string
	Version, ExpectedRevision      int
}

// RestoreSaveVersion makes the content of a history version the new checkpoint (revision + 1, reason restore,
// device = caller). The current checkpoint goes to history first (before_restore) unless a version of it exists.
// A different expected revision returns ErrSaveConflictStale and changes nothing.
func (s *Service) RestoreSaveVersion(ctx context.Context, in RestoreInput) (_ SaveSlot, err error) {
	defer s.publishOK(&err, TopicSaves)
	if !ValidSlotName(in.Slot) || in.Version < 1 {
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
	if err := s.checkNoSession(ctx, tx, u, g); err != nil { // a Session may have started while the body was staged
		return SaveSlot{}, err
	}
	cur, err := loadCheckpoint(ctx, tx, u, g, sl)
	if err != nil {
		return SaveSlot{}, err
	}
	var sha string
	var size int64
	var cheats int
	err = tx.QueryRowContext(ctx, `SELECT sha256, size, cheats_used FROM save_history WHERE user_id = ? AND game_id = ? AND slot = ? AND version = ?`,
		u, g, sl, in.Version).Scan(&sha, &size, &cheats)
	if errors.Is(err, sql.ErrNoRows) {
		return SaveSlot{}, ErrNotFound
	}
	if err != nil {
		return SaveSlot{}, internal(err)
	}
	if in.ExpectedRevision != cur.Revision {
		return SaveSlot{}, ErrSaveConflictStale
	}
	if _, err := os.Stat(s.saveFile(u, g, sl, sha)); err != nil {
		return SaveSlot{}, internal(err)
	}
	if err := captureCheckpoint(ctx, tx, u, g, sl, cur, HistoryBeforeRestore); err != nil {
		return SaveSlot{}, internal(err)
	}
	if _, err := tx.ExecContext(ctx, `UPDATE save_slots SET revision = ?, sha256 = ?, size = ?, device_id = ?, reason = ?, created_at = ?, cheats_used = ?
		WHERE user_id = ? AND game_id = ? AND slot = ?`, cur.Revision+1, sha, size, in.DeviceID, SyncRestore, s.now().Unix(), cheats, u, g, sl); err != nil {
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
	s.notifySaveUpdated(u, in.DeviceID, g, sl, out.Current, "")
	return out, nil
}

// pathPartRe matches a user ID (u_<hex>) or game ID (UUID): no separators, no dots.
var pathPartRe = regexp.MustCompile(`^[A-Za-z0-9_-]{1,64}$`)

// UploadSaveInput is a deliberate save file upload (feature saves_v4). DeviceID is the caller's device, or
// AdminDeviceID(user) for the app. ExpectedRevision nil = no check (web only); otherwise 0 = the slot must
// not exist yet, N = the current checkpoint revision must be N.
type UploadSaveInput struct {
	UserID, DeviceID, GameID, Slot string
	ExpectedRevision               *int
	SHA256                         string // claimed hash, verified; "" = not checked (web)
	Body                           io.Reader
}

// UploadSaveFile makes the body the new checkpoint of a slot (revision + 1, reason upload, device = caller), or
// creates the slot at revision 1. An existing checkpoint first goes to history (before_upload) unless a version of it
// exists. Identical content changes nothing. A wrong expected revision returns ErrSaveConflictStale.
func (s *Service) UploadSaveFile(ctx context.Context, in UploadSaveInput) (_ SaveSlot, err error) {
	defer s.publishOK(&err, TopicSaves, TopicLibrary)
	// User and game IDs become path segments of the content file: only plain ID characters pass.
	if !pathPartRe.MatchString(in.UserID) || !pathPartRe.MatchString(in.GameID) {
		return SaveSlot{}, ErrNotFound
	}
	if !ValidSlotName(in.Slot) {
		return SaveSlot{}, badRequest("Invalid slot name")
	}
	if in.SHA256 != "" && !ValidSHA256(in.SHA256) {
		return SaveSlot{}, badRequest("Invalid content hash")
	}
	if in.ExpectedRevision != nil && *in.ExpectedRevision < 0 {
		return SaveSlot{}, badRequest("Invalid expected revision")
	}
	if _, err := s.GetGame(ctx, in.GameID); err != nil {
		return SaveSlot{}, err
	}
	if err := s.checkNoSession(ctx, s.db, in.UserID, in.GameID); err != nil {
		return SaveSlot{}, err
	}
	tmpName, sha, size, err := s.stageUpload(in.Body)
	defer os.Remove(tmpName) // no effect after a successful rename
	if err != nil {
		return SaveSlot{}, err
	}
	if size == 0 {
		return SaveSlot{}, badRequest("Save file is empty")
	}
	if in.SHA256 != "" && sha != in.SHA256 {
		return SaveSlot{}, badRequest("Content hash does not match the body")
	}

	s.saveMu.Lock()
	defer s.saveMu.Unlock()
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return SaveSlot{}, internal(err)
	}
	defer tx.Rollback()
	u, g, sl := in.UserID, in.GameID, in.Slot
	// A Session may have started while the body was staged (the upload lock applies during a Session).
	if err := s.checkNoSession(ctx, tx, u, g); err != nil {
		return SaveSlot{}, err
	}
	cur, err := loadCheckpoint(ctx, tx, u, g, sl)
	exists := err == nil
	if err != nil && !errors.Is(err, ErrNotFound) {
		return SaveSlot{}, err
	}
	if !exists && !ValidNewSlotName(sl) {
		return SaveSlot{}, badRequest("Invalid slot name")
	}
	if exists && cur.SHA256 == sha { // idempotent
		out, err := loadSlot(ctx, tx, u, g, sl)
		if err != nil {
			return SaveSlot{}, err
		}
		return out, nil
	}
	if want := in.ExpectedRevision; want != nil && (exists && *want != cur.Revision || !exists && *want != 0) {
		return SaveSlot{}, ErrSaveConflictStale
	}
	dst := s.saveFile(u, g, sl, sha)
	placed, err := placeContent(tmpName, dst)
	if err != nil {
		return SaveSlot{}, internal(err)
	}
	committed := false
	defer func() {
		if !committed && placed {
			os.Remove(dst)
		}
	}()
	rev := 1
	if exists {
		rev = cur.Revision + 1
		if err := captureCheckpoint(ctx, tx, u, g, sl, cur, HistoryBeforeUpload); err != nil {
			return SaveSlot{}, internal(err)
		}
	}
	if _, err := tx.ExecContext(ctx, `INSERT INTO save_slots(user_id, game_id, slot, revision, sha256, size, device_id, reason, created_at)
		VALUES (?,?,?,?,?,?,?,?,?) ON CONFLICT(user_id, game_id, slot) DO UPDATE SET revision = excluded.revision,
		sha256 = excluded.sha256, size = excluded.size, device_id = excluded.device_id, reason = excluded.reason,
		created_at = excluded.created_at, cheats_used = 0`, u, g, sl, rev, sha, size, in.DeviceID, SyncUpload, s.now().Unix()); err != nil {
		return SaveSlot{}, internal(err)
	}
	out, err := loadSlot(ctx, tx, u, g, sl)
	if err != nil {
		return SaveSlot{}, err
	}
	if err := tx.Commit(); err != nil {
		return SaveSlot{}, internal(err)
	}
	committed = true
	s.thinSlot(ctx, u, g, sl)
	s.notifySaveUpdated(u, in.DeviceID, g, sl, out.Current, "")
	return out, nil
}

// CreateSaveSnapshot creates a history version (manual_snapshot) from the current checkpoint. The label is
// optional (trimmed, up to MaxSnapshotLabel characters); ErrNotFound without a checkpoint.
func (s *Service) CreateSaveSnapshot(ctx context.Context, userID, gameID, slot string, label *string) (_ SaveVersion, err error) {
	defer s.publishOK(&err, TopicSaves)
	if !ValidSlotName(slot) {
		return SaveVersion{}, ErrNotFound
	}
	var lbl *string
	if label != nil {
		t := strings.TrimSpace(*label)
		if !utf8.ValidString(t) || utf8.RuneCountInString(t) > MaxSnapshotLabel {
			return SaveVersion{}, badRequest("Label must be at most %d characters", MaxSnapshotLabel)
		}
		if t != "" {
			lbl = &t
		}
	}
	s.saveMu.Lock()
	defer s.saveMu.Unlock()
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return SaveVersion{}, internal(err)
	}
	defer tx.Rollback()
	cur, err := loadCheckpoint(ctx, tx, userID, gameID, slot)
	if err != nil {
		return SaveVersion{}, err
	}
	ver, err := addHistory(ctx, tx, userID, gameID, slot, historyEntry{revision: cur.Revision, sha: cur.SHA256, size: cur.Size,
		deviceID: cur.DeviceID, syncReason: cur.Reason, reason: HistoryManualSnapshot, createdAt: s.now().Unix(), label: lbl, cheatsUsed: cur.CheatsUsed})
	if err != nil {
		return SaveVersion{}, internal(err)
	}
	v, err := scanVersion(tx.QueryRowContext(ctx, `SELECT `+versionCols+` FROM save_history h LEFT JOIN devices d ON d.id = h.device_id
		WHERE h.user_id = ? AND h.game_id = ? AND h.slot = ? AND h.version = ?`, userID, gameID, slot, ver))
	if err != nil {
		return SaveVersion{}, internal(err)
	}
	if err := tx.Commit(); err != nil {
		return SaveVersion{}, internal(err)
	}
	s.thinSlot(ctx, userID, gameID, slot)
	return v, nil
}

// DeleteSaveVersion deletes one version from the history of a slot (feature saves_v3 for manual snapshots, saves_v6
// for any version). A missing slot or version returns ErrNotFound; the secured version of an open conflict of the
// slot returns a conflict error. The checkpoint is never touched and nothing is pushed. The content file goes only
// if no checkpoint, version or conflict still uses it.
func (s *Service) DeleteSaveVersion(ctx context.Context, userID, gameID, slot string, version int) (err error) {
	defer s.publishOK(&err, TopicSaves)
	if !ValidSlotName(slot) || version < 1 {
		return ErrNotFound
	}
	s.saveMu.Lock()
	defer s.saveMu.Unlock()
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return internal(err)
	}
	defer tx.Rollback()
	var sha, reason string
	err = tx.QueryRowContext(ctx, `SELECT sha256, reason FROM save_history WHERE user_id = ? AND game_id = ? AND slot = ? AND version = ?`,
		userID, gameID, slot, version).Scan(&sha, &reason)
	if errors.Is(err, sql.ErrNoRows) {
		return ErrNotFound
	}
	if err != nil {
		return internal(err)
	}
	var secured int
	if err := tx.QueryRowContext(ctx, `SELECT COUNT(*) FROM save_conflicts WHERE user_id = ? AND game_id = ? AND slot = ? AND status = 'open' AND secured_version = ?`,
		userID, gameID, slot, version).Scan(&secured); err != nil {
		return internal(err)
	}
	if secured > 0 {
		return conflict("Resolve the open conflict first")
	}
	if _, err := tx.ExecContext(ctx, `DELETE FROM save_history WHERE user_id = ? AND game_id = ? AND slot = ? AND version = ?`,
		userID, gameID, slot, version); err != nil {
		return internal(err)
	}
	// Keep version numbers monotonic: the deleted number is never handed out again.
	if _, err := tx.ExecContext(ctx, `UPDATE save_slots SET history_high = MAX(history_high, ?) WHERE user_id = ? AND game_id = ? AND slot = ?`,
		version, userID, gameID, slot); err != nil {
		return internal(err)
	}
	if err := tx.Commit(); err != nil {
		return internal(err)
	}
	s.dropIfUnreferenced(ctx, userID, gameID, slot, sha)
	slog.Info("save version deleted", "user_id", userID, "game_id", gameID, "slot", slot, "version", version, "reason", reason)
	return nil
}

// CreateSaveSlot creates the slot newSlot of a game from the current checkpoint of fromSlot. The new slot starts at
// revision 1 (reason restore, device deviceID) and gets the history version v1 (manual_snapshot, label From “<fromSlot>”)
// so that the origin stays visible and is kept. ErrNotFound without a source checkpoint, ErrBadRequest for an invalid
// name, ErrSaveSlotExists if the name is taken. Connected devices of the user get save_updated.
func (s *Service) CreateSaveSlot(ctx context.Context, userID, gameID, fromSlot, newSlot, deviceID string) (_ SaveSlot, err error) {
	defer s.publishOK(&err, TopicSaves)
	if !ValidSlotName(fromSlot) {
		return SaveSlot{}, ErrNotFound
	}
	if !ValidNewSlotName(newSlot) {
		return SaveSlot{}, badRequest("Invalid slot name")
	}
	s.saveMu.Lock()
	defer s.saveMu.Unlock()
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return SaveSlot{}, internal(err)
	}
	defer tx.Rollback()
	src, err := loadCheckpoint(ctx, tx, userID, gameID, fromSlot)
	if err != nil {
		return SaveSlot{}, err
	}
	var n int
	if err := tx.QueryRowContext(ctx, `SELECT COUNT(*) FROM save_slots WHERE user_id = ? AND game_id = ? AND slot = ?`,
		userID, gameID, newSlot).Scan(&n); err != nil {
		return SaveSlot{}, internal(err)
	}
	if n > 0 {
		return SaveSlot{}, ErrSaveSlotExists
	}
	dst := s.saveFile(userID, gameID, newSlot, src.SHA256)
	placed, err := copyContent(s.saveFile(userID, gameID, fromSlot, src.SHA256), dst)
	if err != nil {
		return SaveSlot{}, internal(err)
	}
	committed := false
	defer func() {
		if !committed && placed {
			os.Remove(dst)
		}
	}()
	now := s.now().Unix()
	if _, err := tx.ExecContext(ctx, `INSERT INTO save_slots(user_id, game_id, slot, revision, sha256, size, device_id, reason, created_at, cheats_used)
		VALUES (?,?,?,?,?,?,?,?,?,?)`, userID, gameID, newSlot, 1, src.SHA256, src.Size, deviceID, SyncRestore, now, b2i(src.CheatsUsed)); err != nil {
		return SaveSlot{}, internal(err)
	}
	label := fmt.Sprintf("From “%s”", fromSlot)
	if r := []rune(label); len(r) > MaxSnapshotLabel {
		label = string(r[:MaxSnapshotLabel])
	}
	if _, err := addHistory(ctx, tx, userID, gameID, newSlot, historyEntry{revision: 1, sha: src.SHA256, size: src.Size,
		deviceID: deviceID, syncReason: SyncRestore, reason: HistoryManualSnapshot, createdAt: now, label: &label, cheatsUsed: src.CheatsUsed}); err != nil {
		return SaveSlot{}, internal(err)
	}
	out, err := loadSlot(ctx, tx, userID, gameID, newSlot)
	if err != nil {
		return SaveSlot{}, err
	}
	if err := tx.Commit(); err != nil {
		return SaveSlot{}, internal(err)
	}
	committed = true
	s.notifySaveUpdated(userID, deviceID, gameID, newSlot, out.Current, "")
	return out, nil
}

// copyContent places a copy of src at dst (hard link, else a byte copy). placed is false if dst already exists.
func copyContent(src, dst string) (placed bool, err error) {
	if _, err := os.Stat(dst); err == nil {
		return false, nil
	}
	if err := os.MkdirAll(filepath.Dir(dst), 0o750); err != nil {
		return false, err
	}
	if os.Link(src, dst) == nil {
		syncDir(filepath.Dir(dst))
		return true, nil
	}
	in, err := os.Open(src)
	if err != nil {
		return false, err
	}
	defer in.Close()
	tmp, err := os.CreateTemp(filepath.Dir(dst), ".copy-*")
	if err != nil {
		return false, err
	}
	_, err = io.Copy(tmp, in)
	if err == nil {
		err = tmp.Sync()
	}
	if cerr := tmp.Close(); err == nil {
		err = cerr
	}
	if err == nil {
		err = os.Chmod(tmp.Name(), 0o640)
	}
	if err == nil {
		err = os.Rename(tmp.Name(), dst)
	}
	if err != nil {
		os.Remove(tmp.Name())
		return false, err
	}
	syncDir(filepath.Dir(dst))
	return true, nil
}

// ---- retention ----

type thinRow struct {
	version   int
	sha       string
	createdAt time.Time
	keep      bool
}

// thinSlot applies the retention rules to the history of one slot: the newest `recent` versions, the newest
// version of each day within `daily` days and of each week within `weekly` weeks are kept; manual snapshots and
// versions referenced by an open conflict never go. Content files that nothing references any more are deleted.
// The caller holds saveMu. Errors are ignored: thinning runs again after the next insert and in the daily sweep.
func (s *Service) thinSlot(ctx context.Context, userID, gameID, slot string) {
	k := s.saveKeep
	if k.recent <= 0 {
		return
	}
	rows, err := s.db.QueryContext(ctx, `SELECT h.version, h.sha256, h.created_at,
		h.reason = 'manual_snapshot' OR EXISTS (SELECT 1 FROM save_conflicts c WHERE c.user_id = h.user_id AND c.game_id = h.game_id
			AND c.slot = h.slot AND c.status = 'open' AND c.secured_version = h.version)
		FROM save_history h WHERE h.user_id = ? AND h.game_id = ? AND h.slot = ? ORDER BY h.version DESC`, userID, gameID, slot)
	if err != nil {
		return
	}
	var list []thinRow
	for rows.Next() {
		var r thinRow
		var created int64
		if err := rows.Scan(&r.version, &r.sha, &created, &r.keep); err != nil {
			rows.Close()
			return
		}
		r.createdAt = time.Unix(created, 0).UTC()
		list = append(list, r)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return
	}
	now := s.now().UTC()
	days, weeks := map[string]bool{}, map[string]bool{}
	for i := range list {
		r := &list[i]
		if i < k.recent {
			r.keep = true
		}
		if k.daily <= 0 || !r.createdAt.Before(now.AddDate(0, 0, -k.daily)) {
			if key := r.createdAt.Format("2006-01-02"); !days[key] {
				days[key], r.keep = true, true
			}
		}
		if k.weekly <= 0 || !r.createdAt.Before(now.AddDate(0, 0, -7*k.weekly)) {
			y, w := r.createdAt.ISOWeek()
			if key := fmt.Sprintf("%d-%02d", y, w); !weeks[key] {
				weeks[key], r.keep = true, true
			}
		}
	}
	var drop []thinRow
	for _, r := range list {
		if !r.keep {
			drop = append(drop, r)
		}
	}
	if len(drop) == 0 {
		return
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return
	}
	defer tx.Rollback()
	for _, r := range drop {
		if _, err := tx.ExecContext(ctx, `DELETE FROM save_history WHERE user_id = ? AND game_id = ? AND slot = ? AND version = ?`,
			userID, gameID, slot, r.version); err != nil {
			return
		}
		// A resolved conflict whose secured upload is thinned away has nothing left to show.
		if _, err := tx.ExecContext(ctx, `DELETE FROM save_conflicts WHERE user_id = ? AND game_id = ? AND slot = ? AND status != 'open'
			AND secured_version = ?`, userID, gameID, slot, r.version); err != nil {
			return
		}
	}
	if tx.Commit() != nil {
		return
	}
	seen := map[string]bool{}
	for _, r := range drop {
		if !seen[r.sha] {
			seen[r.sha] = true
			s.dropIfUnreferenced(ctx, userID, gameID, slot, r.sha)
		}
	}
}

// SweepSaves applies the retention rules to every slot (daily sweep and start).
func (s *Service) SweepSaves(ctx context.Context) error {
	rows, err := s.db.QueryContext(ctx, `SELECT DISTINCT user_id, game_id, slot FROM save_history`)
	if err != nil {
		return internal(err)
	}
	type key struct{ u, g, sl string }
	var keys []key
	for rows.Next() {
		var k key
		if err := rows.Scan(&k.u, &k.g, &k.sl); err != nil {
			rows.Close()
			return internal(err)
		}
		keys = append(keys, k)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return internal(err)
	}
	for _, k := range keys {
		if ctx.Err() != nil {
			return ctx.Err()
		}
		s.saveMu.Lock()
		s.thinSlot(ctx, k.u, k.g, k.sl)
		s.saveMu.Unlock()
	}
	return nil
}

// RunSaveSweep sweeps at once (in the caller's goroutine, so start it with go), then every `every`, until ctx ends.
func (s *Service) RunSaveSweep(ctx context.Context, every time.Duration, onErr func(error)) {
	t := time.NewTicker(every)
	defer t.Stop()
	for {
		if err := s.SweepSaves(ctx); err != nil && onErr != nil && ctx.Err() == nil {
			onErr(err)
		}
		select {
		case <-ctx.Done():
			return
		case <-t.C:
		}
	}
}

// ---- push ----

// notifySaveUpdated sends save_updated for a changed checkpoint to the user's connected devices except the
// originating one (originDeviceID "" = all, e.g. a change made in the app). reason "" = the
// checkpoint's own sync reason.
func (s *Service) notifySaveUpdated(userID, originDeviceID, gameID, slot string, cp SaveCheckpoint, reason string) {
	s.notifySaveUpdatedExtra(userID, originDeviceID, gameID, slot, cp, reason, "")
}

// notifySaveUpdatedExtra is notifySaveUpdated plus previous_slot (renamed slot: slot is the new name).
func (s *Service) notifySaveUpdatedExtra(userID, originDeviceID, gameID, slot string, cp SaveCheckpoint, reason, previousSlot string) {
	if reason == "" {
		reason = cp.Reason
	}
	devID := uuid.Nil.String() // web changes have no device
	if id, err := uuid.Parse(cp.DeviceID); err == nil {
		devID = id.String()
	}
	payload := map[string]any{"game_id": gameID, "slot": slot, "revision": cp.Revision, "sha256": cp.SHA256,
		"device_id": devID, "device_name": cp.DeviceName, "reason": reason}
	if previousSlot != "" {
		payload["previous_slot"] = previousSlot
	}
	for _, c := range s.clients() {
		if c.userID == userID && c.deviceID != originDeviceID {
			c.sendMsg("save_updated", "", payload)
		}
	}
}

// SaveUpdated reasons for slot changes that are not a new checkpoint (feature saves_v5).
const (
	SaveReasonRenamed = "renamed"
	SaveReasonDeleted = "deleted"
)

// SessionRunningFor reports whether an active Session of the user runs the game (a hosted Session, not a viewed one).
func (s *Service) SessionRunningFor(ctx context.Context, userID, gameID string) (bool, error) {
	err := s.checkNoSession(ctx, s.db, userID, gameID)
	if errors.Is(err, ErrSessionRunning) {
		return true, nil
	}
	return false, err
}

// checkNoSession returns ErrSessionRunning while an active Session of the user runs the game.
func (s *Service) checkNoSession(ctx context.Context, q scanner2, userID, gameID string) error {
	var n int
	if err := q.QueryRowContext(ctx, `SELECT COUNT(*) FROM sessions WHERE ended_at IS NULL AND owner_user_id = ? AND game_id = ?`,
		userID, gameID).Scan(&n); err != nil {
		return internal(err)
	}
	if n > 0 {
		return ErrSessionRunning
	}
	return nil
}

type scanner2 interface {
	QueryRowContext(context.Context, string, ...any) *sql.Row
}

// RenameInput renames a slot (feature saves_v5). DeviceID is the caller's device or AdminDeviceID(user).
type RenameInput struct {
	UserID, DeviceID, GameID, Slot, NewSlot string
}

// RenameSaveSlot gives a slot a new name; checkpoint, history and conflicts move with it, nothing is lost.
// ErrNotFound for an unknown slot, ErrSaveSlotProtected for the default slot, ErrBadRequest for an invalid name (or
// the same name), ErrSaveSlotExists if the name is taken, ErrSessionRunning while a Session of the game runs.
// Other devices get save_updated (reason renamed, slot = the new name, previous_slot = the old one).
func (s *Service) RenameSaveSlot(ctx context.Context, in RenameInput) (_ SaveSlot, err error) {
	defer s.publishOK(&err, TopicSaves, TopicLibrary)
	u, g, from, to := in.UserID, in.GameID, in.Slot, in.NewSlot
	if !pathPartRe.MatchString(u) || !pathPartRe.MatchString(g) || !ValidSlotName(from) {
		return SaveSlot{}, ErrNotFound
	}
	s.saveMu.Lock()
	defer s.saveMu.Unlock()
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return SaveSlot{}, internal(err)
	}
	defer tx.Rollback()
	if _, err := loadCheckpoint(ctx, tx, u, g, from); err != nil {
		return SaveSlot{}, err
	}
	if from == DefaultSlot {
		return SaveSlot{}, ErrSaveSlotProtected
	}
	if !ValidNewSlotName(to) || to == from {
		return SaveSlot{}, badRequest("Invalid slot name")
	}
	if err := s.checkNoSession(ctx, tx, u, g); err != nil {
		return SaveSlot{}, err
	}
	var n int
	if err := tx.QueryRowContext(ctx, `SELECT (SELECT COUNT(*) FROM save_slots WHERE user_id = ?1 AND game_id = ?2 AND slot = ?3)
		+ (SELECT COUNT(*) FROM save_history WHERE user_id = ?1 AND game_id = ?2 AND slot = ?3)
		+ (SELECT COUNT(*) FROM save_conflicts WHERE user_id = ?1 AND game_id = ?2 AND slot = ?3)`, u, g, to).Scan(&n); err != nil {
		return SaveSlot{}, internal(err)
	}
	oldDir, newDir := s.saveDir(u, g, from), s.saveDir(u, g, to)
	_, statErr := os.Stat(newDir)
	if n > 0 || statErr == nil { // never merge into leftovers
		return SaveSlot{}, ErrSaveSlotExists
	}
	for _, t := range []string{"save_slots", "save_history", "save_conflicts"} {
		if _, err := tx.ExecContext(ctx, `UPDATE `+t+` SET slot = ? WHERE user_id = ? AND game_id = ? AND slot = ?`, to, u, g, from); err != nil {
			return SaveSlot{}, internal(err)
		}
	}
	out, err := loadSlot(ctx, tx, u, g, to)
	if err != nil {
		return SaveSlot{}, err
	}
	moved := false
	if err := os.Rename(oldDir, newDir); err == nil {
		moved = true
		syncDir(filepath.Dir(newDir))
	} else if !errors.Is(err, os.ErrNotExist) {
		return SaveSlot{}, internal(err)
	}
	if err := tx.Commit(); err != nil {
		if moved {
			os.Rename(newDir, oldDir)
		}
		return SaveSlot{}, internal(err)
	}
	slog.Info("save slot renamed", "user_id", u, "game_id", g, "from", from, "to", to, "revision", out.Current.Revision)
	s.notifySaveUpdatedExtra(u, in.DeviceID, g, to, out.Current, SaveReasonRenamed, from)
	return out, nil
}

// DeleteInput deletes a slot (feature saves_v5). ConfirmRevision must equal the current checkpoint revision.
type DeleteInput struct {
	UserID, DeviceID, GameID, Slot string
	ConfirmRevision                int
}

// DeleteSaveSlot deletes a slot with its checkpoint, history, conflicts and content files for good (hard delete). The
// caller must confirm the checkpoint revision it saw: a different revision returns ErrSaveConflictStale and nothing
// is deleted, so newer progress is never lost unseen. ErrNotFound for an unknown slot, ErrConflict while a conflict of the slot is open, ErrSessionRunning while a Session of the game
// runs. Other devices get save_updated (reason deleted). The deletion is logged.
func (s *Service) DeleteSaveSlot(ctx context.Context, in DeleteInput) (err error) {
	defer s.publishOK(&err, TopicSaves, TopicLibrary)
	u, g, sl := in.UserID, in.GameID, in.Slot
	if !pathPartRe.MatchString(u) || !pathPartRe.MatchString(g) || !ValidSlotName(sl) {
		return ErrNotFound
	}
	s.saveMu.Lock()
	defer s.saveMu.Unlock()
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return internal(err)
	}
	defer tx.Rollback()
	cur, err := loadCheckpoint(ctx, tx, u, g, sl)
	if err != nil {
		return err
	}
	if err := s.checkNoSession(ctx, tx, u, g); err != nil {
		return err
	}
	if in.ConfirmRevision != cur.Revision {
		return ErrSaveConflictStale
	}
	var open int
	if err := tx.QueryRowContext(ctx, `SELECT COUNT(*) FROM save_conflicts WHERE user_id = ? AND game_id = ? AND slot = ? AND status = 'open'`,
		u, g, sl).Scan(&open); err != nil {
		return internal(err)
	}
	if open > 0 {
		return conflict("Resolve the open conflict of this slot first")
	}
	for _, t := range []string{"save_conflicts", "save_history", "save_slots"} {
		if _, err := tx.ExecContext(ctx, `DELETE FROM `+t+` WHERE user_id = ? AND game_id = ? AND slot = ?`, u, g, sl); err != nil {
			return internal(err)
		}
	}
	if err := tx.Commit(); err != nil {
		return internal(err)
	}
	if err := os.RemoveAll(s.saveDir(u, g, sl)); err != nil {
		slog.Warn("delete save slot: remove content failed", "user_id", u, "game_id", g, "slot", sl, "err", err)
	}
	slog.Info("save slot deleted", "user_id", u, "game_id", g, "slot", sl, "revision", cur.Revision, "device_id", in.DeviceID)
	s.notifySaveUpdated(u, in.DeviceID, g, sl, cur, SaveReasonDeleted)
	return nil
}
