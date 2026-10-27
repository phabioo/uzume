package hub_test

import (
	"encoding/json"
	"errors"
	"strings"
	"testing"

	"github.com/phabioo/uzume/server/internal/hub"
	"github.com/phabioo/uzume/server/internal/hub/hubtest"
)

type rzEnv struct {
	svc         *hub.Service
	admin, anna hub.User
	bob         hub.User
	game, game2 hub.Game
}

func newRZEnv(t *testing.T) *rzEnv {
	t.Helper()
	src := hubtest.NewToolSource(t)
	src.AddRelease(t, "1.6.2", false, allToolPlatforms...)
	src.AddRelease(t, "1.6.1", false, "windows-x64")
	svc := toolHub(t, src)
	admin, g := variantFixture(t, svc, src)
	if _, err := svc.InstallToolVersion(ctx, hub.ToolRandomizer, "1.6.1"); err != nil {
		t.Fatal(err)
	}
	if _, err := svc.SetToolOffered(ctx, hub.ToolRandomizer, true); err != nil {
		t.Fatal(err)
	}
	e := &rzEnv{svc: svc, admin: admin, game: g}
	var err error
	if e.anna, err = svc.CreateUser(ctx, "anna", "Anna"); err != nil {
		t.Fatal(err)
	}
	if e.bob, err = svc.CreateUser(ctx, "bob", "Bob"); err != nil {
		t.Fatal(err)
	}
	if e.game2, err = svc.AddROM(ctx, strings.NewReader("another-homebrew-rom"), "other.nds", "Other", "", admin.ID); err != nil {
		t.Fatal(err)
	}
	return e
}

func (e *rzEnv) create(t *testing.T, mode string, allowOwn bool, invite ...string) hub.Run {
	t.Helper()
	run, err := e.svc.CreateRun(ctx, hub.CreateRunInput{UserID: e.admin.ID, Name: "Randomized", GameID: e.game.ID, InviteUserIDs: invite,
		Randomizer: &hub.RunRandomizerInput{Mode: mode, PresetID: "chaos", Settings: "428secretsettings", ToolVersion: "1.6.2", AllowOwn: allowOwn}})
	if err != nil {
		t.Fatal(err)
	}
	return run
}

func TestRunRandomizerRecipeAndViews(t *testing.T) {
	e := newRZEnv(t)
	run := e.create(t, "each", true, e.anna.ID)
	rz := run.Randomizer
	if rz.Mode != "each" || rz.PresetID != "chaos" || rz.ToolID != "randomizer" || rz.ToolVersion != "1.6.2" || !rz.AllowOwn ||
		rz.BaseSHA256 != e.game.ROMSHA256 || rz.TitleID != "" {
		t.Fatalf("%+v", rz)
	}
	c, _ := run.Participant(e.admin.ID)
	if c.VariantKind == nil || *c.VariantKind != "run" || c.VariantState == nil || *c.VariantState != "not_built" {
		t.Fatalf("creator %+v", c)
	}
	if a, _ := run.Participant(e.anna.ID); a.VariantKind != nil || a.VariantState != nil {
		t.Fatalf("invited has no variant yet: %+v", a)
	}
	// Neither settings nor seed appear anywhere in the run as delivered (REST, pushes, exports).
	for _, r := range []hub.Run{run, mustGetRun(t, e.svc, e.admin.ID, run.ID), mustGetRun(t, e.svc, e.anna.ID, run.ID)} {
		b, _ := jsonOf(r)
		if strings.Contains(b, "secretsettings") || strings.Contains(b, "seed") {
			t.Fatalf("leak: %s", b)
		}
	}
	// An ordinary run has mode off and no variant fields.
	plain, err := e.svc.CreateRun(ctx, hub.CreateRunInput{UserID: e.admin.ID, Name: "Plain", GameID: e.game.ID})
	if err != nil || plain.Randomizer.Mode != "off" || plain.Randomizer.ToolVersion != "" {
		t.Fatalf("%+v %v", plain.Randomizer, err)
	}
	if p, _ := plain.Participant(e.admin.ID); p.VariantKind != nil || p.VariantState != nil {
		t.Fatalf("%+v", p)
	}
	if off, err := e.svc.CreateRun(ctx, hub.CreateRunInput{UserID: e.admin.ID, Name: "Off", GameID: e.game.ID,
		Randomizer: &hub.RunRandomizerInput{Mode: "off"}}); err != nil || off.Randomizer.Mode != "off" {
		t.Fatalf("%v", err)
	}
	if _, err := e.svc.GetRunVariant(ctx, e.admin.ID, plain.ID); !errors.Is(err, hub.ErrConflict) {
		t.Fatalf("mode off: %v", err)
	}
}

