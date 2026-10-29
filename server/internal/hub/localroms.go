package hub

import (
	"context"
	"database/sql"
	"errors"
	"os"
	"path/filepath"
	"strings"

	"github.com/google/uuid"
)

// FeatureLocalROMsV1 is the handshake feature flag for local ROMs: register/unregister local files, resumable
// uploads and the per-file `roms`/`on_hub` fields of a game.
const FeatureLocalROMsV1 = "local_roms_v1"

// romMeta describes one ROM file that joins the library, with the identity read from its header.
type romMeta struct {
	sha, filename, title, system, addedBy string
	size                                  int64
	code, kind                            string
	version                               *int
	uploadID                              string // upload session removed in the same transaction ("" = none)
}

func nullInt(v *int) sql.NullInt64 {
	if v == nil {
		return sql.NullInt64{}
	}
	return sql.NullInt64{Int64: int64(*v), Valid: true}
}

// gameCodeKindFor returns the identity kind a system's game code has ("" = the system has no identity).
func gameCodeKindFor(system string) string {
	if r, ok := identityFor(system); ok {
		return r.kind
	}
	return ""
}

// validGameCode checks a client-supplied game code against the format of its kind.
func validGameCode(kind, code string) bool {
	switch kind {
	case GameCodeNDS, GameCodeGBA:
		if len(code) != 4 {
			return false
		}
		for i := 0; i < 4; i++ {
			if code[i] < 0x20 || code[i] > 0x7E {
				return false
			}
		}
		return true
	case GameTitleGB:
		// Header title: 1..16 printable ASCII characters, trimmed (the app shows other bytes as '?').
		return code != "" && len(code) <= 16 && printableASCII([]byte(code)) && code == strings.TrimSpace(code)
	case Game3DS:
		if len(code) != 16 {
			return false
		}
		for i := 0; i < 16; i++ {
			c := code[i]
			if !(c >= '0' && c <= '9' || c >= 'A' && c <= 'F') {
				return false
			}
		}
		return true
	}
	return false
}

// attachFile records the file in the library inside tx and returns the game it belongs to:
//  1. the SHA-256 is already known: that game (stored=1 is set when the file is stored now),
//  2. same identity (system, code, kind, version; code not empty): an extra file of that game,
//  3. otherwise a new game.
//
// owner (non-empty for a registration) becomes owner of the file. created reports a new game.
func (s *Service) attachFile(ctx context.Context, tx *sql.Tx, m romMeta, stored bool, owner string) (gameID string, created bool, err error) {
	now := s.Now().Unix()
	var isStored int
	err = tx.QueryRowContext(ctx, `SELECT game_id, stored FROM game_roms WHERE sha256 = ?`, m.sha).Scan(&gameID, &isStored)
	switch {
	case err == nil:
		if stored && isStored == 1 {
			return gameID, false, conflictWith("This ROM is already in the library", gameID)
		}
		if stored {
			if _, err := tx.ExecContext(ctx, `UPDATE game_roms SET stored = 1, size = ?, filename = ? WHERE sha256 = ?`, m.size, m.filename, m.sha); err != nil {
				return "", false, internal(err)
			}
		}
	case errors.Is(err, sql.ErrNoRows):
		err = tx.QueryRowContext(ctx, `SELECT id FROM games WHERE ? <> '' AND system = ? AND game_code = ? AND game_code_kind = ? AND game_version IS ?
			ORDER BY added_at, id LIMIT 1`, m.code, m.system, m.code, m.kind, nullInt(m.version)).Scan(&gameID)
		if err != nil && !errors.Is(err, sql.ErrNoRows) {
			return "", false, internal(err)
		}
		if errors.Is(err, sql.ErrNoRows) {
			gameID, created = uuid.NewString(), true
			// The same ROM again after the game was deleted: re-use the old id, so its saves are linked again.
			if old, err := revivedGameID(ctx, tx, m); err != nil {
				return "", false, err
			} else if old != "" {
				gameID = old
			}
			storedKind := m.kind
			if storedKind == "" {
				storedKind = gameCodeNone // identity looked for, nothing found: the start-up backfill does not retry
			}
			if _, err := tx.ExecContext(ctx, `INSERT INTO games(`+gameCols+`) VALUES (?,?,?,?,?,?,?,?,?,?,?)`,
				gameID, m.title, m.system, m.sha, m.size, m.filename, m.addedBy, now, m.code, storedKind, nullInt(m.version)); err != nil {
				return "", false, internal(err)
			}
		}
		st := 0
		if stored {
			st = 1
		}
		if _, err := tx.ExecContext(ctx, `INSERT INTO game_roms(sha256, game_id, size, filename, stored, added_by, added_at) VALUES (?,?,?,?,?,?,?)`,
			m.sha, gameID, m.size, m.filename, st, m.addedBy, now); err != nil {
			return "", false, internal(err)
		}
	default:
		return "", false, internal(err)
	}
	if m.code != "" { // fill an identity that was unknown until this file (for example a game registered by hash only)
		if _, err := tx.ExecContext(ctx, `UPDATE games SET game_code = ?, game_code_kind = ?, game_version = ?
			WHERE id = ? AND game_code = '' AND system = ?`, m.code, m.kind, nullInt(m.version), gameID, m.system); err != nil {
			return "", false, internal(err)
		}
	}
	if owner != "" {
		if _, err := tx.ExecContext(ctx, `INSERT OR IGNORE INTO rom_owners(sha256, user_id) VALUES (?, ?)`, m.sha, owner); err != nil {
			return "", false, internal(err)
		}
	}
	return gameID, created, nil
}

