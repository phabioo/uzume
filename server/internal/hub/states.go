package hub

import (
	"context"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"errors"
	"fmt"
	"image/png"
	"io"
	"log/slog"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"time"
)

// Save states (0.20 package 2, feature states_v1): per user, game, save slot and state number 0..10 (0 = quick slot)
// one opaque core serialization (the "state", at most 64 MiB) with an optional PNG preview (at most 512 KiB). Files:
// <data>/states/<user>/<game>/<slot>/<n>.state and <n>.png; metadata in the save_states table. The server never reads
// the state. Own per-user quota (setting state_quota_bytes, default 2 GiB). Like saves, states keep existing when the
// game is deleted from the library; the clean-up "deleted_game_states" removes them.

// FeatureStatesV1 is the handshake feature flag for the save state endpoints.
const FeatureStatesV1 = "states_v1"

const (
	// MaxStateBytes is the maximum size of one state blob.
	MaxStateBytes = 64 << 20
	// MaxStatePreviewBytes is the maximum size of one preview PNG.
	MaxStatePreviewBytes = 512 << 10
	// MaxStateNumber is the highest state number (0 = quick slot).
	MaxStateNumber = 10
	// DefaultStateQuotaBytes is the per-user quota when the admin has not set one.
	DefaultStateQuotaBytes int64 = 2 << 30
	// MinStateQuotaBytes is the smallest quota the admin can set (one state must fit).
	MinStateQuotaBytes int64 = MaxStateBytes + MaxStatePreviewBytes
	// StateMetaMaxBytes is the maximum size of the meta part of an upload.
	StateMetaMaxBytes = 4 << 10

	settingStateQuota = "state_quota_bytes"
)

// CodeStateQuotaExceeded: the upload would exceed the user's save state quota (413).
const CodeStateQuotaExceeded Code = "state_quota_exceeded"

// ErrStateQuotaExceeded is returned when a state does not fit into the user's quota.
var ErrStateQuotaExceeded = &Error{Code: CodeStateQuotaExceeded, Message: "Not enough save state space left; delete older states first"}

var stateCoreIDRe = regexp.MustCompile(`^[a-z0-9][a-z0-9_-]{0,63}$`)

// SaveState is the metadata of one stored state.
type SaveState struct {
	GameID       string
	Slot         string
	N            int
	CoreID       string
	CoreVersion  string
	SizeBytes    int64
	PreviewBytes int64 // 0 = no preview
	SHA256       string
	DeviceID     string
	DeviceName   string
	CreatedAt    time.Time
}

// SaveStateList lists the states of a game plus the user's quota numbers.
type SaveStateList struct {
	States     []SaveState
	UsedBytes  int64 // all states of the user (every game), blobs and previews
	QuotaBytes int64
}

// PutStateInput is an upload. State is read to the end (at most MaxStateBytes); the preview is optional.
type PutStateInput struct {
	UserID, DeviceID, GameID, Slot string
	N                              int
	CoreID, CoreVersion            string
	State                          io.Reader
	// NextPreview is called after State was read to its end and returns the optional preview (nil reader = none).
	NextPreview func() (io.Reader, error)
}

func (s *Service) stateDir(userID, gameID, slot string) string {
	return filepath.Join(s.dataDir, "states", userID, gameID, slot)
}

func (s *Service) stateFile(userID, gameID, slot string, n int) string {
	return filepath.Join(s.stateDir(userID, gameID, slot), strconv.Itoa(n)+".state")
}

func (s *Service) statePreviewFile(userID, gameID, slot string, n int) string {
	return filepath.Join(s.stateDir(userID, gameID, slot), strconv.Itoa(n)+".png")
}