func TestRunRandomizerValidation(t *testing.T) {
	e := newRZEnv(t)
	mk := func(in hub.RunRandomizerInput) error {
		_, err := e.svc.CreateRun(ctx, hub.CreateRunInput{UserID: e.admin.ID, Name: "R", GameID: e.game.ID, Randomizer: &in})
		return err
	}
	ok := hub.RunRandomizerInput{Mode: "same", Settings: "428abc", ToolVersion: "1.6.2"}
	if err := mk(ok); err != nil {
		t.Fatal(err)
	}
	for name, mod := range map[string]func(*hub.RunRandomizerInput){
		"unknown mode":     func(in *hub.RunRandomizerInput) { in.Mode = "all" },
		"no settings":      func(in *hub.RunRandomizerInput) { in.Settings = "" },
		"long settings":    func(in *hub.RunRandomizerInput) { in.Settings = strings.Repeat("s", 4097) },
		"unknown version":  func(in *hub.RunRandomizerInput) { in.ToolVersion = "9.9.9" },
		"no version":       func(in *hub.RunRandomizerInput) { in.ToolVersion = "" },
		"unknown tool":     func(in *hub.RunRandomizerInput) { in.ToolID = "other" },
		"long preset":      func(in *hub.RunRandomizerInput) { in.PresetID = strings.Repeat("p", 65) },
		"non-ascii config": func(in *hub.RunRandomizerInput) { in.Settings = "ü" },
	} {
		in := ok
		mod(&in)
		if err := mk(in); !errors.Is(err, hub.ErrBadRequest) {
			t.Errorf("%s: %v", name, err)
		}
	}
	// 1.6.1 is cached for one platform only: that is enough to be offered.
	in := ok
	in.ToolVersion = "1.6.1"
	if err := mk(in); err != nil {
		t.Fatal(err)
	}
	// Not offered: refused.
	if _, err := e.svc.SetToolOffered(ctx, hub.ToolRandomizer, false); err != nil {
		t.Fatal(err)
	}
	if err := mk(ok); !errors.Is(err, hub.ErrBadRequest) {
		t.Fatalf("not offered: %v", err)
	}
}

