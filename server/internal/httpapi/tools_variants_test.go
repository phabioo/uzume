package httpapi

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/google/uuid"

	"github.com/phabioo/uzume/server/internal/hub"
	"github.com/phabioo/uzume/server/internal/hub/hubtest"
)

// Tools catalog, game variants and runs with a randomizer recipe over REST (0.21, ADR 0031). A fake GitHub source
// serves dummy engine zips; no real engine, no ROM.

type toolEnv struct {
	*sessEnv
	src         *hubtest.ToolSource
	adm         dev
	annaD, bobD dev
}

func newToolEnv(t *testing.T) *toolEnv {
	t.Helper()
	src := hubtest.NewToolSource(t)
	src.AddRelease(t, "1.6.2", false, "windows-x64", "linux-x64", "linux-arm64")
	src.AddRelease(t, "1.6.1", false, "windows-x64")
	s := newSessEnv(t, func(o *hub.Options) { src.Apply(o) })
	te := &toolEnv{sessEnv: s, src: src}
	te.adm = s.device(s.admin.ID, "Admin PC")
	te.annaD = s.device(s.anna.ID, "Anna PC")
	te.bobD = s.device(s.bob.ID, "Bob PC")
	return te
}

// ready syncs and installs 1.6.2 and offers the tool.
func (te *toolEnv) ready() {
	te.t.Helper()
	wantStatus(te.t, te.api(te.adm, "POST", "/api/v1/admin/tools/randomizer/sync", nil), 200, "")
	wantStatus(te.t, te.api(te.adm, "POST", "/api/v1/admin/tools/randomizer/versions/1.6.2/install", nil), 200, "")
	wantStatus(te.t, te.api(te.adm, "PUT", "/api/v1/admin/tools/randomizer", map[string]any{"offered": true}), 200, "")
}

type toolResp struct {
	ID            string `json:"id"`
	Name          string `json:"name"`
	Offered       bool   `json:"offered"`
	Version       string `json:"version"`
	License       string `json:"license"`
	SourceURL     string `json:"source_url"`
	LatestVersion string `json:"latest_version"`
	Update        *bool  `json:"update_available"`
	Source        struct {
		State     string `json:"state"`
		LastError string `json:"last_error"`
	} `json:"source"`
	Versions []struct {
		Version   string                       `json:"version"`
		Installed bool                         `json:"installed"`
		Complete  bool                         `json:"complete"`
		UsedBy    struct{ Variants, Runs int } `json:"used_by"`
		Platforms []struct {
			Platform string `json:"platform"`
			Size     int64  `json:"size"`
			Sha256   string `json:"sha256"`
			Cached   bool   `json:"cached"`
			Root     string `json:"root"`
			Origin   string `json:"origin"`
		} `json:"platforms"`
	} `json:"versions"`
}

func TestToolsAdminEndpointsNeedAdmin(t *testing.T) {
	te := newToolEnv(t)
	calls := []struct {
		method, path string
		body         any
	}{
		{"GET", "/api/v1/admin/tools", nil},
		{"PUT", "/api/v1/admin/tools/randomizer", map[string]any{"offered": true}},
		{"POST", "/api/v1/admin/tools/randomizer/sync", nil},
		{"POST", "/api/v1/admin/tools/randomizer/versions/1.6.2/install", nil},
		{"DELETE", "/api/v1/admin/tools/randomizer/versions/1.6.2", nil},
	}
	for _, c := range calls {
		wantStatus(t, te.do(c.method, c.path, c.body, opt{}), 401, "")
		wantStatus(t, te.api(te.annaD, c.method, c.path, c.body), 403, "forbidden")
	}
	for _, p := range []string{"/api/v1/tools", "/api/v1/tools/randomizer/packages/1.6.2/linux-x64", "/api/v1/variants", "/api/v1/games/" + te.game.ID + "/variants"} {
		wantStatus(t, te.do("GET", p, nil, opt{}), 401, "")
	}
	// The tool state did not change.
	tool := decode[struct{ Tools []toolResp }](t, te.api(te.adm, "GET", "/api/v1/admin/tools", nil)).Tools[0]
	if tool.Offered || len(tool.Versions) != 0 {
		t.Fatalf("%+v", tool)
	}
}