// StateQuota returns the per-user quota in bytes.
func (s *Service) StateQuota(ctx context.Context) (int64, error) {
	v, ok, err := s.getSetting(ctx, settingStateQuota)
	if err != nil {
		return 0, err
	}
	if !ok {
		return DefaultStateQuotaBytes, nil
	}
	n, perr := strconv.ParseInt(v, 10, 64)
	if perr != nil || n < MinStateQuotaBytes {
		return DefaultStateQuotaBytes, nil
	}
	return n, nil
}

// SetStateQuota sets the per-user quota (at least MinStateQuotaBytes).
func (s *Service) SetStateQuota(ctx context.Context, bytes int64) error {
	if bytes < MinStateQuotaBytes {
		return badRequest("The save state quota must be at least %d MiB", MinStateQuotaBytes>>20+1)
	}
	return s.setSetting(ctx, settingStateQuota, strconv.FormatInt(bytes, 10))
}

var stateCols = `st.game_id, st.slot, st.n, st.core_id, st.core_version, st.size_bytes, st.preview_bytes, st.sha256, st.device_id, ` +
	deviceNameSQL("d", "st.device_id") + `, st.created_at`

func scanSaveState(r scanner) (SaveState, error) {
	var st SaveState
	var created int64
	if err := r.Scan(&st.GameID, &st.Slot, &st.N, &st.CoreID, &st.CoreVersion, &st.SizeBytes, &st.PreviewBytes, &st.SHA256,
		&st.DeviceID, &st.DeviceName, &created); err != nil {
		return SaveState{}, err
	}
	st.CreatedAt = time.Unix(created, 0).UTC()
	return st, nil
}

func (s *Service) stateRow(ctx context.Context, userID, gameID, slot string, n int) (SaveState, error) {
	st, err := scanSaveState(s.db.QueryRowContext(ctx, `SELECT `+stateCols+` FROM save_states st LEFT JOIN devices d ON d.id = st.device_id
		WHERE st.user_id = ? AND st.game_id = ? AND st.slot = ? AND st.n = ?`, userID, gameID, slot, n))
	if errors.Is(err, sql.ErrNoRows) {
		return SaveState{}, ErrNotFound
	}
	if err != nil {
		return SaveState{}, internal(err)
	}
	return st, nil
}

func (s *Service) stateUsed(ctx context.Context, q dbq, userID string) (int64, error) {
	var n int64
	if err := q.QueryRowContext(ctx, `SELECT COALESCE(SUM(size_bytes + preview_bytes), 0) FROM save_states WHERE user_id = ?`, userID).Scan(&n); err != nil {
		return 0, internal(err)
	}
	return n, nil
}

func validStateKey(slot string, n int) error {
	if !ValidSlotName(slot) {
		return ErrNotFound
	}
	if n < 0 || n > MaxStateNumber {
		return ErrNotFound
	}
	return nil
}

// ListStates returns the caller's states of a game, optionally only one save slot, ordered by slot and number.
// ErrNotFound for a game the caller cannot see.
func (s *Service) ListStates(ctx context.Context, userID, gameID, slot string) (SaveStateList, error) {
	if _, err := s.GetGameFor(ctx, userID, gameID); err != nil {
		return SaveStateList{}, err
	}
	q := `SELECT ` + stateCols + ` FROM save_states st LEFT JOIN devices d ON d.id = st.device_id WHERE st.user_id = ? AND st.game_id = ?`
	args := []any{userID, gameID}
	if slot != "" {
		if !ValidSlotName(slot) {
			return SaveStateList{}, badRequest("Invalid slot name")
		}
		q += ` AND st.slot = ?`
		args = append(args, slot)
	}
	rows, err := s.db.QueryContext(ctx, q+` ORDER BY st.slot, st.n`, args...)
	if err != nil {
		return SaveStateList{}, internal(err)
	}
	out := SaveStateList{States: []SaveState{}}
	for rows.Next() {
		st, err := scanSaveState(rows)
		if err != nil {
			rows.Close()
			return SaveStateList{}, internal(err)
		}
		out.States = append(out.States, st)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return SaveStateList{}, internal(err)
	}
	if out.UsedBytes, err = s.stateUsed(ctx, s.db, userID); err != nil {
		return SaveStateList{}, err
	}
	if out.QuotaBytes, err = s.StateQuota(ctx); err != nil {
		return SaveStateList{}, err
	}
	return out, nil
}