func TestRunVariantSeedsAndAccess(t *testing.T) {
	e := newRZEnv(t)
	run := e.create(t, "each", false, e.anna.ID, e.bob.ID)
	if _, err := e.svc.JoinRun(ctx, e.anna.ID, run.ID, e.game.ID); err != nil {
		t.Fatal(err)
	}
	// Invited but not joined: no recipe. Strangers do not see the run at all.
	if _, err := e.svc.GetRunVariant(ctx, e.bob.ID, run.ID); !errors.Is(err, hub.ErrForbidden) {
		t.Fatalf("invited: %v", err)
	}
	stranger, _ := e.svc.CreateUser(ctx, "carol", "Carol")
	if _, err := e.svc.GetRunVariant(ctx, stranger.ID, run.ID); !errors.Is(err, hub.ErrNotFound) {
		t.Fatalf("stranger: %v", err)
	}
	va, err := e.svc.GetRunVariant(ctx, e.admin.ID, run.ID)
	if err != nil {
		t.Fatal(err)
	}
	vb, _ := e.svc.GetRunVariant(ctx, e.anna.ID, run.ID)
	if va.Settings != "428secretsettings" || va.ToolID != "randomizer" || va.ToolVersion != "1.6.2" || va.BaseSHA256 != e.game.ROMSHA256 ||
		va.Slot != hub.RunSaveSlot(run.ID) || va.Kind != "run" || va.Seed < 0 || va.Seed == vb.Seed || vb.Settings != va.Settings {
		t.Fatalf("%+v %+v", va, vb)
	}
	// Mode each: every person gets DeriveRunSeed(run seed, user id); the run seed itself is never delivered.
	runSeed, err := hub.RunSeedForTest(e.svc, run.ID)
	if err != nil || runSeed < 0 {
		t.Fatalf("%d %v", runSeed, err)
	}
	if va.Seed != hub.DeriveRunSeed(runSeed, e.admin.ID) || vb.Seed != hub.DeriveRunSeed(runSeed, e.anna.ID) || va.Seed == runSeed || vb.Seed == runSeed {
		t.Fatalf("each: %d %d run seed %d", va.Seed, vb.Seed, runSeed)
	}
	again, _ := e.svc.GetRunVariant(ctx, e.admin.ID, run.ID)
	if again.Seed != va.Seed {
		t.Fatal("seed must be stable")
	}

	// Mode same: everybody gets the run seed.
	same := e.create(t, "same", false, e.anna.ID)
	if _, err := e.svc.JoinRun(ctx, e.anna.ID, same.ID, e.game.ID); err != nil {
		t.Fatal(err)
	}
	sa, _ := e.svc.GetRunVariant(ctx, e.admin.ID, same.ID)
	sb, _ := e.svc.GetRunVariant(ctx, e.anna.ID, same.ID)
	sameSeed, _ := hub.RunSeedForTest(e.svc, same.ID)
	if sa.Seed != sb.Seed || sa.Seed != sameSeed || sa.Seed < 0 {
		t.Fatalf("same world: %d %d run seed %d", sa.Seed, sb.Seed, sameSeed)
	}
	if sameSeed == runSeed {
		t.Fatal("every run has its own seed")
	}
}

