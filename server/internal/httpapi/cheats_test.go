package httpapi

import (
	"archive/zip"
	"crypto/sha1"
	"fmt"
	"hash/crc32"
	"os"
	"path/filepath"
	"testing"
)

// Cheat database over REST (0.20 package 3, feature cheat_db_v1). Lists and DATs are synthetic and made here.

type gameCheatsT struct {
	Source    string `json:"source"`
	UpdatedAt string `json:"updated_at"`
	MatchedBy string `json:"matched_by"`
	ListName  string `json:"list_name"`
	Cheats    []struct {
		Name        string `json:"name"`
		Code        string `json:"code"`
		FormatGuess string `json:"format_guess"`
	} `json:"cheats"`
}

type adminCheatDbT struct {
	URL         string `json:"url"`
	URL3DS      string `json:"url_3ds"`
	Lists3DS    int    `json:"lists_3ds"`
	Enabled     bool   `json:"enabled"`
	Source      string `json:"source"`
	UpdatedAt   string `json:"updated_at"`
	SizeBytes   int64  `json:"size_bytes"`
	Lists       int    `json:"lists"`
	Dats        int    `json:"dats"`
	LastCheck   string `json:"last_check"`
	LastSuccess string `json:"last_success"`
	LastError   string `json:"last_error"`
}

// cheatZip writes a cht list and a DAT for the test game (the content of demo.nds in newSessEnv).
func cheatZip(t *testing.T) string {
	t.Helper()
	const rom = "homebrew-rom-dummy"
	dat := fmt.Sprintf("game (\n\tname \"Synthetic Demo\"\n\trom ( name \"x.nds\" size %d crc %08x sha1 %x )\n)\n",
		len(rom), crc32.ChecksumIEEE([]byte(rom)), sha1.Sum([]byte(rom)))
	cht := "cheats = 2\ncheat0_desc = \"Lives\"\ncheat0_code = \"02001234 000000FF\"\ncheat0_enable = false\n" +
		"cheat1_desc = \"Odd\"\ncheat1_code = \"zzz\"\ncheat1_enable = false\n"
	p := filepath.Join(t.TempDir(), "cheats.zip")
	f, err := os.Create(p)
	if err != nil {
		t.Fatal(err)
	}
	zw := zip.NewWriter(f)
	for name, body := range map[string]string{
		"metadat/no-intro/Nintendo - Nintendo DS.dat":   dat,
		"cht/Nintendo - Nintendo DS/Synthetic Demo.cht": cht,
	} {
		w, _ := zw.Create(name)
		w.Write([]byte(body))
	}
	if err := zw.Close(); err != nil {
		t.Fatal(err)
	}
	f.Close()
	return p
}

func TestGameCheatsAPI(t *testing.T) {
	s := newSessEnv(t, nil)
	anna := s.device(s.anna.ID, "Annas PC")
	gid := s.game.ID
	const unknown = "00000000-0000-4000-8000-000000000000"

	wantStatus(t, s.do("GET", "/api/v1/games/"+gid+"/cheats", nil, opt{}), 401, "")
	wantStatus(t, s.api(anna, "GET", "/api/v1/games/"+unknown+"/cheats", nil), 404, "not_found")

	// No lists on the server yet: 200 with an empty list.
	c := decode[gameCheatsT](t, s.api(anna, "GET", "/api/v1/games/"+gid+"/cheats", nil))
	// the database is on (default URL) but holds nothing yet: "unavailable", not "none" (= off)
	if c.Source != "unavailable" || len(c.Cheats) != 0 || c.MatchedBy != "" || c.UpdatedAt != "" {
		t.Fatalf("%+v", c)
	}

	if err := s.svc.ImportCheats(t.Context(), cheatZip(t)); err != nil {
		t.Fatal(err)
	}
	c = decode[gameCheatsT](t, s.api(anna, "GET", "/api/v1/games/"+gid+"/cheats", nil))
	if c.Source != "imported" || c.MatchedBy != "hash" || c.ListName != "Synthetic Demo" || c.UpdatedAt == "" || len(c.Cheats) != 2 {
		t.Fatalf("%+v", c)
	}
	if c.Cheats[0].Name != "Lives" || c.Cheats[0].Code != "02001234 000000FF" || c.Cheats[0].FormatGuess != "ar_ds" || c.Cheats[1].FormatGuess != "raw" {
		t.Fatalf("%+v", c.Cheats)
	}
}