// stage copies r into a temp file (at most max bytes) and returns the name, size and SHA-256. A larger body is a
// payload_too_large error carrying what.
func (s *Service) stageLimited(r io.Reader, max int64, what string) (name string, size int64, sum string, err error) {
	tmp, err := os.CreateTemp(filepath.Join(s.dataDir, "tmp"), "state-*")
	if err != nil {
		return "", 0, "", internal(err)
	}
	name = tmp.Name()
	h := sha256.New()
	size, err = io.Copy(io.MultiWriter(tmp, h), io.LimitReader(r, max+1))
	if err == nil && size > max {
		err = ErrPayloadTooLarge
	}
	if err == nil {
		err = tmp.Sync()
	}
	if cerr := tmp.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		os.Remove(name)
		var mbe *http.MaxBytesError
		if errors.Is(err, ErrPayloadTooLarge) || errors.As(err, &mbe) {
			return "", 0, "", &Error{Code: CodePayloadTooLarge, Message: fmt.Sprintf("%s exceeds %s", what, humanBytes(max))}
		}
		return "", 0, "", internal(err)
	}
	return name, size, hex.EncodeToString(h.Sum(nil)), nil
}

func humanBytes(n int64) string {
	if n >= 1<<20 {
		return fmt.Sprintf("%d MiB", n>>20)
	}
	return fmt.Sprintf("%d KiB", n>>10)
}