func TestRunJoinVariantRules(t *testing.T) {
	e := newRZEnv(t)
	closed := e.create(t, "same", false, e.anna.ID)
	open := e.create(t, "same", true, e.anna.ID, e.bob.ID)
	plain, _ := e.svc.CreateRun(ctx, hub.CreateRunInput{UserID: e.admin.ID, Name: "Plain", GameID: e.game.ID, InviteUserIDs: []string{e.anna.ID}})

	own, err := e.svc.CreateVariant(ctx, hub.CreateVariantInput{UserID: e.anna.ID, GameID: e.game.ID, Name: "Mine", ToolID: "randomizer", ToolVersion: "1.6.2", Settings: "428own", Seed: 7})
	if err != nil {
		t.Fatal(err)
	}
	oldVer, _ := e.svc.CreateVariant(ctx, hub.CreateVariantInput{UserID: e.anna.ID, GameID: e.game.ID, Name: "Old", ToolID: "randomizer", ToolVersion: "1.6.1", Settings: "428own", Seed: 8})
	otherGame, _ := e.svc.CreateVariant(ctx, hub.CreateVariantInput{UserID: e.anna.ID, GameID: e.game2.ID, Name: "Elsewhere", ToolID: "randomizer", ToolVersion: "1.6.2", Settings: "428own", Seed: 9})
	bobs, _ := e.svc.CreateVariant(ctx, hub.CreateVariantInput{UserID: e.bob.ID, GameID: e.game.ID, Name: "Bobs", ToolID: "randomizer", ToolVersion: "1.6.2", Settings: "428own", Seed: 10})

	joinOwn := func(runID string, u hub.User, id string) error {
		_, err := e.svc.JoinRunWith(ctx, u.ID, runID, e.game.ID, &hub.JoinVariant{Kind: "own", VariantID: id})
		return err
	}
	// The run does not allow own variants.
	if err := joinOwn(closed.ID, e.anna, own.ID); !errors.Is(err, &hub.Error{Code: hub.CodeOwnVariantsOff}) {
		t.Fatalf("closed: %v", err)
	}
	// Wrong game or wrong engine version: mismatch; someone else's or unknown variant: not found.
	if err := joinOwn(open.ID, e.anna, oldVer.ID); !errors.Is(err, &hub.Error{Code: hub.CodeOwnVariantMismatch}) {
		t.Fatalf("version: %v", err)
	}
	if err := joinOwn(open.ID, e.anna, otherGame.ID); !errors.Is(err, &hub.Error{Code: hub.CodeOwnVariantMismatch}) {
		t.Fatalf("game: %v", err)
	}
	if err := joinOwn(open.ID, e.anna, bobs.ID); !errors.Is(err, hub.ErrNotFound) {
		t.Fatalf("foreign: %v", err)
	}
	if err := joinOwn(open.ID, e.anna, "00000000-0000-4000-8000-000000000000"); !errors.Is(err, hub.ErrNotFound) {
		t.Fatalf("unknown: %v", err)
	}
	if err := joinOwn(open.ID, e.anna, ""); !errors.Is(err, hub.ErrBadRequest) {
		t.Fatalf("no id: %v", err)
	}
	if got := mustGetRun(t, e.svc, e.anna.ID, open.ID); mustPart(t, got, e.anna.ID).Status != "invited" {
		t.Fatal("a refused join must not join")
	}
	// A deleted variant cannot join.
	gone, _ := e.svc.CreateVariant(ctx, hub.CreateVariantInput{UserID: e.anna.ID, GameID: e.game.ID, Name: "Gone", ToolID: "randomizer", ToolVersion: "1.6.2", Settings: "428own", Seed: 1})
	if err := e.svc.DeleteVariant(ctx, e.anna.ID, "", gone.ID, false); err != nil {
		t.Fatal(err)
	}
	if err := joinOwn(open.ID, e.anna, gone.ID); !errors.Is(err, hub.ErrNotFound) {
		t.Fatalf("deleted: %v", err)
	}
	// Variant choices in a run without randomizer: refused; kind run is the default everywhere.
	if _, err := e.svc.JoinRunWith(ctx, e.anna.ID, plain.ID, e.game.ID, &hub.JoinVariant{Kind: "run"}); !errors.Is(err, hub.ErrBadRequest) {
		t.Fatalf("plain: %v", err)
	}
	if _, err := e.svc.JoinRunWith(ctx, e.anna.ID, open.ID, e.game.ID, &hub.JoinVariant{Kind: "weird"}); !errors.Is(err, hub.ErrBadRequest) {
		t.Fatalf("kind: %v", err)
	}
	// The valid joins.
	if err := joinOwn(open.ID, e.anna, own.ID); err != nil {
		t.Fatal(err)
	}
	if _, err := e.svc.JoinRunWith(ctx, e.bob.ID, open.ID, e.game.ID, &hub.JoinVariant{Kind: "run"}); err != nil {
		t.Fatal(err)
	}
	if _, err := e.svc.JoinRun(ctx, e.anna.ID, closed.ID, e.game.ID); err != nil {
		t.Fatal(err)
	}
	run := mustGetRun(t, e.svc, e.admin.ID, open.ID)
	pa, pb := mustPart(t, run, e.anna.ID), mustPart(t, run, e.bob.ID)
	if *pa.VariantKind != "own" || *pa.VariantState != "not_built" || *pb.VariantKind != "run" || *pb.VariantState != "not_built" {
		t.Fatalf("%+v %+v", pa, pb)
	}
	if b, _ := jsonOf(run); strings.Contains(b, own.ID) {
		t.Fatalf("the own variant id is private: %s", b)
	}
	if p := mustPart(t, mustGetRun(t, e.svc, e.anna.ID, closed.ID), e.anna.ID); *p.VariantKind != "run" {
		t.Fatalf("%+v", p)
	}
	// The own variant's recipe comes from GET /runs/{id}/variant, in the run slot.
	v, err := e.svc.GetRunVariant(ctx, e.anna.ID, open.ID)
	if err != nil || v.Kind != "own" || v.Settings != "428own" || v.Seed != 7 || v.Slot != hub.RunSaveSlot(open.ID) {
		t.Fatalf("%+v %v", v, err)
	}
	// Joining again with the same game is a no-op.
	if _, err := e.svc.JoinRunWith(ctx, e.bob.ID, open.ID, e.game.ID, &hub.JoinVariant{Kind: "run"}); err != nil {
		t.Fatal(err)
	}
}

