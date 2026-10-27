package hub_test

import (
	"bytes"
	"errors"
	"strings"
	"testing"

	"github.com/phabioo/uzume/server/internal/hub"
	"github.com/phabioo/uzume/server/internal/hub/hubtest"
)

// variantFixture syncs and installs randomizer 1.6.2 from the fake source and adds an admin with a game.
func variantFixture(t *testing.T, svc *hub.Service, src *hubtest.ToolSource) (hub.User, hub.Game) {
	t.Helper()
	admin, err := svc.CreateAdmin(ctx, "admin", "secret-12345")
	if err != nil {
		t.Fatal(err)
	}
	g, err := svc.AddROM(ctx, strings.NewReader("homebrew-rom-dummy"), "demo.nds", "Demo", "", admin.ID)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := svc.SyncTool(ctx, hub.ToolRandomizer); err != nil {
		t.Fatal(err)
	}
	if _, err := svc.InstallToolVersion(ctx, hub.ToolRandomizer, "1.6.2"); err != nil {
		t.Fatal(err)
	}
	return admin, g
}

func variantHub(t *testing.T) (*hub.Service, hub.User, hub.Game) {
	t.Helper()
	src := hubtest.NewToolSource(t)
	src.AddRelease(t, "1.6.2", false, allToolPlatforms...)
	svc := toolHub(t, src)
	admin, g := variantFixture(t, svc, src)
	return svc, admin, g
}

func okVariant(admin hub.User, g hub.Game, name string) hub.CreateVariantInput {
	return hub.CreateVariantInput{UserID: admin.ID, GameID: g.ID, Name: name, ToolID: "randomizer", ToolVersion: "1.6.2", Settings: "428abcdef", Seed: 42,
		BaseSHA256: g.ROMSHA256}
}

func TestDeriveRunSeed(t *testing.T) {
	const user = "2f3c8a54-0b7d-4f0e-9a52-6d1e7c4b9a10"
	if got := hub.DeriveRunSeed(123456789, user); got != 8849580158209368111 {
		t.Fatalf("test vector: %d", got)
	}
	// Different people and different run seeds give different, non-negative seeds.
	a, b, c := hub.DeriveRunSeed(1, "u1"), hub.DeriveRunSeed(1, "u2"), hub.DeriveRunSeed(2, "u1")
	if a == b || a == c || a < 0 || b < 0 || c < 0 {
		t.Fatalf("%d %d %d", a, b, c)
	}
	if hub.DeriveRunSeed(1, "u1") != a {
		t.Fatal("not deterministic")
	}
}

