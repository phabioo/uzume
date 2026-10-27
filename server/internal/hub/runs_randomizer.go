package hub

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"database/sql"
	"encoding/binary"
	"errors"
	"strconv"
	"unicode/utf8"
)

// Runs with a randomizer recipe (0.21, ADR 0031 D4). The run stores the recipe, the pinned engine version, the base
// dump pin (Gen 1 to 5) or the 3DS title, and a run seed that is never delivered. Each app builds its own variant from
// GET /runs/{run_id}/variant; no ROM or patch travels between apps or over the server.

// Randomizer modes of a run.
const (
	RandomizerOff  = "off"
	RandomizerEach = "each" // each person gets their own world: the seed is derived from the run seed and the user id
	RandomizerSame = "same" // everyone plays the run seed
)

// Variant kinds and build states of a participant.
const (
	VariantKindRun = "run"
	VariantKindOwn = "own"

	VariantNotBuilt = "not_built"
	VariantBuilding = "building"
	VariantReady    = "ready"
)

const maxPresetIDLen = 64

// RunRandomizer is the public part of a run's randomizer recipe, as the run object carries it. It never holds the
// settings or the seed.
type RunRandomizer struct {
	Mode        string `json:"mode"`
	PresetID    string `json:"preset_id,omitempty"`
	ToolID      string `json:"tool_id,omitempty"`
	ToolVersion string `json:"tool_version,omitempty"`
	AllowOwn    bool   `json:"allow_own"`
	BaseSHA256  string `json:"base_sha256,omitempty"`
	TitleID     string `json:"title_id,omitempty"`
}

// RunRandomizerInput is the randomizer recipe of a new run. Mode "" or "off" means no randomizer.
type RunRandomizerInput struct {
	Mode, PresetID, Settings, ToolID, ToolVersion string
	AllowOwn                                      bool
}

// JoinVariant is the variant a person joins a randomizer run with. Kind is "run" or "own"; VariantID names the own variant.
type JoinVariant struct {
	Kind, VariantID string
}

// runRecipe is a validated recipe ready to store, with the generated run seed.
type runRecipe struct {
	RunRandomizer
	settings string
	seed     int64
}

// RunVariant is what a joined participant gets from GET /runs/{run_id}/variant: the recipe to build their variant.
type RunVariant struct {
	Settings    string
	Seed        int64
	ToolID      string
	ToolVersion string
	BaseSHA256  string
	TitleID     string
	// Slot is the run's save slot.
	Slot string
	// Kind is the participant's variant kind; for "own" the recipe is the one of their own variant.
	Kind string
}

// DeriveRunSeed is the seed of a participant in a run with mode "each": the first 8 bytes of
// SHA-256(decimal(runSeed) + ":" + userID), read big-endian, with the top bit cleared, so the result is a
// non-negative int63. For mode "same" everybody uses runSeed itself. The app must not derive seeds on its own; it
// gets the result from the server.
//
// Test vector: DeriveRunSeed(123456789, "2f3c8a54-0b7d-4f0e-9a52-6d1e7c4b9a10") = 8849580158209368111 (see TestDeriveRunSeed).
func DeriveRunSeed(runSeed int64, userID string) int64 {
	sum := sha256.Sum256([]byte(strconv.FormatInt(runSeed, 10) + ":" + userID))
	return int64(binary.BigEndian.Uint64(sum[:8]) & 0x7FFFFFFFFFFFFFFF)
}

// newRunSeed returns a random non-negative int63 from crypto/rand.
func newRunSeed() (int64, error) {
	var b [8]byte
	if _, err := rand.Read(b[:]); err != nil {
		return 0, err
	}
	return int64(binary.BigEndian.Uint64(b[:]) & 0x7FFFFFFFFFFFFFFF), nil
}