func TestRunVariantState(t *testing.T) {
	e := newRZEnv(t)
	run := e.create(t, "each", false, e.anna.ID)
	plain, _ := e.svc.CreateRun(ctx, hub.CreateRunInput{UserID: e.admin.ID, Name: "Plain", GameID: e.game.ID})

	r, err := e.svc.SetRunVariantState(ctx, e.admin.ID, run.ID, "building")
	if err != nil || *mustPart(t, r, e.admin.ID).VariantState != "building" {
		t.Fatalf("%v", err)
	}
	if _, err := e.svc.SetRunVariantState(ctx, e.admin.ID, run.ID, "ready"); err != nil {
		t.Fatal(err)
	}
	if _, err := e.svc.SetRunVariantState(ctx, e.admin.ID, run.ID, "done"); !errors.Is(err, hub.ErrBadRequest) {
		t.Fatalf("%v", err)
	}
	if _, err := e.svc.SetRunVariantState(ctx, e.anna.ID, run.ID, "ready"); !errors.Is(err, hub.ErrForbidden) {
		t.Fatalf("not joined: %v", err)
	}
	stranger, _ := e.svc.CreateUser(ctx, "carol", "Carol")
	if _, err := e.svc.SetRunVariantState(ctx, stranger.ID, run.ID, "ready"); !errors.Is(err, hub.ErrNotFound) {
		t.Fatalf("stranger: %v", err)
	}
	if _, err := e.svc.SetRunVariantState(ctx, e.admin.ID, plain.ID, "ready"); !errors.Is(err, hub.ErrConflict) {
		t.Fatalf("mode off: %v", err)
	}
	// Only the person themself changes their state; others see it in the run.
	if _, err := e.svc.JoinRun(ctx, e.anna.ID, run.ID, e.game.ID); err != nil {
		t.Fatal(err)
	}
	got := mustGetRun(t, e.svc, e.anna.ID, run.ID)
	if *mustPart(t, got, e.admin.ID).VariantState != "ready" || *mustPart(t, got, e.anna.ID).VariantState != "not_built" {
		t.Fatalf("%+v", got.Participants)
	}
	if _, err := e.svc.SetRunStatus(ctx, e.admin.ID, run.ID, "abandoned"); err != nil {
		t.Fatal(err)
	}
	if _, err := e.svc.SetRunVariantState(ctx, e.admin.ID, run.ID, "building"); !errors.Is(err, hub.ErrConflict) {
		t.Fatalf("over: %v", err)
	}
}

func mustGetRun(t *testing.T, svc *hub.Service, userID, runID string) hub.Run {
	t.Helper()
	r, err := svc.GetRun(ctx, userID, runID)
	if err != nil {
		t.Fatal(err)
	}
	return r
}

func mustPart(t *testing.T, r hub.Run, userID string) hub.RunParticipant {
	t.Helper()
	p, ok := r.Participant(userID)
	if !ok {
		t.Fatalf("no participant %s", userID)
	}
	return p
}

func jsonOf(v any) (string, error) {
	b, err := json.Marshal(v)
	return string(b), err
}