func TestToolsCatalogFlow(t *testing.T) {
	te := newToolEnv(t)
	adm := func() toolResp {
		return decode[struct{ Tools []toolResp }](t, te.api(te.adm, "GET", "/api/v1/admin/tools", nil)).Tools[0]
	}
	tool := adm()
	if tool.ID != "randomizer" || tool.Name != "Randomizer engine" || tool.License != "GPL-3.0" || tool.Offered || tool.Source.State != "never" ||
		tool.SourceURL != "https://github.com/upr-fvx/universal-pokemon-randomizer-fvx" {
		t.Fatalf("%+v", tool)
	}
	wantStatus(t, te.api(te.adm, "POST", "/api/v1/admin/tools/nope/sync", nil), 404, "")
	rec := te.api(te.adm, "POST", "/api/v1/admin/tools/randomizer/sync", nil)
	wantStatus(t, rec, 200, "")
	tool = decode[toolResp](t, rec)
	if tool.LatestVersion != "1.6.2" || tool.Update == nil || !*tool.Update || len(tool.Versions) != 2 || tool.Source.State != "ok" || tool.Offered {
		t.Fatalf("%+v", tool)
	}
	// Nothing installed yet: the app sees nothing, and a failing sync is shown, not an HTTP error.
	if got := decode[struct{ Tools []toolResp }](t, te.api(te.annaD, "GET", "/api/v1/tools", nil)); len(got.Tools) != 0 {
		t.Fatalf("%+v", got)
	}
	te.src.Down = true
	rec = te.api(te.adm, "POST", "/api/v1/admin/tools/randomizer/sync", nil)
	wantStatus(t, rec, 200, "")
	if tool = decode[toolResp](t, rec); tool.Source.State != "error" || !strings.Contains(tool.Source.LastError, "503") || len(tool.Versions) != 2 {
		t.Fatalf("%+v", tool)
	}
	te.src.Down = false

	wantStatus(t, te.api(te.adm, "POST", "/api/v1/admin/tools/randomizer/versions/9.9.9/install", nil), 404, "")
	rec = te.api(te.adm, "POST", "/api/v1/admin/tools/randomizer/versions/1.6.2/install", nil)
	wantStatus(t, rec, 200, "")
	tool = decode[toolResp](t, rec)
	v := tool.Versions[0]
	if !v.Installed || !v.Complete || tool.Offered || tool.Update == nil || *tool.Update || len(v.Platforms) != 3 {
		t.Fatalf("%+v", tool)
	}
	zip := te.src.Zip("1.6.2", "linux-x64")
	sum := sha256.Sum256(zip)
	wantSum := hex.EncodeToString(sum[:])
	var linux = v.Platforms[0]
	for _, p := range v.Platforms {
		if p.Platform == "linux-x64" {
			linux = p
		}
	}
	if linux.Sha256 != wantSum || linux.Size != int64(len(zip)) || !linux.Cached || linux.Root != "UPR_FVX/" || linux.Origin != "github" {
		t.Fatalf("%+v", linux)
	}

	// Installing does not offer: still invisible and not downloadable for apps.
	pkg := "/api/v1/tools/randomizer/packages/1.6.2/linux-x64"
	if got := decode[struct{ Tools []toolResp }](t, te.api(te.annaD, "GET", "/api/v1/tools", nil)); len(got.Tools) != 0 {
		t.Fatalf("%+v", got)
	}
	wantStatus(t, te.api(te.annaD, "GET", pkg, nil), 404, "")

	wantStatus(t, te.api(te.adm, "PUT", "/api/v1/admin/tools/randomizer", map[string]any{"offered": true}), 200, "")
	rec = te.api(te.annaD, "GET", "/api/v1/tools", nil)
	wantStatus(t, rec, 200, "")
	app := decode[struct{ Tools []toolResp }](t, rec).Tools
	if len(app) != 1 || !app[0].Offered || app[0].Version != "1.6.2" || len(app[0].Versions) != 1 || len(app[0].Versions[0].Platforms) != 3 ||
		app[0].License != "GPL-3.0" || strings.Contains(rec.Body.String(), "\"origin\"") {
		t.Fatalf("%s", rec.Body.String())
	}

	// Download with ETag and 304.
	rec = te.api(te.annaD, "GET", pkg, nil)
	wantStatus(t, rec, 200, "")
	if !bytes.Equal(rec.Body.Bytes(), zip) || rec.Header().Get("ETag") != `"`+wantSum+`"` || rec.Header().Get("Content-Type") != "application/zip" {
		t.Fatalf("download: %s %s", rec.Header().Get("ETag"), rec.Header().Get("Content-Type"))
	}
	rec = te.do("GET", pkg, nil, opt{token: te.annaD.tok, header: map[string]string{"If-None-Match": `"` + wantSum + `"`}})
	if rec.Code != 304 || rec.Body.Len() != 0 {
		t.Fatalf("%d", rec.Code)
	}
	// Not cached / unknown: 404.
	for _, p := range []string{"/api/v1/tools/randomizer/packages/1.6.1/windows-x64", "/api/v1/tools/nope/packages/1.6.2/linux-x64"} {
		if rec = te.api(te.annaD, "GET", p, nil); rec.Code != 404 && rec.Code != 400 {
			t.Fatalf("%s: %d", p, rec.Code)
		}
	}
	wantStatus(t, te.api(te.annaD, "GET", "/api/v1/tools/randomizer/packages/1.6.1/windows-x64", nil), 404, "")
	if rec = te.do("GET", "/api/v1/tools/randomizer/packages/1.6.2/macos-arm64", nil, opt{token: te.annaD.tok, raw: true}); rec.Code != 400 && rec.Code != 404 {
		t.Fatalf("unknown platform: %d", rec.Code)
	}

	// A cached file that vanished and comes back with other content is refused (409 tool_hash_mismatch).
	if err := os.Remove(filepath.Join(te.svc.DataDir(), "tools", "randomizer", "1.6.2", "windows-x64.zip")); err != nil {
		t.Fatal(err)
	}
	te.src.Replace("1.6.2", "windows-x64", hubtest.ToolZip(t, "windows-x64", false, "changed"))
	wantStatus(t, te.api(te.adm, "POST", "/api/v1/admin/tools/randomizer/versions/1.6.2/install", nil), 409, "tool_hash_mismatch")

	// Removal is refused while a variant uses the version (409 tool_in_use with used_by).
	vb := map[string]any{"name": "Chaos", "tool_id": "randomizer", "tool_version": "1.6.2", "settings": "428abc", "seed": 5}
	rec = te.api(te.annaD, "POST", "/api/v1/games/"+te.game.ID+"/variants", vb)
	wantStatus(t, rec, 201, "")
	vid := decode[struct{ ID string }](t, rec).ID
	rec = te.api(te.adm, "DELETE", "/api/v1/admin/tools/randomizer/versions/1.6.2", nil)
	wantStatus(t, rec, 409, "tool_in_use")
	if u := decode[struct {
		UsedBy struct{ Variants, Runs int } `json:"used_by"`
	}](t, rec).UsedBy; u.Variants != 1 || u.Runs != 0 {
		t.Fatalf("%s", rec.Body.String())
	}
	wantStatus(t, te.api(te.annaD, "DELETE", "/api/v1/variants/"+vid, nil), 204, "")
	wantStatus(t, te.api(te.adm, "DELETE", "/api/v1/admin/tools/randomizer/versions/1.6.2", nil), 204, "")
	wantStatus(t, te.api(te.adm, "DELETE", "/api/v1/admin/tools/randomizer/versions/1.6.2", nil), 404, "")
	wantStatus(t, te.api(te.annaD, "GET", pkg, nil), 404, "")
}

