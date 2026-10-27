package hub

import (
	"context"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"errors"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"time"
	"unicode"
	"unicode/utf8"
)

// System is a known game system (library view of the registry, see systems.go).
type System struct {
	ID         string
	Name       string
	Extensions []string
}

// Systems returns the known systems (registry order by ID).
func (s *Service) Systems(ctx context.Context) ([]System, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT id, display_name, extensions FROM systems ORDER BY id`)
	if err != nil {
		return nil, internal(err)
	}
	defer rows.Close()
	var out []System
	for rows.Next() {
		var sys System
		var ext string
		if err := rows.Scan(&sys.ID, &sys.Name, &ext); err != nil {
			return nil, internal(err)
		}
		sys.Extensions = splitList(ext)
		out = append(out, sys)
	}
	return out, rows.Err()
}

// systemForFilename derives the system from the file extension.
func systemForFilename(systems []System, filename string) (string, bool) {
	ext := strings.ToLower(filepath.Ext(filename))
	for _, s := range systems {
		for _, e := range s.Extensions {
			if e == ext {
				return s.ID, true
			}
		}
	}
	return "", false
}

// Game is a library entry; the ROM file lives at <data-dir>/roms/<sha[:2]>/<sha>.
type Game struct {
	ID         string
	Title      string
	System     string
	ROMSHA256  string
	ROMSize    int64
	Filename   string
	UploadedBy string
	AddedAt    time.Time
	// GameCode and GameCodeKind: identity read from the ROM header ("" when unknown or unreadable).
	GameCode     string
	GameCodeKind string
	// GameVersion is the ROM version of the identity (nil = unknown).
	GameVersion *int
	// ROMs are all known files of the game (primary first); OnHub is true when at least one is stored on the server.
	// ROMSHA256/ROMSize/Filename describe a stored file if there is one, else the primary file.
	ROMs  []ROMFile
	OnHub bool
}

// ROMFile is one known file of a game. Stored files are on the server; the others are only registered by their owners.
type ROMFile struct {
	SHA256, Filename, AddedBy string
	Size                      int64
	Stored                    bool
	AddedAt                   time.Time
}

// MaxROMBytes is the upload limit for ROMs (4 GiB), shared by the app and the API.
const MaxROMBytes int64 = 4 << 30

var sha256Re = regexp.MustCompile(`^[0-9a-f]{64}$`)

// ValidSHA256 checks the format of a ROM hash (64 lowercase hex characters).
func ValidSHA256(s string) bool { return sha256Re.MatchString(s) }

func (s *Service) romPath(sha string) string {
	return filepath.Join(s.dataDir, "roms", sha[:2], sha)
}

const gameCols = `id, title, system, rom_sha256, rom_size, filename, uploaded_by, added_at, game_code, game_code_kind, game_version`

func scanGame(r scanner) (Game, error) {
	var g Game
	var added int64
	var ver sql.NullInt64
	if err := r.Scan(&g.ID, &g.Title, &g.System, &g.ROMSHA256, &g.ROMSize, &g.Filename, &g.UploadedBy, &added, &g.GameCode, &g.GameCodeKind, &ver); err != nil {
		return Game{}, err
	}
	if ver.Valid {
		v := int(ver.Int64)
		g.GameVersion = &v
	}
	if g.GameCodeKind == gameCodeNone {
		g.GameCodeKind = ""
	}
	g.AddedAt = time.Unix(added, 0).UTC()
	return g, nil
}

func cleanText(v string, max int) string {
	v = strings.Map(func(r rune) rune {
		if unicode.IsControl(r) {
			return -1
		}
		return r
	}, v)
	v = strings.TrimSpace(v)
	if len(v) > max {
		cut := max
		for cut > 0 && !utf8.RuneStart(v[cut]) { // never cut inside a multi-byte rune
			cut--
		}
		v = strings.TrimSpace(v[:cut])
	}
	return v
}

// AddROM streams r into a temp file in the data directory, computes SHA-256 and places the file atomically
// at roms/<sha[:2]>/<sha>. Empty system: derive from the file extension. Empty title: file name without
// extension. A ROM that already exists (same hash) results in a conflict error.
func (s *Service) AddROM(ctx context.Context, r io.Reader, filename, title, system, uploadedBy string) (_ Game, err error) {
	defer s.publishOK(&err, TopicLibrary)
	filename = cleanText(filepath.Base(strings.ReplaceAll(filename, `\`, "/")), 255)
	if filename == "" || filename == "." || filename == "/" {
		return Game{}, badRequest("File name missing")
	}
	systems, err := s.Systems(ctx)
	if err != nil {
		return Game{}, err
	}
	if system == "" {
		var ok bool
		if system, ok = systemForFilename(systems, filename); !ok {
			return Game{}, badRequest("System cannot be derived from the file extension")
		}
	} else {
		known := false
		for _, sys := range systems {
			known = known || sys.ID == system
		}
		if !known {
			return Game{}, badRequest("Unknown system")
		}
	}
	title = cleanText(title, 200)
	if title == "" {
		title = strings.TrimSuffix(filename, filepath.Ext(filename))
	}
	if _, err := s.GetUser(ctx, uploadedBy); err != nil {
		if errors.Is(err, ErrNotFound) {
			return Game{}, badRequest("Uploader does not exist")
		}
		return Game{}, err
	}

	if err := s.checkDiskSpace(0); err != nil { // the size of a streamed body is not known yet: keep the reserve
		return Game{}, err
	}
	tmp, err := os.CreateTemp(filepath.Join(s.dataDir, "tmp"), "rom-*")
	if err != nil {
		return Game{}, internal(err)
	}
	tmpName := tmp.Name()
	defer os.Remove(tmpName) // no effect after a successful rename
	h := sha256.New()
	size, err := io.Copy(io.MultiWriter(tmp, h), r)
	if err == nil {
		err = tmp.Sync() // data must be durable before the rename makes it the ROM
	}
	if cerr := tmp.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		return Game{}, internal(err)
	}
	if size == 0 {
		return Game{}, badRequest("ROM file is empty")
	}
	code, kind, ver := ReadGameIdentityVersion(tmpName, system)
	return s.commitStored(ctx, tmpName, romMeta{sha: hex.EncodeToString(h.Sum(nil)), size: size, filename: filename,
		title: title, system: system, addedBy: uploadedBy, code: code, kind: kind, version: ver})
}

// duplicateROM builds the conflict error for a ROM that is already in the library (with the existing game ID).
func (s *Service) duplicateROM(ctx context.Context, sha string) error {
	e := conflict("This ROM is already in the library")
	if g, err := s.GetGameByHash(ctx, sha); err == nil {
		e.ExistingGameID = g.ID
	}
	return e
}

// ListGames returns the whole library (all games, also local-only ones) sorted by title. The API lists
// ListGamesFor instead.
func (s *Service) ListGames(ctx context.Context) ([]Game, error) {
	return s.queryGames(ctx, ``)
}

// ListGamesFor returns the games visible to a user: games with a file stored on the server, and games for which the
// user registered a local file.
func (s *Service) ListGamesFor(ctx context.Context, userID string) ([]Game, error) {
	return s.queryGames(ctx, visibleWhere, userID)
}

// visibleWhere selects the games visible to the user bound to its single placeholder.
const visibleWhere = `WHERE EXISTS (SELECT 1 FROM game_roms r WHERE r.game_id = games.id AND r.stored = 1)
	OR EXISTS (SELECT 1 FROM game_roms r JOIN rom_owners o ON o.sha256 = r.sha256 WHERE r.game_id = games.id AND o.user_id = ?)`

func (s *Service) queryGames(ctx context.Context, where string, args ...any) ([]Game, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT `+gameCols+` FROM games `+where+` ORDER BY title COLLATE NOCASE, added_at`, args...)
	if err != nil {
		return nil, internal(err)
	}
	out := []Game{}
	for rows.Next() {
		g, err := scanGame(rows)
		if err != nil {
			rows.Close()
			return nil, internal(err)
		}
		out = append(out, g)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return nil, internal(err)
	}
	files, err := s.loadROMs(ctx, "")
	if err != nil {
		return nil, err
	}
	for i := range out {
		applyROMs(&out[i], files[out[i].ID])
	}
	return out, nil
}

