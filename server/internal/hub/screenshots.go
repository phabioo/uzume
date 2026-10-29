package hub

import (
	"context"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"errors"
	"image/png"
	"io"
	"log/slog"
	"net/http"
	"os"
	"path/filepath"
	"time"

	"github.com/google/uuid"
)

// Screenshots (0.19 package 5): PNG files per user and game under <data>/screenshots/<user>/<game>/<id>.png, metadata
// in the screenshots table, optionally tagged with a run the user joined for that game.

// MaxScreenshotBytes is the maximum size of one screenshot.
const MaxScreenshotBytes = 16 << 20

// CodeInvalidImage: the body is not a PNG image (400).
const CodeInvalidImage Code = "invalid_image"

// ErrInvalidImage is returned for a body that is not a decodable PNG.
var ErrInvalidImage = &Error{Code: CodeInvalidImage, Message: "The body is not a PNG image"}

// Screenshot is one stored screenshot.
type Screenshot struct {
	ID        string
	GameID    string
	RunID     *string
	UserID    string
	UserName  string
	CreatedAt time.Time
	Width     int
	Height    int
	SizeBytes int64
}

// ScreenshotList is a list plus the summed size of its files.
type ScreenshotList struct {
	Screenshots []Screenshot
	TotalBytes  int64
}

// ScreenshotUsageRow is the screenshot storage of one user and game.
type ScreenshotUsageRow struct {
	UserID string
	GameID string
	Count  int
	Bytes  int64
}

// ScreenshotUsageResult lists the usage per user and game plus the total.
type ScreenshotUsageResult struct {
	Rows       []ScreenshotUsageRow
	TotalCount int
	TotalBytes int64
}

func (s *Service) screenshotDir(userID, gameID string) string {
	return filepath.Join(s.dataDir, "screenshots", userID, gameID)
}

func (s *Service) screenshotFile(userID, gameID, id string) string {
	return filepath.Join(s.screenshotDir(userID, gameID), id+".png")
}

const screenshotCols = `s.id, s.game_id, s.run_id, s.user_id, u.display_name, s.created_at, s.width, s.height, s.size_bytes`

func scanScreenshot(r scanner) (Screenshot, error) {
	var sc Screenshot
	var run sql.NullString
	var created int64
	if err := r.Scan(&sc.ID, &sc.GameID, &run, &sc.UserID, &sc.UserName, &created, &sc.Width, &sc.Height, &sc.SizeBytes); err != nil {
		return Screenshot{}, err
	}
	if run.Valid {
		sc.RunID = &run.String
	}
	sc.CreatedAt = time.Unix(created, 0).UTC()
	return sc, nil
}