func TestToolsSourceOffAnswers400(t *testing.T) {
	e := newEnv(t, nil)
	tok := e.login(uuid.NewString())
	wantStatus(t, e.do("POST", "/api/v1/admin/tools/randomizer/sync", nil, opt{token: tok}), 400, "bad_request")
	tool := decode[struct{ Tools []toolResp }](t, e.do("GET", "/api/v1/admin/tools", nil, opt{token: tok})).Tools[0]
	if tool.Source.State != "off" {
		t.Fatalf("%+v", tool.Source)
	}
}

type variantResp struct {
	ID          string  `json:"id"`
	GameID      string  `json:"game_id"`
	Name        string  `json:"name"`
	Slot        string  `json:"slot"`
	ToolID      string  `json:"tool_id"`
	ToolVersion string  `json:"tool_version"`
	Settings    string  `json:"settings"`
	Seed        int64   `json:"seed"`
	BaseSha256  string  `json:"base_sha256"`
	SpoilerLog  bool    `json:"spoiler_log"`
	RunID       *string `json:"run_id"`
	DeletedAt   *string `json:"deleted_at"`
}

func variantBody(name string) map[string]any {
	return map[string]any{"name": name, "tool_id": "randomizer", "tool_version": "1.6.2", "settings": "428secret", "seed": 12345,
		"base_sha256": strings.Repeat("ab", 32), "spoiler_log": true}
}