// PutState stores or replaces one state. The blob and the preview are written to temporary files first; a state that
// does not fit into the quota (counting the state it replaces as freed) is refused with 413 state_quota_exceeded and
// changes nothing. A replaced state without a new preview loses its old preview.
func (s *Service) PutState(ctx context.Context, in PutStateInput) (SaveState, error) {
	if err := validStateKey(in.Slot, in.N); err != nil {
		return SaveState{}, err
	}
	if !stateCoreIDRe.MatchString(in.CoreID) || in.CoreVersion == "" || len(in.CoreVersion) > 64 {
		return SaveState{}, badRequest("core_id and core_version are required")
	}
	if _, err := s.GetGameFor(ctx, in.UserID, in.GameID); err != nil {
		return SaveState{}, err
	}
	stateTmp, size, sum, err := s.stageLimited(in.State, MaxStateBytes, "State")
	if err != nil {
		return SaveState{}, err
	}
	defer os.Remove(stateTmp) // no effect after a successful rename
	if size == 0 {
		return SaveState{}, badRequest("The state is empty")
	}
	var prevTmp string
	var prevSize int64
	var preview io.Reader
	if in.NextPreview != nil {
		if preview, err = in.NextPreview(); err != nil {
			return SaveState{}, err
		}
	}
	if preview != nil {
		prevTmp, prevSize, _, err = s.stageLimited(preview, MaxStatePreviewBytes, "Preview")
		if err != nil {
			return SaveState{}, err
		}
		defer os.Remove(prevTmp)
		if prevSize > 0 {
			f, oerr := os.Open(prevTmp)
			if oerr != nil {
				return SaveState{}, internal(oerr)
			}
			cfg, derr := png.DecodeConfig(f)
			f.Close()
			if derr != nil || cfg.Width <= 0 || cfg.Height <= 0 {
				return SaveState{}, ErrInvalidImage
			}
		} else {
			prevTmp = ""
		}
	}

	s.stateMu.Lock()
	defer s.stateMu.Unlock()
	used, err := s.stateUsed(ctx, s.db, in.UserID)
	if err != nil {
		return SaveState{}, err
	}
	var oldSize, oldPrev int64
	switch err := s.db.QueryRowContext(ctx, `SELECT size_bytes, preview_bytes FROM save_states WHERE user_id = ? AND game_id = ? AND slot = ? AND n = ?`,
		in.UserID, in.GameID, in.Slot, in.N).Scan(&oldSize, &oldPrev); {
	case errors.Is(err, sql.ErrNoRows):
	case err != nil:
		return SaveState{}, internal(err)
	}
	quota, err := s.StateQuota(ctx)
	if err != nil {
		return SaveState{}, err
	}
	if used-oldSize-oldPrev+size+prevSize > quota {
		return SaveState{}, ErrStateQuotaExceeded
	}

	dst := s.stateFile(in.UserID, in.GameID, in.Slot, in.N)
	if err := os.MkdirAll(filepath.Dir(dst), 0o750); err != nil {
		return SaveState{}, internal(err)
	}
	if err := os.Chmod(stateTmp, 0o640); err != nil {
		return SaveState{}, internal(err)
	}
	if err := os.Rename(stateTmp, dst); err != nil {
		return SaveState{}, internal(err)
	}
	pdst := s.statePreviewFile(in.UserID, in.GameID, in.Slot, in.N)
	if prevTmp != "" {
		if err := os.Chmod(prevTmp, 0o640); err != nil {
			return SaveState{}, internal(err)
		}
		if err := os.Rename(prevTmp, pdst); err != nil {
			return SaveState{}, internal(err)
		}
	} else if err := os.Remove(pdst); err != nil && !errors.Is(err, os.ErrNotExist) {
		slog.Warn("states: remove old preview failed", "err", err)
	}
	syncDir(filepath.Dir(dst))
	if _, err := s.db.ExecContext(ctx, `INSERT INTO save_states(user_id, game_id, slot, n, core_id, core_version, size_bytes, preview_bytes, sha256, device_id, created_at)
		VALUES (?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(user_id, game_id, slot, n) DO UPDATE SET core_id = excluded.core_id,
		core_version = excluded.core_version, size_bytes = excluded.size_bytes, preview_bytes = excluded.preview_bytes,
		sha256 = excluded.sha256, device_id = excluded.device_id, created_at = excluded.created_at`,
		in.UserID, in.GameID, in.Slot, in.N, in.CoreID, in.CoreVersion, size, prevSize, sum, in.DeviceID, s.now().Unix()); err != nil {
		return SaveState{}, internal(err)
	}
	return s.stateRow(ctx, in.UserID, in.GameID, in.Slot, in.N)
}

// OpenState opens the blob of a state of the caller; the caller closes the file.
func (s *Service) OpenState(ctx context.Context, userID, gameID, slot string, n int) (*os.File, SaveState, error) {
	if err := validStateKey(slot, n); err != nil {
		return nil, SaveState{}, err
	}
	st, err := s.stateRow(ctx, userID, gameID, slot, n)
	if err != nil {
		return nil, SaveState{}, err
	}
	f, err := os.Open(s.stateFile(userID, gameID, slot, n))
	if errors.Is(err, os.ErrNotExist) {
		return nil, SaveState{}, ErrNotFound
	}
	if err != nil {
		return nil, SaveState{}, internal(err)
	}
	return f, st, nil
}

// OpenStatePreview opens the preview PNG; ErrNotFound when the state has none.
func (s *Service) OpenStatePreview(ctx context.Context, userID, gameID, slot string, n int) (*os.File, SaveState, error) {
	if err := validStateKey(slot, n); err != nil {
		return nil, SaveState{}, err
	}
	st, err := s.stateRow(ctx, userID, gameID, slot, n)
	if err != nil {
		return nil, SaveState{}, err
	}
	if st.PreviewBytes == 0 {
		return nil, SaveState{}, ErrNotFound
	}
	f, err := os.Open(s.statePreviewFile(userID, gameID, slot, n))
	if errors.Is(err, os.ErrNotExist) {
		return nil, SaveState{}, ErrNotFound
	}
	if err != nil {
		return nil, SaveState{}, internal(err)
	}
	return f, st, nil
}