func TestVariantCreateValidationAndNames(t *testing.T) {
	svc, admin, g := variantHub(t)
	anna, _ := svc.CreateUser(ctx, "anna", "Anna")

	v, err := svc.CreateVariant(ctx, okVariant(admin, g, "  Nuzlocke friendly "))
	if err != nil {
		t.Fatal(err)
	}
	if v.Name != "Nuzlocke friendly" || !strings.HasPrefix(v.Slot, "var-") || len(v.Slot) != 12 || !hub.ValidSlotName(v.Slot) ||
		v.Slot != "var-"+strings.ReplaceAll(v.ID, "-", "")[:8] || v.RunID != nil || v.Seed != 42 || v.DeletedAt != nil || v.BaseSHA256 != g.ROMSHA256 {
		t.Fatalf("%+v", v)
	}
	// The name is unique per user and game, ignoring case; other people may use it.
	if _, err := svc.CreateVariant(ctx, okVariant(admin, g, "nuzlocke FRIENDLY")); !errors.Is(err, &hub.Error{Code: hub.CodeNameTaken}) {
		t.Fatalf("%v", err)
	}
	if _, err := svc.CreateVariant(ctx, func() hub.CreateVariantInput { in := okVariant(anna, g, "Nuzlocke friendly"); return in }()); err != nil {
		t.Fatalf("another user: %v", err)
	}
	bad := map[string]func(*hub.CreateVariantInput){
		"empty name":       func(in *hub.CreateVariantInput) { in.Name = "  " },
		"long name":        func(in *hub.CreateVariantInput) { in.Name = strings.Repeat("n", 49) },
		"control in name":  func(in *hub.CreateVariantInput) { in.Name = "a\nb" },
		"empty settings":   func(in *hub.CreateVariantInput) { in.Settings = "" },
		"long settings":    func(in *hub.CreateVariantInput) { in.Settings = strings.Repeat("s", 4097) },
		"non-ascii":        func(in *hub.CreateVariantInput) { in.Settings = "428é" },
		"control settings": func(in *hub.CreateVariantInput) { in.Settings = "428\x01" },
		"negative seed":    func(in *hub.CreateVariantInput) { in.Seed = -1 },
		"unknown tool":     func(in *hub.CreateVariantInput) { in.ToolID = "other" },
		"unknown version":  func(in *hub.CreateVariantInput) { in.ToolVersion = "9.9.9" },
		"bad base":         func(in *hub.CreateVariantInput) { in.BaseSHA256 = "abc" },
		"bad title id":     func(in *hub.CreateVariantInput) { in.TitleID = "xyz" },
	}
	for name, mod := range bad {
		in := okVariant(admin, g, "Bad "+name)
		mod(&in)
		if _, err := svc.CreateVariant(ctx, in); !errors.Is(err, hub.ErrBadRequest) {
			t.Errorf("%s: %v", name, err)
		}
	}
	in := okVariant(admin, g, "Unknown game")
	in.GameID = "00000000-0000-4000-8000-000000000000"
	if _, err := svc.CreateVariant(ctx, in); !errors.Is(err, hub.ErrNotFound) {
		t.Fatalf("unknown game: %v", err)
	}
	// Boundaries are fine; a seed of 2^63-1 too.
	in = okVariant(admin, g, strings.Repeat("n", 48))
	in.Settings, in.Seed = strings.Repeat("s", 4096), 1<<63-1
	if _, err := svc.CreateVariant(ctx, in); err != nil {
		t.Fatal(err)
	}
	// A version that is not offered any more stays valid for recipes (the catalog still knows it).
	if _, err := svc.SetToolOffered(ctx, hub.ToolRandomizer, false); err != nil {
		t.Fatal(err)
	}
	if _, err := svc.CreateVariant(ctx, okVariant(admin, g, "Old recipe")); err != nil {
		t.Fatalf("not offered but known: %v", err)
	}
}

func TestVariantOwnerIsolationRenameAndList(t *testing.T) {
	svc, admin, g := variantHub(t)
	anna, _ := svc.CreateUser(ctx, "anna", "Anna")
	a, _ := svc.CreateVariant(ctx, okVariant(admin, g, "A"))
	b, _ := svc.CreateVariant(ctx, okVariant(admin, g, "B"))

	if _, err := svc.RenameVariant(ctx, anna.ID, a.ID, "Mine"); !errors.Is(err, hub.ErrNotFound) {
		t.Fatalf("rename foreign: %v", err)
	}
	if err := svc.DeleteVariant(ctx, anna.ID, "", a.ID, true); !errors.Is(err, hub.ErrNotFound) {
		t.Fatalf("delete foreign: %v", err)
	}
	if vs, _ := svc.ListVariants(ctx, anna.ID, "", true); len(vs) != 0 {
		t.Fatalf("anna sees %+v", vs)
	}
	if _, err := svc.RenameVariant(ctx, admin.ID, a.ID, "b"); !errors.Is(err, &hub.Error{Code: hub.CodeNameTaken}) {
		t.Fatalf("rename to taken: %v", err)
	}
	r, err := svc.RenameVariant(ctx, admin.ID, a.ID, "A2")
	if err != nil || r.Name != "A2" || r.Slot != a.Slot || r.Seed != a.Seed {
		t.Fatalf("%+v %v", r, err)
	}
	if _, err := svc.RenameVariant(ctx, admin.ID, a.ID, "A"); err != nil { // its own name again is fine
		t.Fatal(err)
	}
	if _, err := svc.RenameVariant(ctx, admin.ID, "not-a-uuid", "x"); !errors.Is(err, hub.ErrNotFound) {
		t.Fatalf("%v", err)
	}
	if vs, _ := svc.ListVariants(ctx, admin.ID, g.ID, false); len(vs) != 2 || vs[0].ID != b.ID && vs[0].ID != a.ID {
		t.Fatalf("%+v", vs)
	}
	// Delete, keep saves: the recipe stays marked deleted, the name is free again, listings hide it.
	if err := svc.DeleteVariant(ctx, admin.ID, "", a.ID, false); err != nil {
		t.Fatal(err)
	}
	if vs, _ := svc.ListVariants(ctx, admin.ID, "", false); len(vs) != 1 || vs[0].ID != b.ID {
		t.Fatalf("%+v", vs)
	}
	vs, _ := svc.ListVariants(ctx, admin.ID, "", true)
	if len(vs) != 2 {
		t.Fatalf("%+v", vs)
	}
	for _, v := range vs {
		if v.ID == a.ID && (v.DeletedAt == nil || v.Settings != a.Settings || v.Seed != a.Seed) {
			t.Fatalf("recipe must stay: %+v", v)
		}
	}
	if err := svc.DeleteVariant(ctx, admin.ID, "", a.ID, false); err != nil {
		t.Fatalf("again: %v", err)
	}
	if _, err := svc.RenameVariant(ctx, admin.ID, a.ID, "Zed"); !errors.Is(err, hub.ErrNotFound) {
		t.Fatalf("a deleted variant cannot be renamed: %v", err)
	}
	if _, err := svc.CreateVariant(ctx, okVariant(admin, g, "A")); err != nil {
		t.Fatalf("name free again: %v", err)
	}
}