func TestVariantsREST(t *testing.T) {
	te := newToolEnv(t)
	te.ready()
	path := "/api/v1/games/" + te.game.ID + "/variants"
	rec := te.api(te.annaD, "POST", path, variantBody("  Nuzlocke friendly "))
	wantStatus(t, rec, 201, "")
	v := decode[variantResp](t, rec)
	if v.Name != "Nuzlocke friendly" || v.GameID != te.game.ID || !strings.HasPrefix(v.Slot, "var-") || len(v.Slot) != 12 || v.Seed != 12345 ||
		v.Settings != "428secret" || !v.SpoilerLog || v.RunID != nil || v.DeletedAt != nil || v.BaseSha256 != strings.Repeat("ab", 32) ||
		v.ToolID != "randomizer" || v.ToolVersion != "1.6.2" {
		t.Fatalf("%+v", v)
	}
	wantStatus(t, te.api(te.annaD, "POST", path, variantBody("nuzlocke friendly")), 409, "name_taken")
	wantStatus(t, te.api(te.bobD, "POST", path, variantBody("Nuzlocke friendly")), 201, "") // another person, same name

	// Validation.
	for name, mod := range map[string]func(map[string]any){
		"name too long": func(b map[string]any) { b["name"] = strings.Repeat("x", 49) },
		"empty name":    func(b map[string]any) { b["name"] = " " },
		"bad version":   func(b map[string]any) { b["tool_version"] = "7.7.7" },
		"bad tool":      func(b map[string]any) { b["tool_id"] = "other" },
		"non-ascii":     func(b map[string]any) { b["settings"] = "42ö" },
		"bad base":      func(b map[string]any) { b["base_sha256"] = "zz" },
	} {
		b := variantBody("Invalid " + name)
		mod(b)
		rec = te.do("POST", path, b, opt{token: te.annaD.tok, raw: true})
		if rec.Code != 400 {
			t.Errorf("%s: %d %s", name, rec.Code, rec.Body.String())
		}
	}
	for _, b := range []map[string]any{{"seed": -1}, {"settings": ""}, {"settings": strings.Repeat("s", 4097)}} {
		bb := variantBody("Bounds")
		for k, val := range b {
			bb[k] = val
		}
		if rec = te.do("POST", path, bb, opt{token: te.annaD.tok, raw: true}); rec.Code != 400 {
			t.Errorf("%v: %d", b, rec.Code)
		}
	}
	wantStatus(t, te.api(te.annaD, "POST", "/api/v1/games/00000000-0000-4000-8000-000000000000/variants", variantBody("X")), 404, "")

	// Owner isolation: lists, rename and delete.
	for _, p := range []string{"/api/v1/variants", path} {
		list := decode[struct{ Variants []variantResp }](t, te.api(te.bobD, "GET", p, nil)).Variants
		if len(list) != 1 || list[0].ID == v.ID {
			t.Fatalf("bob sees %+v", list)
		}
		list = decode[struct{ Variants []variantResp }](t, te.api(te.annaD, "GET", p, nil)).Variants
		if len(list) != 1 || list[0].ID != v.ID {
			t.Fatalf("anna sees %+v", list)
		}
	}
	wantStatus(t, te.api(te.bobD, "PATCH", "/api/v1/variants/"+v.ID, map[string]any{"name": "Mine"}), 404, "")
	wantStatus(t, te.api(te.bobD, "DELETE", "/api/v1/variants/"+v.ID, nil), 404, "")
	wantStatus(t, te.api(te.adm, "DELETE", "/api/v1/variants/"+v.ID, nil), 404, "") // not even the admin

	rec = te.api(te.annaD, "PATCH", "/api/v1/variants/"+v.ID, map[string]any{"name": "Renamed"})
	wantStatus(t, rec, 200, "")
	if r := decode[variantResp](t, rec); r.Name != "Renamed" || r.Slot != v.Slot {
		t.Fatalf("%+v", r)
	}
	other := decode[variantResp](t, te.api(te.annaD, "POST", path, variantBody("Second")))
	wantStatus(t, te.api(te.annaD, "PATCH", "/api/v1/variants/"+other.ID, map[string]any{"name": "renamed"}), 409, "name_taken")

	// Delete: keep the saves (default) keeps the recipe, hidden unless include_deleted=1.
	wantStatus(t, te.api(te.annaD, "DELETE", "/api/v1/variants/"+v.ID+"?saves=keep", nil), 204, "")
	list := decode[struct{ Variants []variantResp }](t, te.api(te.annaD, "GET", "/api/v1/variants", nil)).Variants
	if len(list) != 1 || list[0].ID != other.ID {
		t.Fatalf("%+v", list)
	}
	list = decode[struct{ Variants []variantResp }](t, te.api(te.annaD, "GET", "/api/v1/variants?include_deleted=1", nil)).Variants
	if len(list) != 2 {
		t.Fatalf("%+v", list)
	}
	for _, x := range list {
		if x.ID == v.ID && (x.DeletedAt == nil || x.Settings != "428secret" || x.Seed != 12345) {
			t.Fatalf("recipe kept: %+v", x)
		}
	}
	// Delete with the saves: the slot goes too (a save exists).
	if _, err := te.svc.UploadSaveFile(t.Context(), hub.UploadSaveInput{UserID: te.anna.ID, DeviceID: te.annaD.id, GameID: te.game.ID, Slot: other.Slot,
		Body: strings.NewReader("save")}); err != nil {
		t.Fatal(err)
	}
	rec = te.api(te.annaD, "GET", "/api/v1/saves", nil)
	if !strings.Contains(rec.Body.String(), other.Slot) {
		t.Fatalf("slot missing: %s", rec.Body.String())
	}
	wantStatus(t, te.api(te.annaD, "DELETE", "/api/v1/variants/"+other.ID+"?saves=delete", nil), 204, "")
	if rec = te.api(te.annaD, "GET", "/api/v1/saves", nil); strings.Contains(rec.Body.String(), other.Slot) {
		t.Fatalf("slot still there: %s", rec.Body.String())
	}
	list = decode[struct{ Variants []variantResp }](t, te.api(te.annaD, "GET", "/api/v1/variants?include_deleted=1", nil)).Variants
	if len(list) != 1 || list[0].ID != v.ID {
		t.Fatalf("%+v", list)
	}
	if rec = te.do("DELETE", "/api/v1/variants/"+v.ID+"?saves=maybe", nil, opt{token: te.annaD.tok, raw: true}); rec.Code != 400 {
		t.Fatalf("%d", rec.Code)
	}
}