func TestAdminCheatDbAPI(t *testing.T) {
	s := newSessEnv(t, nil)
	anna := s.device(s.anna.ID, "Annas PC")
	admin := s.device(s.admin.ID, "Admin PC")

	// Admins only.
	wantStatus(t, s.do("GET", "/api/v1/admin/cheat-db", nil, opt{}), 401, "")
	wantStatus(t, s.api(anna, "GET", "/api/v1/admin/cheat-db", nil), 403, "forbidden")
	wantStatus(t, s.api(anna, "PUT", "/api/v1/admin/cheat-db", map[string]any{"url": "none"}), 403, "forbidden")
	wantStatus(t, s.api(anna, "POST", "/api/v1/admin/cheat-db/refresh", nil), 403, "forbidden")

	// Defaults: the libretro-database archive, nothing stored yet.
	st := decode[adminCheatDbT](t, s.api(admin, "GET", "/api/v1/admin/cheat-db", nil))
	if !st.Enabled || st.URL != "https://github.com/libretro/libretro-database/archive/refs/heads/master.zip" || st.Lists != 0 || st.Source != "" || st.UpdatedAt != "" {
		t.Fatalf("%+v", st)
	}

	// Changing the source needs "Confirm it's you".
	wantStatus(t, s.api(admin, "PUT", "/api/v1/admin/cheat-db", map[string]any{"url": "none"}), 403, "confirmation_required")
	s.confirm(admin.tok)
	wantStatus(t, s.do("PUT", "/api/v1/admin/cheat-db", map[string]any{"url": "http://insecure.example/db.zip"}, opt{token: admin.tok, raw: true}), 400, "bad_request")
	wantStatus(t, s.do("PUT", "/api/v1/admin/cheat-db", map[string]any{"url": ""}, opt{token: admin.tok, raw: true}), 400, "bad_request")
	st = decode[adminCheatDbT](t, s.api(admin, "PUT", "/api/v1/admin/cheat-db", map[string]any{"url": "https://mirror.example/db.zip"}))
	if st.URL != "https://mirror.example/db.zip" || !st.Enabled {
		t.Fatalf("%+v", st)
	}
	wantStatus(t, s.api(admin, "POST", "/api/v1/admin/cheat-db/refresh", nil), 202, "")

	// The 3DS source is off by default and changes on its own; an empty body is refused.
	if st.URL3DS != "none" || st.Lists3DS != 0 {
		t.Fatalf("%+v", st)
	}
	st = decode[adminCheatDbT](t, s.api(admin, "PUT", "/api/v1/admin/cheat-db", map[string]any{"url_3ds": "https://mirror.example/3ds.zip"}))
	if st.URL3DS != "https://mirror.example/3ds.zip" || st.URL != "https://mirror.example/db.zip" {
		t.Fatalf("%+v", st)
	}
	wantStatus(t, s.do("PUT", "/api/v1/admin/cheat-db", map[string]any{}, opt{token: admin.tok, raw: true}), 400, "bad_request")
	wantStatus(t, s.do("PUT", "/api/v1/admin/cheat-db", map[string]any{"url_3ds": "http://x.example/a.zip"}, opt{token: admin.tok, raw: true}), 400, "bad_request")
	decode[adminCheatDbT](t, s.api(admin, "PUT", "/api/v1/admin/cheat-db", map[string]any{"url_3ds": "none"}))

	// Off: refresh is a 400, the state says so; an import still works (offline servers) and shows in the state.
	st = decode[adminCheatDbT](t, s.api(admin, "PUT", "/api/v1/admin/cheat-db", map[string]any{"url": "none"}))
	if st.URL != "none" || st.Enabled {
		t.Fatalf("%+v", st)
	}
	wantStatus(t, s.api(admin, "POST", "/api/v1/admin/cheat-db/refresh", nil), 400, "bad_request")
	if err := s.svc.ImportCheats(t.Context(), cheatZip(t)); err != nil {
		t.Fatal(err)
	}
	st = decode[adminCheatDbT](t, s.api(admin, "GET", "/api/v1/admin/cheat-db", nil))
	if st.Source != "imported" || st.Lists != 1 || st.Dats != 1 || st.SizeBytes <= 0 || st.UpdatedAt == "" || st.LastSuccess == "" || st.LastError != "" || st.Enabled {
		t.Fatalf("%+v", st)
	}
}