// AddScreenshot stores a PNG of the user for a game. runID "" = untagged; otherwise the user must have joined that
// run with this game, else ErrNotFound.
func (s *Service) AddScreenshot(ctx context.Context, userID, gameID, runID string, body io.Reader) (Screenshot, error) {
	if _, err := s.GetGameFor(ctx, userID, gameID); err != nil {
		return Screenshot{}, err
	}
	if runID != "" {
		var n int
		if err := s.db.QueryRowContext(ctx, `SELECT COUNT(*) FROM run_participants WHERE run_id = ? AND user_id = ? AND status = 'joined' AND game_id = ?`,
			runID, userID, gameID).Scan(&n); err != nil {
			return Screenshot{}, internal(err)
		}
		if n == 0 {
			return Screenshot{}, ErrNotFound
		}
	}
	tmp, err := os.CreateTemp(filepath.Join(s.dataDir, "tmp"), "shot-*")
	if err != nil {
		return Screenshot{}, internal(err)
	}
	tmpName := tmp.Name()
	defer os.Remove(tmpName) // no-op after a successful rename
	h := sha256.New()
	size, err := io.Copy(io.MultiWriter(tmp, h), io.LimitReader(body, MaxScreenshotBytes+1))
	if err == nil && size > MaxScreenshotBytes {
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
		if errors.Is(err, ErrPayloadTooLarge) || errors.As(err, &mbe) {
			return Screenshot{}, &Error{Code: CodePayloadTooLarge, Message: "Screenshot exceeds 16 MiB"}
		}
		return Screenshot{}, internal(err)
	}
	f, err := os.Open(tmpName)
	if err != nil {
		return Screenshot{}, internal(err)
	}
	cfg, derr := png.DecodeConfig(f)
	f.Close()
	if derr != nil || cfg.Width <= 0 || cfg.Height <= 0 {
		return Screenshot{}, ErrInvalidImage
	}
	id := uuid.NewString()
	dst := s.screenshotFile(userID, gameID, id)
	if err := os.MkdirAll(filepath.Dir(dst), 0o750); err != nil {
		return Screenshot{}, internal(err)
	}
	if err := os.Chmod(tmpName, 0o640); err != nil {
		return Screenshot{}, internal(err)
	}
	if err := os.Rename(tmpName, dst); err != nil {
		return Screenshot{}, internal(err)
	}
	syncDir(filepath.Dir(dst))
	now := s.Now().Unix()
	var run any
	if runID != "" {
		run = runID
	}
	if _, err := s.db.ExecContext(ctx, `INSERT INTO screenshots(id, user_id, game_id, run_id, created_at, width, height, size_bytes, sha256)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`, id, userID, gameID, run, now, cfg.Width, cfg.Height, size, hex.EncodeToString(h.Sum(nil))); err != nil {
		os.Remove(dst)
		return Screenshot{}, internal(err)
	}
	return s.screenshotByID(ctx, id)
}

func (s *Service) screenshotByID(ctx context.Context, id string) (Screenshot, error) {
	sc, err := scanScreenshot(s.db.QueryRowContext(ctx, `SELECT `+screenshotCols+` FROM screenshots s JOIN users u ON u.id = s.user_id WHERE s.id = ?`, id))
	if errors.Is(err, sql.ErrNoRows) {
		return Screenshot{}, ErrNotFound
	}
	if err != nil {
		return Screenshot{}, internal(err)
	}
	return sc, nil
}