type rzRun struct {
	RunID      string `json:"run_id"`
	Randomizer struct {
		Mode        string  `json:"mode"`
		PresetID    string  `json:"preset_id"`
		ToolID      string  `json:"tool_id"`
		ToolVersion string  `json:"tool_version"`
		AllowOwn    bool    `json:"allow_own"`
		BaseSha256  string  `json:"base_sha256"`
		TitleID     *string `json:"title_id"`
	} `json:"randomizer"`
	Participants []struct {
		UserID       string  `json:"user_id"`
		Status       string  `json:"status"`
		VariantKind  *string `json:"variant_kind"`
		VariantState *string `json:"variant_state"`
	} `json:"participants"`
}

func (r rzRun) part(id string) (kind, state string, ok bool) {
	for _, p := range r.Participants {
		if p.UserID == id {
			if p.VariantKind != nil {
				kind = *p.VariantKind
			}
			if p.VariantState != nil {
				state = *p.VariantState
			}
			return kind, state, true
		}
	}
	return "", "", false
}

func TestRunRandomizerREST(t *testing.T) {
	te := newToolEnv(t)
	te.ready()
	body := func(mode string, allowOwn bool) map[string]any {
		return map[string]any{"name": "Rando", "plugins": []any{}, "game_id": te.game.ID, "invite_user_ids": []string{te.anna.ID, te.bob.ID},
			"randomizer": map[string]any{"mode": mode, "preset_id": "classic", "settings": "428verysecretsettings", "tool_version": "1.6.2", "allow_own": allowOwn}}
	}
	// A version that is not offered: 400.
	bad := body("each", false)
	bad["randomizer"].(map[string]any)["tool_version"] = "1.6.1"
	wantStatus(t, te.api(te.adm, "POST", "/api/v1/runs", bad), 400, "bad_request")
	bad["randomizer"] = map[string]any{"mode": "each", "tool_version": "1.6.2"} // no settings
	wantStatus(t, te.api(te.adm, "POST", "/api/v1/runs", bad), 400, "bad_request")

	rec := te.api(te.adm, "POST", "/api/v1/runs", body("each", true))
	wantStatus(t, rec, 201, "")
	run := decode[rzRun](t, rec)
	rz := run.Randomizer
	if rz.Mode != "each" || rz.PresetID != "classic" || rz.ToolID != "randomizer" || rz.ToolVersion != "1.6.2" || !rz.AllowOwn ||
		rz.BaseSha256 != te.game.ROMSHA256 || rz.TitleID != nil {
		t.Fatalf("%+v", rz)
	}
	if k, s, _ := run.part(te.env.admin.ID); k != "run" || s != "not_built" {
		t.Fatalf("creator %s %s", k, s)
	}
	// Settings and seed never appear in any view of the run.
	hidden := func(rec interface{ String() string }) {
		t.Helper()
		if b := rec.String(); strings.Contains(b, "verysecretsettings") || strings.Contains(b, "seed") {
			t.Fatalf("leak: %s", b)
		}
	}
	hidden(rec.Body)
	for _, d := range []dev{te.adm, te.annaD, te.bobD} {
		hidden(te.api(d, "GET", "/api/v1/runs/"+run.RunID, nil).Body)
		hidden(te.api(d, "GET", "/api/v1/runs", nil).Body)
	}
	hidden(te.api(te.adm, "GET", "/api/v1/admin/runs/"+run.RunID, nil).Body)

	// The recipe is for joined participants only; the seed differs per person and is not the run seed.
	variantPath := "/api/v1/runs/" + run.RunID + "/variant"
	wantStatus(t, te.api(te.annaD, "GET", variantPath, nil), 403, "forbidden") // invited
	stranger, _ := te.svc.CreateUser(t.Context(), "zed", "Zed")
	zedD := te.device(stranger.ID, "Zed PC")
	wantStatus(t, te.api(zedD, "GET", variantPath, nil), 404, "")
	type recipe struct {
		Settings, ToolID, ToolVersion, BaseSha256, Slot, VariantKind string
		Seed                                                         int64
	}
	rjson := func(rec interface{ String() string }) recipe {
		var r struct {
			Settings    string `json:"settings"`
			Seed        int64  `json:"seed"`
			ToolID      string `json:"tool_id"`
			ToolVersion string `json:"tool_version"`
			BaseSha256  string `json:"base_sha256"`
			Slot        string `json:"slot"`
			VariantKind string `json:"variant_kind"`
		}
		if err := jsonUnmarshal(rec.String(), &r); err != nil {
			t.Fatal(err)
		}
		return recipe{r.Settings, r.ToolID, r.ToolVersion, r.BaseSha256, r.Slot, r.VariantKind, r.Seed}
	}
	rec = te.api(te.adm, "GET", variantPath, nil)
	wantStatus(t, rec, 200, "")
	ra := rjson(rec.Body)
	if ra.Settings != "428verysecretsettings" || ra.ToolID != "randomizer" || ra.ToolVersion != "1.6.2" || ra.BaseSha256 != te.game.ROMSHA256 ||
		ra.Slot != hub.RunSaveSlot(run.RunID) || ra.VariantKind != "run" || ra.Seed < 0 {
		t.Fatalf("%+v", ra)
	}

	// Anna joins with an own variant, Bob with the run variant.
	own := decode[variantResp](t, te.api(te.annaD, "POST", "/api/v1/games/"+te.game.ID+"/variants", variantBody("Own")))
	wantStatus(t, te.post(te.annaD, run.RunID, "join", map[string]any{"game_id": te.game.ID, "variant": map[string]any{"kind": "own"}}), 400, "") // no variant_id
	wantStatus(t, te.post(te.annaD, run.RunID, "join", map[string]any{"game_id": te.game.ID, "variant": map[string]any{"kind": "own", "variant_id": "00000000-0000-4000-8000-000000000001"}}), 404, "")
	rec = te.post(te.annaD, run.RunID, "join", map[string]any{"game_id": te.game.ID, "variant": map[string]any{"kind": "own", "variant_id": own.ID}})
	wantStatus(t, rec, 200, "")
	if k, s, _ := decode[rzRun](t, rec).part(te.anna.ID); k != "own" || s != "not_built" {
		t.Fatalf("%s %s", k, s)
	}
	if strings.Contains(rec.Body.String(), own.ID) {
		t.Fatalf("own variant id leaked: %s", rec.Body.String())
	}
	wantStatus(t, te.post(te.bobD, run.RunID, "join", map[string]any{"game_id": te.game.ID}), 200, "")
	rb := rjson(te.api(te.bobD, "GET", variantPath, nil).Body)
	ranna := rjson(te.api(te.annaD, "GET", variantPath, nil).Body)
	if rb.Seed == ra.Seed || rb.VariantKind != "run" || ranna.VariantKind != "own" || ranna.Settings != "428secret" || ranna.Seed != 12345 || ranna.Slot != ra.Slot {
		t.Fatalf("%+v %+v %+v", ra, rb, ranna)
	}

	// Own variants need allow_own and a matching engine version.
	closed := decode[rzRun](t, te.api(te.adm, "POST", "/api/v1/runs", body("same", false)))
	wantStatus(t, te.post(te.annaD, closed.RunID, "join", map[string]any{"game_id": te.game.ID, "variant": map[string]any{"kind": "own", "variant_id": own.ID}}), 409, "own_variants_off")
	te.src.AddRelease(t, "1.6.3", false, "linux-x64")
	wantStatus(t, te.api(te.adm, "POST", "/api/v1/admin/tools/randomizer/sync", nil), 200, "")
	wantStatus(t, te.api(te.adm, "POST", "/api/v1/admin/tools/randomizer/versions/1.6.3/install", nil), 200, "")
	nb := body("same", true)
	nb["randomizer"].(map[string]any)["tool_version"] = "1.6.3"
	newer := decode[rzRun](t, te.api(te.adm, "POST", "/api/v1/runs", nb))
	wantStatus(t, te.post(te.annaD, newer.RunID, "join", map[string]any{"game_id": te.game.ID, "variant": map[string]any{"kind": "own", "variant_id": own.ID}}), 409, "own_variant_mismatch")
	wantStatus(t, te.post(te.annaD, newer.RunID, "join", map[string]any{"game_id": te.game.ID, "variant": map[string]any{"kind": "run"}}), 200, "")
	// Another person's variant does not exist for the caller.
	bobsOwn := decode[variantResp](t, te.api(te.bobD, "POST", "/api/v1/games/"+te.game.ID+"/variants", variantBody("Bobs")))
	wantStatus(t, te.post(te.bobD, closed.RunID, "join", map[string]any{"game_id": te.game.ID, "variant": map[string]any{"kind": "own", "variant_id": own.ID}}), 409, "own_variants_off")
	wantStatus(t, te.post(te.annaD, run.RunID, "join", map[string]any{"game_id": te.game.ID, "variant": map[string]any{"kind": "own", "variant_id": bobsOwn.ID}}), 200, "") // already joined: no-op

	// Variant state: the person themself, a run with randomizer only.
	vs := "/api/v1/runs/" + run.RunID + "/variant-state"
	rec = te.api(te.bobD, "POST", vs, map[string]any{"state": "building"})
	wantStatus(t, rec, 200, "")
	if _, s, _ := decode[rzRun](t, rec).part(te.bob.ID); s != "building" {
		t.Fatalf("%s", s)
	}
	if _, s, _ := decode[rzRun](t, te.api(te.adm, "GET", "/api/v1/runs/"+run.RunID, nil)).part(te.bob.ID); s != "building" {
		t.Fatalf("others see it: %s", s)
	}
	wantStatus(t, te.api(te.bobD, "POST", vs, map[string]any{"state": "ready"}), 200, "")
	if rec = te.do("POST", vs, map[string]any{"state": "finished"}, opt{token: te.bobD.tok, raw: true}); rec.Code != 400 {
		t.Fatalf("%d", rec.Code)
	}
	wantStatus(t, te.api(zedD, "POST", vs, map[string]any{"state": "ready"}), 404, "")
	plain := decode[rzRun](t, te.api(te.adm, "POST", "/api/v1/runs", map[string]any{"name": "Plain", "plugins": []any{}, "game_id": te.game.ID}))
	if plain.Randomizer.Mode != "off" {
		t.Fatalf("%+v", plain.Randomizer)
	}
	wantStatus(t, te.api(te.adm, "POST", "/api/v1/runs/"+plain.RunID+"/variant-state", map[string]any{"state": "ready"}), 409, "")
	wantStatus(t, te.api(te.adm, "GET", "/api/v1/runs/"+plain.RunID+"/variant", nil), 409, "")
	wantStatus(t, te.post(te.annaD, plain.RunID, "join", map[string]any{"game_id": te.game.ID, "variant": map[string]any{"kind": "run"}}), 404, "") // not invited
}

func jsonUnmarshal(s string, v any) error { return json.Unmarshal([]byte(s), v) }

// post is a POST below /runs/{id}/.
func (te *toolEnv) post(d dev, runID, rest string, body any) *httptest.ResponseRecorder {
	te.t.Helper()
	return te.api(d, "POST", "/api/v1/runs/"+runID+"/"+rest, body)
}