// newRunRandomizer validates the recipe of a new run and pins the creator's game. nil = no randomizer. The tool version
// must be offered (the tool is offered and a platform zip is cached). The base dump is the game's SHA-256; a 3DS
// game pins its title id instead.
func (s *Service) newRunRandomizer(ctx context.Context, in *RunRandomizerInput, game Game) (*runRecipe, error) {
	if in == nil || in.Mode == "" || in.Mode == RandomizerOff {
		return nil, nil
	}
	if in.Mode != RandomizerEach && in.Mode != RandomizerSame {
		return nil, badRequest("randomizer.mode must be off, each or same")
	}
	if !validSettings(in.Settings) {
		return nil, badRequest("Randomizer settings must be 1 to %d printable ASCII characters", MaxVariantSettingsLen)
	}
	if n := utf8.RuneCountInString(in.PresetID); n > maxPresetIDLen || !validDisplayText(in.PresetID) {
		return nil, badRequest("preset_id is invalid")
	}
	toolID := in.ToolID
	if toolID == "" {
		toolID = ToolRandomizer
	}
	ok, err := s.ToolVersionOffered(ctx, toolID, in.ToolVersion)
	if err != nil {
		return nil, err
	}
	if !ok {
		return nil, badRequest("Tool version %s is not offered by this server", in.ToolVersion)
	}
	seed, err := newRunSeed()
	if err != nil {
		return nil, internal(err)
	}
	r := &runRecipe{settings: in.Settings, seed: seed}
	r.Mode, r.PresetID, r.ToolID, r.ToolVersion, r.AllowOwn = in.Mode, in.PresetID, toolID, in.ToolVersion, in.AllowOwn
	if game.GameCodeKind == Game3DS {
		r.TitleID = game.GameCode
	} else {
		r.BaseSHA256 = game.ROMSHA256
	}
	return r, nil
}

func insertRunRandomizer(ctx context.Context, q dbq, runID, creatorID string, r *runRecipe) error {
	allow := 0
	if r.AllowOwn {
		allow = 1
	}
	if _, err := q.ExecContext(ctx, `INSERT INTO run_randomizer(run_id, mode, preset_id, settings, tool_id, tool_version, allow_own, run_seed, base_sha256, title_id)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`, runID, r.Mode, r.PresetID, r.settings, r.ToolID, r.ToolVersion, allow, r.seed, r.BaseSHA256, r.TitleID); err != nil {
		return internal(err)
	}
	if _, err := q.ExecContext(ctx, `UPDATE run_participants SET variant_kind = 'run', variant_state = 'not_built' WHERE run_id = ? AND user_id = ?`, runID, creatorID); err != nil {
		return internal(err)
	}
	return nil
}

// loadRunRandomizers fills Run.Randomizer for the runs of one chunk (public fields only).
func loadRunRandomizers(ctx context.Context, q dbq, in string, args []any, byID map[string]*Run) error {
	rows, err := q.QueryContext(ctx, `SELECT run_id, mode, preset_id, tool_id, tool_version, allow_own, base_sha256, title_id FROM run_randomizer WHERE run_id IN `+in, args...)
	if err != nil {
		return internal(err)
	}
	defer rows.Close()
	for rows.Next() {
		var rid string
		var rz RunRandomizer
		var allow int
		if err := rows.Scan(&rid, &rz.Mode, &rz.PresetID, &rz.ToolID, &rz.ToolVersion, &allow, &rz.BaseSHA256, &rz.TitleID); err != nil {
			return internal(err)
		}
		rz.AllowOwn = allow == 1
		if r := byID[rid]; r != nil {
			r.Randomizer = rz
		}
	}
	return rows.Err()
}

// checkJoinVariant validates the variant choice of a joining person and returns the kind and own variant id to store
// ("" kind = the run is not randomized and nothing is stored).
func (s *Service) checkJoinVariant(ctx context.Context, q dbq, run Run, userID, gameID string, jv *JoinVariant) (kind, variantID string, err error) {
	if run.Randomizer.Mode == RandomizerOff {
		if jv != nil && jv.Kind != "" {
			return "", "", badRequest("This run has no randomizer; there is no variant to choose")
		}
		return "", "", nil
	}
	if jv == nil || jv.Kind == "" || jv.Kind == VariantKindRun {
		if jv != nil && jv.VariantID != "" {
			return "", "", badRequest("variant_id is only for kind own")
		}
		return VariantKindRun, "", nil
	}
	if jv.Kind != VariantKindOwn {
		return "", "", badRequest("variant.kind must be run or own")
	}
	if !run.Randomizer.AllowOwn {
		return "", "", &Error{Code: CodeOwnVariantsOff, Message: "This run does not allow own variants"}
	}
	if jv.VariantID == "" {
		return "", "", badRequest("variant_id is required for kind own")
	}
	v, err := loadVariant(ctx, q, jv.VariantID)
	if err != nil || v.UserID != userID || v.DeletedAt != nil {
		if err != nil && !errors.Is(err, ErrNotFound) {
			return "", "", err
		}
		return "", "", ErrNotFound
	}
	if v.GameID != gameID || v.ToolID != run.Randomizer.ToolID || v.ToolVersion != run.Randomizer.ToolVersion {
		return "", "", &Error{Code: CodeOwnVariantMismatch, Message: "The variant is for another game or another engine version than the run uses"}
	}
	return VariantKindOwn, v.ID, nil
}

