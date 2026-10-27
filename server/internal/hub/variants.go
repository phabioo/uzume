package hub

import (
	"context"
	"database/sql"
	"errors"
	"regexp"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/google/uuid"
)

// Game variants (0.21, ADR 0031 D1): a library game plus a recipe (tool and version, settings string, seed, the pinned
// base dump or 3DS title and update), played in an own save slot "var-<8 hex of the id>". The server stores recipes
// only: no ROM data, patches or spoiler logs. A variant belongs to its owner; nobody else sees its settings or seed.

// Limits of a variant.
const (
	MaxVariantNameLen     = 48
	MaxVariantSettingsLen = 4096
	maxVariantUpdateLen   = 32
)

var (
	sha256HexRe = regexp.MustCompile(`^[0-9a-f]{64}$`)
	titleIDRe   = regexp.MustCompile(`^[0-9A-Fa-f]{16}$`)
)

// Variant is a stored recipe.
type Variant struct {
	ID, UserID, GameID, Name, Slot string
	ToolID, ToolVersion, Settings  string
	Seed                           int64
	BaseSHA256, TitleID            string
	UpdateVersion                  string
	SpoilerLog                     bool
	RunID                          *string
	CreatedAt                      time.Time
	DeletedAt                      *time.Time
}

// VariantSlot is the save slot of a variant: "var-" plus the first 8 hex characters of the id.
func VariantSlot(id string) string {
	return "var-" + strings.ReplaceAll(strings.ToLower(id), "-", "")[:8]
}

const variantCols = `id, user_id, game_id, name, slot, tool_id, tool_version, settings, seed, base_sha256, title_id, update_version,
	spoiler_log, run_id, created_at, deleted_at`

func scanVariant(sc interface{ Scan(...any) error }) (Variant, error) {
	var v Variant
	var spoiler int
	var runID sql.NullString
	var created int64
	var deleted sql.NullInt64
	if err := sc.Scan(&v.ID, &v.UserID, &v.GameID, &v.Name, &v.Slot, &v.ToolID, &v.ToolVersion, &v.Settings, &v.Seed, &v.BaseSHA256,
		&v.TitleID, &v.UpdateVersion, &spoiler, &runID, &created, &deleted); err != nil {
		return Variant{}, err
	}
	v.SpoilerLog = spoiler == 1
	v.CreatedAt = unixTime(created)
	if runID.Valid {
		v.RunID = &runID.String
	}
	if deleted.Valid {
		t := unixTime(deleted.Int64)
		v.DeletedAt = &t
	}
	return v, nil
}

func loadVariant(ctx context.Context, q dbq, id string) (Variant, error) {
	v, err := scanVariant(q.QueryRowContext(ctx, `SELECT `+variantCols+` FROM game_variants WHERE id = ?`, id))
	if errors.Is(err, sql.ErrNoRows) {
		return Variant{}, ErrNotFound
	}
	if err != nil {
		return Variant{}, internal(err)
	}
	return v, nil
}

// ownedVariant returns the variant of the user; other people's variants do not exist for the caller (ErrNotFound).
func (s *Service) ownedVariant(ctx context.Context, userID, id string) (Variant, error) {
	if _, err := uuid.Parse(id); err != nil {
		return Variant{}, ErrNotFound
	}
	v, err := loadVariant(ctx, s.db, id)
	if err != nil {
		return Variant{}, err
	}
	if v.UserID != userID {
		return Variant{}, ErrNotFound
	}
	return v, nil
}

// ListVariants returns the variants of a user, newest first, optionally of one game. Deleted ones (recipe kept after
// "delete, keep saves") only with includeDeleted.
func (s *Service) ListVariants(ctx context.Context, userID, gameID string, includeDeleted bool) ([]Variant, error) {
	q := `SELECT ` + variantCols + ` FROM game_variants WHERE user_id = ?`
	args := []any{userID}
	if gameID != "" {
		q += ` AND game_id = ?`
		args = append(args, gameID)
	}
	if !includeDeleted {
		q += ` AND deleted_at IS NULL`
	}
	rows, err := s.db.QueryContext(ctx, q+` ORDER BY created_at DESC, id`, args...)
	if err != nil {
		return nil, internal(err)
	}
	defer rows.Close()
	out := []Variant{}
	for rows.Next() {
		v, err := scanVariant(rows)
		if err != nil {
			return nil, internal(err)
		}
		out = append(out, v)
	}
	return out, rows.Err()
}