func TestVariantDeleteRemovesSaveSlot(t *testing.T) {
	svc, admin, g := variantHub(t)
	dev := pairDevice(t, svc, admin.ID, "Desktop")
	v, _ := svc.CreateVariant(ctx, okVariant(admin, g, "Saves"))
	if _, err := svc.UploadSaveFile(ctx, hub.UploadSaveInput{UserID: admin.ID, DeviceID: dev, GameID: g.ID, Slot: v.Slot, Body: bytes.NewReader([]byte("save data"))}); err != nil {
		t.Fatal(err)
	}
	other, _ := svc.CreateVariant(ctx, okVariant(admin, g, "Other"))
	if _, err := svc.UploadSaveFile(ctx, hub.UploadSaveInput{UserID: admin.ID, DeviceID: dev, GameID: g.ID, Slot: other.Slot, Body: bytes.NewReader([]byte("other save"))}); err != nil {
		t.Fatal(err)
	}
	slotExists := func(slot string) bool {
		sums, err := svc.ListSaveSlots(ctx, admin.ID)
		if err != nil {
			t.Fatal(err)
		}
		for _, s := range sums {
			if s.Slot == slot {
				return true
			}
		}
		return false
	}
	if !slotExists(v.Slot) {
		t.Fatal("slot not created")
	}
	// Keep: the saves stay.
	if err := svc.DeleteVariant(ctx, admin.ID, dev, other.ID, false); err != nil || !slotExists(other.Slot) {
		t.Fatalf("keep: %v", err)
	}
	// Delete: the slot and the recipe go, other slots stay.
	if err := svc.DeleteVariant(ctx, admin.ID, dev, v.ID, true); err != nil {
		t.Fatal(err)
	}
	if slotExists(v.Slot) || !slotExists(other.Slot) {
		t.Fatal("slot must be removed, other slots stay")
	}
	if vs, _ := svc.ListVariants(ctx, admin.ID, "", true); len(vs) != 1 || vs[0].ID != other.ID {
		t.Fatalf("recipe row must go: %+v", vs)
	}
	// A variant that never saved deletes cleanly with saves=delete; so does a kept one later.
	n, _ := svc.CreateVariant(ctx, okVariant(admin, g, "Never saved"))
	if err := svc.DeleteVariant(ctx, admin.ID, dev, n.ID, true); err != nil {
		t.Fatal(err)
	}
	if err := svc.DeleteVariant(ctx, admin.ID, dev, other.ID, true); err != nil || slotExists(other.Slot) {
		t.Fatalf("delete after keep: %v", err)
	}
}

func TestGameDeleteRemovesVariants(t *testing.T) {
	svc, admin, g := variantHub(t)
	if _, err := svc.CreateVariant(ctx, okVariant(admin, g, "X")); err != nil {
		t.Fatal(err)
	}
	if err := svc.DeleteGame(ctx, g.ID); err != nil {
		t.Fatal(err)
	}
	if vs, _ := svc.ListVariants(ctx, admin.ID, "", true); len(vs) != 0 {
		t.Fatalf("%+v", vs)
	}
}