func nullIfEmpty(v string) any {
	if v == "" {
		return nil
	}
	return v
}

// GetRunVariant returns the recipe a joined participant builds their variant from. 404 for non-participants, 403 for
// participants who have not joined, 409 for a run without randomizer. Mode "same" gives the run seed, "each" the seed
// derived for this person (DeriveRunSeed); a participant who joined with an own variant gets the recipe of that variant.
func (s *Service) GetRunVariant(ctx context.Context, userID, runID string) (RunVariant, error) {
	if err := joinedParticipant(ctx, s.db, runID, userID); err != nil {
		return RunVariant{}, err
	}
	var mode, settings, toolID, toolVersion, base, title string
	var seed int64
	err := s.db.QueryRowContext(ctx, `SELECT mode, settings, tool_id, tool_version, run_seed, base_sha256, title_id FROM run_randomizer WHERE run_id = ?`, runID).
		Scan(&mode, &settings, &toolID, &toolVersion, &seed, &base, &title)
	if errors.Is(err, sql.ErrNoRows) {
		return RunVariant{}, conflict("This run has no randomizer")
	}
	if err != nil {
		return RunVariant{}, internal(err)
	}
	out := RunVariant{Settings: settings, Seed: seed, ToolID: toolID, ToolVersion: toolVersion, BaseSHA256: base, TitleID: title,
		Slot: RunSaveSlot(runID), Kind: VariantKindRun}
	var kind, vid sql.NullString
	if err := s.db.QueryRowContext(ctx, `SELECT variant_kind, variant_id FROM run_participants WHERE run_id = ? AND user_id = ?`, runID, userID).Scan(&kind, &vid); err != nil {
		return RunVariant{}, internal(err)
	}
	if kind.String == VariantKindOwn {
		v, err := loadVariant(ctx, s.db, vid.String)
		if err != nil || v.UserID != userID || v.DeletedAt != nil {
			return RunVariant{}, &Error{Code: CodeOwnVariantMismatch, Message: "Your own variant for this run no longer exists"}
		}
		out.Settings, out.Seed, out.ToolID, out.ToolVersion, out.BaseSHA256, out.TitleID, out.Kind =
			v.Settings, v.Seed, v.ToolID, v.ToolVersion, v.BaseSHA256, v.TitleID, VariantKindOwn
		return out, nil
	}
	if mode == RandomizerEach {
		out.Seed = DeriveRunSeed(seed, userID)
	}
	return out, nil
}

// SetRunVariantState records how far the caller's own variant is ("not_built", "building", "ready") and pushes
// run_updated to the participants. Only for a joined participant of a randomized run that is not over.
func (s *Service) SetRunVariantState(ctx context.Context, userID, runID, state string) (_ Run, err error) {
	defer s.publishOK(&err, TopicRuns)
	if state != VariantNotBuilt && state != VariantBuilding && state != VariantReady {
		return Run{}, badRequest("state must be not_built, building or ready")
	}
	s.runs.mu.Lock()
	defer s.runs.mu.Unlock()
	run, err := loadRun(ctx, s.db, runID, true)
	if err != nil {
		return Run{}, err
	}
	p, ok := run.Participant(userID)
	if !ok {
		return Run{}, ErrNotFound
	}
	if p.Status != ParticipantJoined {
		return Run{}, &Error{Code: CodeForbidden, Message: "Join the run first"}
	}
	if run.Randomizer.Mode == RandomizerOff {
		return Run{}, conflict("This run has no randomizer")
	}
	if run.Finished() {
		return Run{}, conflict("The run is over")
	}
	if p.VariantState == nil || *p.VariantState != state {
		if _, err := s.db.ExecContext(ctx, `UPDATE run_participants SET variant_state = ? WHERE run_id = ? AND user_id = ?`, state, runID, userID); err != nil {
			return Run{}, internal(err)
		}
		if run, err = loadRun(ctx, s.db, runID, true); err != nil {
			return Run{}, err
		}
		s.pushRunUpdated(run)
	}
	return viewFor(run, userID), nil
}