// CreateVariantInput is the request to store a recipe.
type CreateVariantInput struct {
	UserID, GameID, Name           string
	ToolID, ToolVersion, Settings  string
	Seed                           int64
	BaseSHA256, TitleID, UpdateVer string
	SpoilerLog                     bool
}

// validSettings checks a settings string: 1 to 4096 printable ASCII characters.
func validSettings(v string) bool {
	if len(v) < 1 || len(v) > MaxVariantSettingsLen {
		return false
	}
	for i := 0; i < len(v); i++ {
		if v[i] < 0x20 || v[i] > 0x7e {
			return false
		}
	}
	return true
}

func validDisplayText(v string) bool {
	for _, r := range v {
		if r < 0x20 || r == 0x7f {
			return false
		}
	}
	return utf8.ValidString(v)
}

// CreateVariant stores a recipe for a game the user can see. The tool version must be in the catalog (offered or
// not, so old recipes stay valid); the name is unique per user and game among active variants (409 name_taken).
func (s *Service) CreateVariant(ctx context.Context, in CreateVariantInput) (_ Variant, err error) {
	defer s.publishOK(&err, TopicLibrary)
	name := strings.TrimSpace(in.Name)
	if n := utf8.RuneCountInString(name); n < 1 || n > MaxVariantNameLen || !validDisplayText(name) {
		return Variant{}, badRequest("Name must be 1 to %d characters long", MaxVariantNameLen)
	}
	if !validSettings(in.Settings) {
		return Variant{}, badRequest("Settings must be 1 to %d printable ASCII characters", MaxVariantSettingsLen)
	}
	if in.Seed < 0 {
		return Variant{}, badRequest("Seed must not be negative")
	}
	if _, ok := findToolDef(in.ToolID); !ok {
		return Variant{}, badRequest("Unknown tool")
	}
	if in.BaseSHA256 != "" && !sha256HexRe.MatchString(in.BaseSHA256) {
		return Variant{}, badRequest("base_sha256 must be 64 lowercase hex characters")
	}
	if in.TitleID != "" && !titleIDRe.MatchString(in.TitleID) {
		return Variant{}, badRequest("title_id must be 16 hex characters")
	}
	if len(in.UpdateVer) > maxVariantUpdateLen || !validDisplayText(in.UpdateVer) {
		return Variant{}, badRequest("update_version is invalid")
	}
	if _, err := s.GetGameFor(ctx, in.UserID, in.GameID); err != nil {
		return Variant{}, err
	}
	s.tools.usageMu.RLock() // against RemoveToolVersion between the catalog check and the insert
	defer s.tools.usageMu.RUnlock()
	known, err := s.ToolVersionKnown(ctx, in.ToolID, in.ToolVersion)
	if err != nil {
		return Variant{}, err
	}
	if !known {
		return Variant{}, badRequest("Tool version %s is not in the catalog of this server", in.ToolVersion)
	}
	var taken int
	if err := s.db.QueryRowContext(ctx, `SELECT COUNT(*) FROM game_variants WHERE user_id = ? AND game_id = ? AND deleted_at IS NULL
		AND name = ? COLLATE NOCASE`, in.UserID, in.GameID, name).Scan(&taken); err != nil {
		return Variant{}, internal(err)
	}
	if taken > 0 {
		return Variant{}, &Error{Code: CodeNameTaken, Message: "You already have a variant with this name for this game"}
	}
	spoiler := 0
	if in.SpoilerLog {
		spoiler = 1
	}
	now := s.now().UTC()
	for attempt := 0; attempt < 5; attempt++ { // the slot has 8 hex characters: retry the (unlikely) collision
		id := uuid.NewString()
		_, err := s.db.ExecContext(ctx, `INSERT INTO game_variants(id, user_id, game_id, name, slot, tool_id, tool_version, settings, seed,
			base_sha256, title_id, update_version, spoiler_log, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
			id, in.UserID, in.GameID, name, VariantSlot(id), in.ToolID, in.ToolVersion, in.Settings, in.Seed, in.BaseSHA256,
			strings.ToUpper(in.TitleID), in.UpdateVer, spoiler, now.Unix())
		if err == nil {
			return loadVariant(ctx, s.db, id)
		}
		if !strings.Contains(err.Error(), "UNIQUE") {
			return Variant{}, internal(err)
		}
		if err := s.db.QueryRowContext(ctx, `SELECT COUNT(*) FROM game_variants WHERE user_id = ? AND game_id = ? AND deleted_at IS NULL
			AND name = ? COLLATE NOCASE`, in.UserID, in.GameID, name).Scan(&taken); err == nil && taken > 0 {
			return Variant{}, &Error{Code: CodeNameTaken, Message: "You already have a variant with this name for this game"}
		}
	}
	return Variant{}, internal(errors.New("no free variant slot"))
}

// RenameVariant changes the name of an active variant of the user (the slot and the recipe stay).
func (s *Service) RenameVariant(ctx context.Context, userID, id, name string) (_ Variant, err error) {
	defer s.publishOK(&err, TopicLibrary)
	name = strings.TrimSpace(name)
	if n := utf8.RuneCountInString(name); n < 1 || n > MaxVariantNameLen || !validDisplayText(name) {
		return Variant{}, badRequest("Name must be 1 to %d characters long", MaxVariantNameLen)
	}
	v, err := s.ownedVariant(ctx, userID, id)
	if err != nil {
		return Variant{}, err
	}
	if v.DeletedAt != nil {
		return Variant{}, ErrNotFound
	}
	var taken int
	if err := s.db.QueryRowContext(ctx, `SELECT COUNT(*) FROM game_variants WHERE user_id = ? AND game_id = ? AND deleted_at IS NULL
		AND name = ? COLLATE NOCASE AND id <> ?`, userID, v.GameID, name, id).Scan(&taken); err != nil {
		return Variant{}, internal(err)
	}
	if taken > 0 {
		return Variant{}, &Error{Code: CodeNameTaken, Message: "You already have a variant with this name for this game"}
	}
	if _, err := s.db.ExecContext(ctx, `UPDATE game_variants SET name = ? WHERE id = ?`, name, id); err != nil {
		return Variant{}, internal(err)
	}
	return loadVariant(ctx, s.db, id)
}

// DeleteVariant removes a variant. deleteSaves removes the user's save slot of the variant through the save code
// (the same rules as deleting a slot: refused while a Session of the game runs or a conflict is open, nothing is
// deleted then) and the recipe row for good. Without it the saves stay and the recipe is kept marked deleted
// ("Build it again"); deleting an already deleted variant without deleteSaves does nothing.
func (s *Service) DeleteVariant(ctx context.Context, userID, deviceID, id string, deleteSaves bool) (err error) {
	defer s.publishOK(&err, TopicLibrary)
	v, err := s.ownedVariant(ctx, userID, id)
	if err != nil {
		return err
	}
	if !deleteSaves {
		if v.DeletedAt != nil {
			return nil
		}
		if _, err := s.db.ExecContext(ctx, `UPDATE game_variants SET deleted_at = ? WHERE id = ?`, s.now().Unix(), id); err != nil {
			return internal(err)
		}
		return nil
	}
	cp, err := loadCheckpoint(ctx, s.db, userID, v.GameID, v.Slot)
	switch {
	case err == nil:
		if err := s.DeleteSaveSlot(ctx, DeleteInput{UserID: userID, DeviceID: deviceID, GameID: v.GameID, Slot: v.Slot, ConfirmRevision: cp.Revision}); err != nil &&
			!errors.Is(err, ErrNotFound) {
			return err
		}
	case errors.Is(err, ErrNotFound): // never saved
	default:
		return err
	}
	if _, err := s.db.ExecContext(ctx, `DELETE FROM game_variants WHERE id = ?`, id); err != nil {
		return internal(err)
	}
	return nil
}