// loadROMs returns the files of one game, or of all games when gameID is empty (primary file first is applied by
// applyROMs).
func (s *Service) loadROMs(ctx context.Context, gameID string) (map[string][]ROMFile, error) {
	q := `SELECT game_id, sha256, size, filename, stored, added_by, added_at FROM game_roms`
	var args []any
	if gameID != "" {
		q += ` WHERE game_id = ?`
		args = append(args, gameID)
	}
	rows, err := s.db.QueryContext(ctx, q+` ORDER BY added_at, sha256`, args...)
	if err != nil {
		return nil, internal(err)
	}
	defer rows.Close()
	out := map[string][]ROMFile{}
	for rows.Next() {
		var id string
		var f ROMFile
		var stored, added int64
		if err := rows.Scan(&id, &f.SHA256, &f.Size, &f.Filename, &stored, &f.AddedBy, &added); err != nil {
			return nil, internal(err)
		}
		f.Stored, f.AddedAt = stored != 0, time.Unix(added, 0).UTC()
		out[id] = append(out[id], f)
	}
	return out, rows.Err()
}

// applyROMs sets ROMs (primary file first), OnHub and the ROM* fields (a stored file if any, else the primary).
func applyROMs(g *Game, files []ROMFile) {
	sort.SliceStable(files, func(i, j int) bool {
		return files[i].SHA256 == g.ROMSHA256 && files[j].SHA256 != g.ROMSHA256
	})
	g.ROMs = files
	if g.ROMs == nil {
		g.ROMs = []ROMFile{}
	}
	for _, f := range files {
		if f.Stored {
			g.OnHub = true
			g.ROMSHA256, g.ROMSize, g.Filename = f.SHA256, f.Size, f.Filename
			return
		}
	}
}