// DeleteState removes one state of the caller (row and files); ErrNotFound when there is none.
func (s *Service) DeleteState(ctx context.Context, userID, gameID, slot string, n int) error {
	if err := validStateKey(slot, n); err != nil {
		return err
	}
	s.stateMu.Lock()
	defer s.stateMu.Unlock()
	res, err := s.db.ExecContext(ctx, `DELETE FROM save_states WHERE user_id = ? AND game_id = ? AND slot = ? AND n = ?`, userID, gameID, slot, n)
	if err != nil {
		return internal(err)
	}
	if c, _ := res.RowsAffected(); c == 0 {
		return ErrNotFound
	}
	s.removeStateFiles(userID, gameID, slot, n)
	return nil
}

func (s *Service) removeStateFiles(userID, gameID, slot string, n int) {
	for _, p := range []string{s.stateFile(userID, gameID, slot, n), s.statePreviewFile(userID, gameID, slot, n)} {
		if err := os.Remove(p); err != nil && !errors.Is(err, os.ErrNotExist) {
			slog.Warn("states: remove file failed", "err", err)
		}
	}
	// Tidy up empty directories (best effort; fails while something is left).
	d := s.stateDir(userID, gameID, slot)
	for i := 0; i < 3; i++ {
		if os.Remove(d) != nil {
			break
		}
		d = filepath.Dir(d)
	}
}

// removeUserStateFiles removes all state files of a user (rows go with the user by cascade).
func (s *Service) removeUserStateFiles(userID string) {
	_ = os.RemoveAll(filepath.Join(s.dataDir, "states", userID)) // best effort
}

// deletedGameStates are the states whose game no longer exists in the library.
const deletedGameStates = `SELECT user_id, game_id, slot, n, size_bytes + preview_bytes FROM save_states WHERE game_id NOT IN (SELECT id FROM games)`

// deletedGameStateStats counts the states of deleted games and their bytes (clean-up suggestion).
func (s *Service) deletedGameStateStats(ctx context.Context) (StorageCleanup, error) {
	res := StorageCleanup{ID: CleanupDeletedGameStates}
	err := s.db.QueryRowContext(ctx, `SELECT COUNT(*), COALESCE(SUM(size_bytes + preview_bytes), 0) FROM save_states
		WHERE game_id NOT IN (SELECT id FROM games)`).Scan(&res.Count, &res.Bytes)
	if err != nil {
		return res, internal(err)
	}
	return res, nil
}

// cleanupDeletedGameStates removes the states of deleted games (rows and files).
func (s *Service) cleanupDeletedGameStates(ctx context.Context) (res StorageCleanupResult, err error) {
	s.stateMu.Lock()
	defer s.stateMu.Unlock()
	rows, err := s.db.QueryContext(ctx, deletedGameStates)
	if err != nil {
		return res, internal(err)
	}
	type row struct {
		user, game, slot string
		n                int
		bytes            int64
	}
	var gone []row
	for rows.Next() {
		var r row
		if err := rows.Scan(&r.user, &r.game, &r.slot, &r.n, &r.bytes); err != nil {
			rows.Close()
			return res, internal(err)
		}
		gone = append(gone, r)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return res, internal(err)
	}
	for _, r := range gone {
		if _, err := s.db.ExecContext(ctx, `DELETE FROM save_states WHERE user_id = ? AND game_id = ? AND slot = ? AND n = ?`, r.user, r.game, r.slot, r.n); err != nil {
			return res, internal(err)
		}
		s.removeStateFiles(r.user, r.game, r.slot, r.n)
		res.Removed++
		res.FreedBytes += r.bytes
	}
	return res, nil
}