// revivedGameID returns the id of a removed game that still has saves and that this file belongs to: the same
// SHA-256, else the same identity (system, game code, kind and ROM version; code not empty). The tombstone is
// consumed. "" when there is none.
func revivedGameID(ctx context.Context, tx *sql.Tx, m romMeta) (string, error) {
	var id string
	err := tx.QueryRowContext(ctx, `SELECT game_id FROM removed_game_roms WHERE sha256 = ?`, m.sha).Scan(&id)
	if errors.Is(err, sql.ErrNoRows) && m.code != "" {
		err = tx.QueryRowContext(ctx, `SELECT game_id FROM removed_games WHERE system = ? AND game_code = ? AND game_code_kind = ? AND game_version IS ?
			ORDER BY removed_at, game_id LIMIT 1`, m.system, m.code, m.kind, nullInt(m.version)).Scan(&id)
	}
	if errors.Is(err, sql.ErrNoRows) {
		return "", nil
	}
	if err != nil {
		return "", internal(err)
	}
	if _, err := tx.ExecContext(ctx, `DELETE FROM removed_games WHERE game_id = ?`, id); err != nil { // cascades the hashes
		return "", internal(err)
	}
	return id, nil
}

func conflictWith(msg, existing string) *Error {
	e := conflict(msg)
	e.ExistingGameID = existing
	return e
}

// commitStored moves the finished file at tmpPath to roms/<sha[:2]>/<sha> and records it as a stored file (see
// attach). A SHA-256 that is already stored results in a conflict error naming the existing game. The temp file is
// consumed on success.
func (s *Service) commitStored(ctx context.Context, tmpPath string, m romMeta) (Game, error) {
	var known int
	if err := s.db.QueryRowContext(ctx, `SELECT COUNT(*) FROM game_roms WHERE sha256 = ? AND stored = 1`, m.sha).Scan(&known); err != nil {
		return Game{}, internal(err)
	}
	if known > 0 {
		return Game{}, s.duplicateROM(ctx, m.sha)
	}
	dst := s.romPath(m.sha)
	if err := os.MkdirAll(filepath.Dir(dst), 0o750); err != nil {
		return Game{}, internal(err)
	}
	if err := os.Chmod(tmpPath, 0o640); err != nil {
		return Game{}, internal(err)
	}
	if err := os.Rename(tmpPath, dst); err != nil {
		return Game{}, internal(err)
	}
	syncDir(filepath.Dir(dst))
	gameID, err := s.commitTx(ctx, m, true, "")
	if err != nil {
		if isUnique(err) || errors.Is(err, ErrConflict) { // parallel upload of the same ROM: the file belongs to the other entry
			return Game{}, s.duplicateROM(ctx, m.sha)
		}
		os.Remove(dst)
		return Game{}, err
	}
	return s.GetGame(ctx, gameID)
}

func (s *Service) commitTx(ctx context.Context, m romMeta, stored bool, owner string) (string, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return "", internal(err)
	}
	defer tx.Rollback()
	gameID, _, err := s.attachFile(ctx, tx, m, stored, owner)
	if err != nil {
		return "", err
	}
	if m.uploadID != "" {
		if _, err := tx.ExecContext(ctx, `DELETE FROM uploads WHERE id = ?`, m.uploadID); err != nil {
			return "", internal(err)
		}
	}
	if err := tx.Commit(); err != nil {
		return "", internal(err)
	}
	s.refreshPresenceGame(ctx, gameID) // a new owner or a stored file changes who may see the title
	return gameID, nil
}