// GetGame returns a game by ID (not filtered by visibility; for internal use and the app).
func (s *Service) GetGame(ctx context.Context, id string) (Game, error) {
	return s.oneGame(ctx, `id = ?`, id)
}

// GetGameFor returns a game by ID when it is visible to the user (else ErrNotFound).
func (s *Service) GetGameFor(ctx context.Context, userID, id string) (Game, error) {
	g, err := s.GetGame(ctx, id)
	if err != nil {
		return Game{}, err
	}
	if g.OnHub {
		return g, nil
	}
	var n int
	if err := s.db.QueryRowContext(ctx, `SELECT COUNT(*) FROM game_roms r JOIN rom_owners o ON o.sha256 = r.sha256
		WHERE r.game_id = ? AND o.user_id = ?`, id, userID).Scan(&n); err != nil {
		return Game{}, internal(err)
	}
	if n == 0 {
		return Game{}, ErrNotFound
	}
	return g, nil
}

// GetGameByHash returns the game that knows a ROM file with this SHA-256 (stored or only registered).
func (s *Service) GetGameByHash(ctx context.Context, sha string) (Game, error) {
	var id string
	err := s.db.QueryRowContext(ctx, `SELECT game_id FROM game_roms WHERE sha256 = ?`, sha).Scan(&id)
	if errors.Is(err, sql.ErrNoRows) {
		return Game{}, ErrNotFound
	}
	if err != nil {
		return Game{}, internal(err)
	}
	return s.GetGame(ctx, id)
}

func (s *Service) oneGame(ctx context.Context, where string, arg string) (Game, error) {
	g, err := scanGame(s.db.QueryRowContext(ctx, `SELECT `+gameCols+` FROM games WHERE `+where, arg))
	if errors.Is(err, sql.ErrNoRows) {
		return Game{}, ErrNotFound
	}
	if err != nil {
		return Game{}, internal(err)
	}
	files, err := s.loadROMs(ctx, g.ID)
	if err != nil {
		return Game{}, err
	}
	applyROMs(&g, files[g.ID])
	return g, nil
}

// OpenROM opens a ROM file stored on the server; the caller closes it. A file that is only registered as local is
// not found. The returned game describes that file.
func (s *Service) OpenROM(ctx context.Context, sha string) (*os.File, Game, error) {
	if !ValidSHA256(sha) {
		return nil, Game{}, ErrNotFound
	}
	var id string
	err := s.db.QueryRowContext(ctx, `SELECT game_id FROM game_roms WHERE sha256 = ? AND stored = 1`, sha).Scan(&id)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, Game{}, ErrNotFound
	}
	if err != nil {
		return nil, Game{}, internal(err)
	}
	g, err := s.GetGame(ctx, id)
	if err != nil {
		return nil, Game{}, err
	}
	for _, f := range g.ROMs {
		if f.SHA256 == sha {
			g.ROMSHA256, g.ROMSize, g.Filename, g.AddedAt = f.SHA256, f.Size, f.Filename, f.AddedAt
		}
	}
	f, err := os.Open(s.romPath(sha))
	if errors.Is(err, os.ErrNotExist) {
		return nil, Game{}, ErrNotFound
	}
	if err != nil {
		return nil, Game{}, internal(err)
	}
	return f, g, nil
}

// MaxGameTitle is the maximum length of a library title in bytes.
const MaxGameTitle = 200

// RenameGame changes the display title of a library entry. The ROM file, its hash and all saves stay as they
// are. The title must be valid UTF-8 and not empty after removing control characters; it is cut to MaxGameTitle.
func (s *Service) RenameGame(ctx context.Context, id, title string) (_ Game, err error) {
	defer s.publishOK(&err, TopicLibrary)
	if !utf8.ValidString(title) {
		return Game{}, badRequest("The title must be valid UTF-8 text")
	}
	title = cleanText(title, MaxGameTitle)
	if title == "" {
		return Game{}, badRequest("The title must not be empty")
	}
	g, err := s.GetGame(ctx, id)
	if err != nil {
		return Game{}, err
	}
	if _, err := s.db.ExecContext(ctx, `UPDATE games SET title = ? WHERE id = ?`, title, g.ID); err != nil {
		return Game{}, internal(err)
	}
	g.Title = title
	return g, nil
}