func (s *Service) listScreenshots(ctx context.Context, where string, args ...any) (ScreenshotList, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT `+screenshotCols+` FROM screenshots s JOIN users u ON u.id = s.user_id WHERE `+where+
		` ORDER BY s.created_at DESC, s.rowid DESC`, args...)
	if err != nil {
		return ScreenshotList{}, internal(err)
	}
	defer rows.Close()
	out := ScreenshotList{Screenshots: []Screenshot{}}
	for rows.Next() {
		sc, err := scanScreenshot(rows)
		if err != nil {
			return ScreenshotList{}, internal(err)
		}
		out.Screenshots = append(out.Screenshots, sc)
		out.TotalBytes += sc.SizeBytes
	}
	if err := rows.Err(); err != nil {
		return ScreenshotList{}, internal(err)
	}
	return out, nil
}

// ListScreenshots returns the caller's own screenshots of a game, newest first.
func (s *Service) ListScreenshots(ctx context.Context, userID, gameID string) (ScreenshotList, error) {
	return s.listScreenshots(ctx, `s.user_id = ? AND s.game_id = ?`, userID, gameID)
}

// isJoinedInRun reports whether the user joined the run.
func (s *Service) isJoinedInRun(ctx context.Context, userID, runID string) (bool, error) {
	var n int
	if err := s.db.QueryRowContext(ctx, `SELECT COUNT(*) FROM run_participants WHERE run_id = ? AND user_id = ? AND status = 'joined'`,
		runID, userID).Scan(&n); err != nil {
		return false, internal(err)
	}
	return n > 0, nil
}

// ListRunScreenshots returns the screenshots of all members tagged with the run, newest first; ErrNotFound for
// everybody who has not joined the run.
func (s *Service) ListRunScreenshots(ctx context.Context, userID, runID string) (ScreenshotList, error) {
	ok, err := s.isJoinedInRun(ctx, userID, runID)
	if err != nil {
		return ScreenshotList{}, err
	}
	if !ok {
		return ScreenshotList{}, ErrNotFound
	}
	return s.listScreenshots(ctx, `s.run_id = ?`, runID)
}

// OpenScreenshot opens the PNG for the owner or a member of the screenshot's run; the caller closes the file.
func (s *Service) OpenScreenshot(ctx context.Context, userID, id string) (*os.File, Screenshot, error) {
	sc, err := s.screenshotByID(ctx, id)
	if err != nil {
		return nil, Screenshot{}, err
	}
	if sc.UserID != userID {
		ok := false
		if sc.RunID != nil {
			if ok, err = s.isJoinedInRun(ctx, userID, *sc.RunID); err != nil {
				return nil, Screenshot{}, err
			}
		}
		if !ok {
			return nil, Screenshot{}, ErrNotFound
		}
	}
	f, err := os.Open(s.screenshotFile(sc.UserID, sc.GameID, sc.ID))
	if errors.Is(err, os.ErrNotExist) {
		return nil, Screenshot{}, ErrNotFound
	}
	if err != nil {
		return nil, Screenshot{}, internal(err)
	}
	return f, sc, nil
}

// DeleteScreenshot removes the caller's own screenshot (row and file); ErrNotFound for anybody else's.
func (s *Service) DeleteScreenshot(ctx context.Context, userID, id string) error {
	res, err := s.db.ExecContext(ctx, `DELETE FROM screenshots WHERE id = ? AND user_id = ?`, id, userID)
	if err != nil {
		return internal(err)
	}
	if n, _ := res.RowsAffected(); n == 0 {
		return ErrNotFound
	}
	// The row is gone; its game id is not needed because the file name is found by the id.
	s.removeScreenshotFile(userID, id)
	return nil
}

// removeScreenshotFile removes <user>/<game>/<id>.png without knowing the game directory.
func (s *Service) removeScreenshotFile(userID, id string) {
	matches, _ := filepath.Glob(filepath.Join(s.dataDir, "screenshots", userID, "*", id+".png"))
	for _, m := range matches {
		if err := os.Remove(m); err != nil && !errors.Is(err, os.ErrNotExist) {
			slog.Warn("screenshots: remove file failed", "err", err)
		}
	}
}

// removeUserScreenshotFiles removes all screenshot files of a user (rows go with the user by cascade).
func (s *Service) removeUserScreenshotFiles(userID string) {
	_ = os.RemoveAll(filepath.Join(s.dataDir, "screenshots", userID)) // best effort
}

// removeGameScreenshotFiles removes the screenshot files of a game of every user.
func (s *Service) removeGameScreenshotFiles(userIDs []string, gameID string) {
	for _, u := range userIDs {
		_ = os.RemoveAll(s.screenshotDir(u, gameID)) // best effort
	}
}

// ScreenshotUsage returns the number and size of screenshots per user and game plus the total (storage overview).
func (s *Service) ScreenshotUsage(ctx context.Context) (ScreenshotUsageResult, error) {
	return s.ScreenshotUsageFor(ctx, "", "")
}

// ScreenshotUsageFor is ScreenshotUsage narrowed to one user and/or one game ("" = any).
func (s *Service) ScreenshotUsageFor(ctx context.Context, userID, gameID string) (ScreenshotUsageResult, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT user_id, game_id, COUNT(*), COALESCE(SUM(size_bytes), 0) FROM screenshots
		WHERE (?1 = '' OR user_id = ?1) AND (?2 = '' OR game_id = ?2)
		GROUP BY user_id, game_id ORDER BY user_id, game_id`, userID, gameID)
	if err != nil {
		return ScreenshotUsageResult{}, internal(err)
	}
	defer rows.Close()
	out := ScreenshotUsageResult{Rows: []ScreenshotUsageRow{}}
	for rows.Next() {
		var r ScreenshotUsageRow
		if err := rows.Scan(&r.UserID, &r.GameID, &r.Count, &r.Bytes); err != nil {
			return ScreenshotUsageResult{}, internal(err)
		}
		out.Rows = append(out.Rows, r)
		out.TotalCount += r.Count
		out.TotalBytes += r.Bytes
	}
	return out, rows.Err()
}