// RegisterInput is a local file an app reports.
type RegisterInput struct {
	SHA256, Filename, System, Title, GameCode string
	Size                                      int64
	GameVersion                               *int
}

// RegisterROM registers a file that exists only on the caller's device. Match by SHA-256, else by identity, else a
// new local-only game is created. The user becomes owner of the file. created is true for a new game. The server never
// sees the file, so the identity is taken from the app; a code that does not fit the system's format is ignored
// (the game then matches by hash only).
func (s *Service) RegisterROM(ctx context.Context, userID string, in RegisterInput) (_ Game, created bool, err error) {
	defer s.publishOK(&err, TopicLibrary)
	if !ValidSHA256(in.SHA256) {
		return Game{}, false, badRequest("sha256 must be 64 lowercase hex characters")
	}
	filename := cleanText(filepath.Base(strings.ReplaceAll(in.Filename, `\`, "/")), 255)
	if filename == "" || filename == "." || filename == "/" {
		return Game{}, false, badRequest("File name missing")
	}
	if in.Size <= 0 || in.Size > MaxROMBytes {
		return Game{}, false, badRequest("size must be between 1 byte and the upload limit")
	}
	system, err := s.resolveSystem(ctx, in.System, filename)
	if err != nil {
		return Game{}, false, err
	}
	title := cleanText(in.Title, MaxGameTitle)
	if title == "" {
		title = strings.TrimSuffix(filename, filepath.Ext(filename))
	}
	m := romMeta{sha: in.SHA256, size: in.Size, filename: filename, title: title, system: system, addedBy: userID}
	if kind := gameCodeKindFor(system); kind != "" && validGameCode(kind, in.GameCode) {
		m.code, m.kind = in.GameCode, kind
		if in.GameVersion != nil && *in.GameVersion >= 0 && *in.GameVersion <= 0xFFFF {
			m.version = in.GameVersion
		}
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return Game{}, false, internal(err)
	}
	defer tx.Rollback()
	gameID, created, err := s.attachFile(ctx, tx, m, false, userID)
	if err != nil {
		return Game{}, false, err
	}
	if err := tx.Commit(); err != nil {
		return Game{}, false, internal(err)
	}
	s.refreshPresenceGame(ctx, gameID)
	g, err := s.GetGame(ctx, gameID)
	return g, created, err
}

// resolveSystem validates an explicit system or derives it from the file name.
func (s *Service) resolveSystem(ctx context.Context, system, filename string) (string, error) {
	systems, err := s.Systems(ctx)
	if err != nil {
		return "", err
	}
	if system == "" {
		sys, ok := systemForFilename(systems, filename)
		if !ok {
			return "", badRequest("System cannot be derived from the file extension")
		}
		return sys, nil
	}
	for _, sys := range systems {
		if sys.ID == system {
			return system, nil
		}
	}
	return "", badRequest("Unknown system")
}

// UnregisterROM removes the user's local registrations for a game. The game, its other files and its saves stay.
func (s *Service) UnregisterROM(ctx context.Context, userID, gameID string) (err error) {
	defer s.publishOK(&err, TopicLibrary)
	if _, err := s.GetGame(ctx, gameID); err != nil {
		return err
	}
	if _, err := s.db.ExecContext(ctx, `DELETE FROM rom_owners WHERE user_id = ? AND sha256 IN (SELECT sha256 FROM game_roms WHERE game_id = ?)`,
		userID, gameID); err != nil {
		return internal(err)
	}
	s.refreshPresenceGame(ctx, gameID) // the title may no longer be visible to this user
	return nil
}

// GameOwners returns, per game, the users who registered a file of it (for the admin library).
func (s *Service) GameOwners(ctx context.Context) (map[string][]string, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT DISTINCT r.game_id, o.user_id FROM rom_owners o JOIN game_roms r ON r.sha256 = o.sha256 ORDER BY r.game_id, o.user_id`)
	if err != nil {
		return nil, internal(err)
	}
	defer rows.Close()
	out := map[string][]string{}
	for rows.Next() {
		var g, u string
		if err := rows.Scan(&g, &u); err != nil {
			return nil, internal(err)
		}
		out[g] = append(out[g], u)
	}
	return out, rows.Err()
}