// DeleteGame removes the entry and all its ROM files stored on the server (registrations of local files go with the
// entry). Saves of the game are never touched (no foreign key, 0002_saves.sql): they stay and remain reachable on
// the Saves page.
func (s *Service) DeleteGame(ctx context.Context, id string) (err error) {
	defer s.publishOK(&err, TopicLibrary)
	g, err := s.GetGame(ctx, id)
	if err != nil {
		return err
	}
	if err := s.deleteGameRows(ctx, g); err != nil {
		return err
	}
	// The rows are gone; a blob that cannot be removed now (e.g. still streaming on Windows) is only logged.
	for _, f := range g.ROMs {
		if !f.Stored || !ValidSHA256(f.SHA256) {
			continue
		}
		if err := os.Remove(s.romPath(f.SHA256)); err != nil && !errors.Is(err, os.ErrNotExist) {
			slog.Warn("delete game: remove ROM file failed", "game_id", g.ID, "err", err)
		}
	}
	return nil
}

// deleteGameRows deletes the game and, when saves of it exist, records its identity and file hashes in
// removed_games so that adding the same ROM again re-uses the game id and the saves are linked to it again.
func (s *Service) deleteGameRows(ctx context.Context, g Game) error {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return internal(err)
	}
	defer tx.Rollback()
	var hasSaves int
	if err := tx.QueryRowContext(ctx, `SELECT
		EXISTS(SELECT 1 FROM save_slots WHERE game_id = ?1) OR EXISTS(SELECT 1 FROM save_history WHERE game_id = ?1) OR
		EXISTS(SELECT 1 FROM save_conflicts WHERE game_id = ?1)`, g.ID).Scan(&hasSaves); err != nil {
		return internal(err)
	}
	if hasSaves == 1 {
		if _, err := tx.ExecContext(ctx, `INSERT OR REPLACE INTO removed_games(game_id, system, game_code, game_code_kind, game_version, removed_at)
			SELECT id, system, game_code, game_code_kind, game_version, ? FROM games WHERE id = ?`, s.Now().Unix(), g.ID); err != nil {
			return internal(err)
		}
		if _, err := tx.ExecContext(ctx, `INSERT OR REPLACE INTO removed_game_roms(sha256, game_id)
			SELECT sha256, game_id FROM game_roms WHERE game_id = ?`, g.ID); err != nil {
			return internal(err)
		}
	}
	// Screenshots go with the game (rows first: they reference it; the files are removed after the commit).
	var shotUsers []string
	urows, err := tx.QueryContext(ctx, `SELECT DISTINCT user_id FROM screenshots WHERE game_id = ?`, g.ID)
	if err != nil {
		return internal(err)
	}
	for urows.Next() {
		var u string
		if err := urows.Scan(&u); err != nil {
			urows.Close()
			return internal(err)
		}
		shotUsers = append(shotUsers, u)
	}
	urows.Close()
	// Favorites and collection entries go with the game; play rows stay (the users' totals keep the time).
	for _, q := range []string{`DELETE FROM favorites WHERE game_id = ?`, `DELETE FROM collection_games WHERE game_id = ?`, `DELETE FROM screenshots WHERE game_id = ?`, `DELETE FROM game_variants WHERE game_id = ?`, `DELETE FROM games WHERE id = ?`} {
		if _, err := tx.ExecContext(ctx, q, g.ID); err != nil {
			return internal(err)
		}
	}
	if err := tx.Commit(); err != nil {
		return internal(err)
	}
	s.removeGameScreenshotFiles(shotUsers, g.ID)
	return nil
}

// StorageStats describes storage usage for the Library page.
type StorageStats struct {
	GameCount int
	ROMBytes  int64
	// FreeBytes: free space in the data directory (0 on non-Linux).
	FreeBytes uint64
}

// Storage returns storage usage (sum of rom_size, free space).
func (s *Service) Storage(ctx context.Context) (StorageStats, error) {
	var st StorageStats
	if err := s.db.QueryRowContext(ctx, `SELECT COUNT(*), COALESCE(SUM(size),0) FROM game_roms WHERE stored = 1`).Scan(&st.GameCount, &st.ROMBytes); err != nil {
		return StorageStats{}, internal(err)
	}
	st.FreeBytes = freeBytes(s.dataDir)
	return st, nil
}
