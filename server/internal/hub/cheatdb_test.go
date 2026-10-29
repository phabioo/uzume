package hub_test

import (
	"archive/zip"
	"bytes"
	"crypto/sha1"
	"errors"
	"fmt"
	"hash/crc32"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"

	"github.com/phabioo/uzume/server/internal/hub"
	"github.com/phabioo/uzume/server/internal/hub/hubtest"
)

// Cheat database (0.20 package 3, feature cheat_db_v1). Everything is synthetic: lists, DATs and ROM bytes are made
// here, the names are invented, and downloads go to a local TLS test server.

func TestParseCht(t *testing.T) {
	in := "\xef\xbb\xbf# a comment\ncheats = 4\n" +
		"cheat0_desc = \"Alpha\"\ncheat0_code = \"AAAAAAAA BBBBBBBB\"\ncheat0_enable = false\n" +
		"cheat1_desc = \"No code\"\n" +
		"cheat2_code = \"CCCCCCCC DDDDDDDD+EEEEEEEE FFFFFFFF\"\n" +
		"cheat3_desc = Plain\ncheat3_code = 12345678\nnot a key value line\ncheatx_desc = \"ignored\"\ncheat9999999_code = \"x\"\n"
	got := hub.ParseCht([]byte(in))
	if len(got) != 3 {
		t.Fatalf("%+v", got)
	}
	if got[0].Name != "Alpha" || got[0].Code != "AAAAAAAA BBBBBBBB" {
		t.Fatalf("%+v", got[0])
	}
	if got[1].Name != "Cheat 3" || got[1].Code != "CCCCCCCC DDDDDDDD+EEEEEEEE FFFFFFFF" {
		t.Fatalf("a missing description gets a number: %+v", got[1])
	}
	if got[2].Name != "Plain" || got[2].Code != "12345678" {
		t.Fatalf("%+v", got[2])
	}
	if len(hub.ParseCht(nil)) != 0 || len(hub.ParseCht([]byte("cheats = 0\n"))) != 0 {
		t.Fatal("empty input gives no cheats")
	}
}

func TestParseDat(t *testing.T) {
	sha := strings.Repeat("ab", 20)
	text := "clrmamepro (\n\tname \"No-Intro\"\n)\n\ngame (\n\tname \"Synthetic One\"\n\tdescription \"Synthetic One\"\n" +
		"\trom ( name \"one.nds\" size 4 crc 0A0B0C0D sha1 " + strings.ToUpper(sha) + " )\n)\n" +
		"game (\n\tname \"Synthetic Two\"\n\trom ( name \"two.nds\" size 4 crc 11223344 )\n)\n"
	idx := hub.ParseDat([]byte(text))
	if idx.SHA1[sha] != "Synthetic One" || idx.CRC["0a0b0c0d"] != "Synthetic One" || idx.CRC["11223344"] != "Synthetic Two" || len(idx.SHA1) != 1 {
		t.Fatalf("%+v", idx)
	}
	xml := `<?xml version="1.0"?><datafile><header><name>x</name></header>` +
		`<game name="Synthetic Three"><rom name="three.nds" size="4" crc="AABBCCDD" sha1="` + sha + `"/></game></datafile>`
	idx = hub.ParseDat([]byte(xml))
	if idx.SHA1[sha] != "Synthetic Three" || idx.CRC["aabbccdd"] != "Synthetic Three" {
		t.Fatalf("%+v", idx)
	}
	if idx := hub.ParseDat([]byte("garbage ( (")); len(idx.SHA1)+len(idx.CRC) != 0 {
		t.Fatal("garbage gives an empty index")
	}
}

func TestGuessCheatFormat(t *testing.T) {
	for _, c := range []struct{ sys, code, want string }{
		{"gba", "1A2B3C4D 1234", hub.CheatCB},
		{"gba", "1A2B3C4D 12345678", hub.CheatARGBA},
		{"gba", "1A2B3C4D12345678", hub.CheatGSA},
		{"gba", "1A2B3C4D 1234+5A6B7C8D 9876", hub.CheatCB},
		{"gba", "1A2B3C4D 1234+5A6B7C8D 98765432", hub.CheatRaw},
		{"nds", "02001234 000000FF", hub.CheatARDS},
		{"nds", "garbage", hub.CheatRaw},
		{"3ds", "00123456 000000FF\nD3000000 00000000", hub.CheatGateway},
		{"gb", "00A-17B-C49", hub.CheatGameGenie},
		{"gbc", "00A-17B", hub.CheatGameGenie},
		{"gb", "01FF16C0", hub.CheatGSA},
		{"gb", "nonsense", hub.CheatRaw},
		{"psx", "1A2B3C4D 1234", hub.CheatRaw},
	} {
		if got := hub.GuessCheatFormat(c.sys, c.code); got != c.want {
			t.Errorf("%s %q: %s, want %s", c.sys, c.code, got, c.want)
		}
	}
}

func zipOf(t *testing.T, files map[string]string) []byte {
	t.Helper()
	var buf bytes.Buffer
	zw := zip.NewWriter(&buf)
	for name, body := range files {
		w, err := zw.Create(name)
		if err != nil {
			t.Fatal(err)
		}
		w.Write([]byte(body))
	}
	if err := zw.Close(); err != nil {
		t.Fatal(err)
	}
	return buf.Bytes()
}

func writeZip(t *testing.T, files map[string]string) string {
	t.Helper()
	p := filepath.Join(t.TempDir(), "db.zip")
	if err := os.WriteFile(p, zipOf(t, files), 0o600); err != nil {
		t.Fatal(err)
	}
	return p
}

func gatewayOf(name, code string) string {
	return "*citra_enabled\n[" + name + "]\n" + code + "\n"
}

func chtOf(entries ...string) string {
	var b strings.Builder
	fmt.Fprintf(&b, "cheats = %d\n", len(entries)/2)
	for i := 0; i+1 < len(entries); i += 2 {
		fmt.Fprintf(&b, "cheat%d_desc = %q\ncheat%d_code = %q\ncheat%d_enable = false\n", i/2, entries[i], i/2, entries[i+1], i/2)
	}
	return b.String()
}

// synthetic database: the DAT names the ROM content of romBytes "Synthetic Demo".
const demoROM = "homebrew-rom-dummy"

func demoFiles() map[string]string {
	sum := sha1.Sum([]byte(demoROM))
	dat := fmt.Sprintf("game (\n\tname \"Synthetic Demo\"\n\trom ( name \"x.nds\" size %d crc %08x sha1 %x )\n)\n",
		len(demoROM), crc32.ChecksumIEEE([]byte(demoROM)), sum)
	return map[string]string{
		"libretro-database-master/metadat/no-intro/Nintendo - Nintendo DS.dat":      dat,
		"libretro-database-master/cht/Nintendo - Nintendo DS/Synthetic Demo.cht":    chtOf("Infinite things", "02001234 000000FF", "Odd one", "XYZ"),
		"libretro-database-master/cht/Nintendo - Nintendo DS/Other Thing.cht":       chtOf("By name", "02005678 00000001"),
		"libretro-database-master/3ds/0004000000abcdef.txt":                         gatewayOf("Title cheat", "00123456 000000ff"),
		"libretro-database-master/cht/Nintendo - Nintendo 3DS/0004000000AAAAAA.cht": chtOf("Ignored", "00123456 000000FF"),
		"libretro-database-master/3ds/notatitleid.txt":                              gatewayOf("Ignored", "00123456 000000FF"),
		"libretro-database-master/cht/Unknown System/Nope.cht":                      chtOf("Not wanted", "1"),
		"libretro-database-master/README.md":                                        "not a list",
		"../evil.cht":                                                               "cheats = 0",
	}
}

func TestCheatDBImportAndMatching(t *testing.T) {
	svc, _, admin := newAdmin(t)
	// Nothing stored yet: an empty answer, never an error.
	demo, err := svc.AddROM(ctx, bytes.NewReader([]byte(demoROM)), "demo.nds", "Demo Homebrew", "", admin.ID)
	if err != nil {
		t.Fatal(err)
	}
	l, err := svc.GameCheats(ctx, admin.ID, demo.ID)
	if err != nil || l.Source != "unavailable" || len(l.Cheats) != 0 || l.Cheats == nil {
		t.Fatalf("%+v %v", l, err)
	}
	st, err := svc.CheatDBStatus(ctx)
	if err != nil || !st.Enabled || st.URL != hub.DefaultCheatDBURL || st.Lists != 0 || st.UpdatedAt != nil {
		t.Fatalf("%+v %v", st, err)
	}

	if err := svc.ImportCheats(ctx, writeZip(t, demoFiles())); err != nil {
		t.Fatal(err)
	}
	st, err = svc.CheatDBStatus(ctx)
	if err != nil || st.Source != "imported" || st.Lists != 3 || st.Dats != 1 || st.UpdatedAt == nil || st.SizeBytes <= 0 || st.LastSuccess == nil || st.LastError != "" {
		t.Fatalf("%+v %v", st, err)
	}

	// By hash: the ROM bytes give the No-Intro name, the name gives the list. Formats are guessed per code.
	l, err = svc.GameCheats(ctx, admin.ID, demo.ID)
	if err != nil || l.Source != "imported" || l.MatchedBy != "hash" || l.ListName != "Synthetic Demo" || len(l.Cheats) != 2 || l.UpdatedAt == nil {
		t.Fatalf("%+v %v", l, err)
	}
	if l.Cheats[0].Name != "Infinite things" || l.Cheats[0].FormatGuess != hub.CheatARDS || l.Cheats[1].FormatGuess != hub.CheatRaw {
		t.Fatalf("%+v", l.Cheats)
	}
	// The hashes are cached: a second lookup works without the ROM file.
	if err := os.Remove(filepath.Join(svc.DataDir(), "roms", demo.ROMSHA256[:2], demo.ROMSHA256)); err != nil {
		t.Fatal(err)
	}
	if l, err := svc.GameCheats(ctx, admin.ID, demo.ID); err != nil || l.MatchedBy != "hash" {
		t.Fatalf("cached hashes: %+v %v", l, err)
	}

	// By file name when the hash is unknown.
	other, err := svc.AddROM(ctx, bytes.NewReader([]byte("another dummy")), "Other Thing.nds", "", "", admin.ID)
	if err != nil {
		t.Fatal(err)
	}
	if l, err = svc.GameCheats(ctx, admin.ID, other.ID); err != nil || l.MatchedBy != "name" || len(l.Cheats) != 1 || l.Cheats[0].Name != "By name" {
		t.Fatalf("%+v %v", l, err)
	}
	// 3DS by title ID.
	g3, err := svc.AddROM(ctx, bytes.NewReader(ncsdROM(0x0004000000ABCDEF)), "three.3ds", "", "", admin.ID)
	if err != nil {
		t.Fatal(err)
	}
	if l, err = svc.GameCheats(ctx, admin.ID, g3.ID); err != nil || l.MatchedBy != "title_id" || len(l.Cheats) != 1 || l.Cheats[0].FormatGuess != hub.CheatGateway || l.Cheats[0].Code != "00123456 000000FF" {
		t.Fatalf("%+v %v", l, err)
	}
	// No list for the game: empty, the source still shows what the server holds.
	none, err := svc.AddROM(ctx, bytes.NewReader([]byte("unlisted")), "unlisted.nds", "", "", admin.ID)
	if err != nil {
		t.Fatal(err)
	}
	if l, err = svc.GameCheats(ctx, admin.ID, none.ID); err != nil || l.MatchedBy != "" || len(l.Cheats) != 0 || l.Source != "imported" {
		t.Fatalf("%+v %v", l, err)
	}
	if _, err := svc.GameCheats(ctx, admin.ID, "00000000-0000-4000-8000-000000000000"); !errors.Is(err, hub.ErrNotFound) {
		t.Fatalf("unknown game: %v", err)
	}

	// Unwanted entries are not stored (unknown system, README, path escape).
	for _, p := range []string{"cht/Unknown System", "README.md"} {
		if _, err := os.Stat(filepath.Join(svc.DataDir(), "cheat-db", p)); err == nil {
			t.Fatalf("%s was stored", p)
		}
	}
	if _, err := os.Stat(filepath.Join(svc.DataDir(), "evil.cht")); err == nil {
		t.Fatal("path escape")
	}

	// A broken archive and one without lists keep the stored lists and are recorded for the admin.
	bad := filepath.Join(t.TempDir(), "bad.zip")
	os.WriteFile(bad, []byte("not a zip"), 0o600)
	if err := svc.ImportCheats(ctx, bad); err == nil {
		t.Fatal("broken zip accepted")
	}
	if err := svc.ImportCheats(ctx, writeZip(t, map[string]string{"readme.txt": "x"})); !errors.Is(err, hub.ErrNoCheatLists) {
		t.Fatalf("empty archive: %v", err)
	}
	st, _ = svc.CheatDBStatus(ctx)
	if st.Lists != 3 || st.LastError == "" || st.LastSuccess == nil {
		t.Fatalf("%+v", st)
	}
	if l, err = svc.GameCheats(ctx, admin.ID, demo.ID); err != nil || len(l.Cheats) != 2 {
		t.Fatalf("lists lost: %+v %v", l, err)
	}
	// A new import replaces everything (no old list stays).
	if err := svc.ImportCheats(ctx, writeZip(t, map[string]string{"cht/Nintendo - Nintendo DS/Only.cht": chtOf("x", "1")})); err != nil {
		t.Fatal(err)
	}
	if l, err = svc.GameCheats(ctx, admin.ID, demo.ID); err != nil || len(l.Cheats) != 0 {
		t.Fatalf("old list still served: %+v %v", l, err)
	}
}

func TestCheatDBSettingAndDownload(t *testing.T) {
	var hits atomic.Int32
	var fail atomic.Bool
	srv := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		hits.Add(1)
		if fail.Load() {
			http.Error(w, "boom", http.StatusInternalServerError)
			return
		}
		w.Write(zipOf(t, demoFiles()))
	}))
	defer srv.Close()
	svc, _ := hubtest.New(t, func(o *hub.Options) { o.CoreHTTPClient = srv.Client(); o.CheatDBURL = srv.URL + "/db.zip" })
	admin, err := svc.CreateAdmin(ctx, "admin", "secret-12345")
	if err != nil {
		t.Fatal(err)
	}
	if u, _ := svc.CheatDBURL(ctx); u != srv.URL+"/db.zip" {
		t.Fatalf("flag value not used: %s", u)
	}
	if err := svc.RefreshCheatDB(ctx); err != nil {
		t.Fatal(err)
	}
	st, _ := svc.CheatDBStatus(ctx)
	if st.Source != "libretro-database" || st.Lists != 3 || st.LastError != "" || st.LastCheck == nil {
		t.Fatalf("%+v", st)
	}
	g, err := svc.AddROM(ctx, bytes.NewReader([]byte(demoROM)), "demo.nds", "", "", admin.ID)
	if err != nil {
		t.Fatal(err)
	}
	if l, _ := svc.GameCheats(ctx, admin.ID, g.ID); l.Source != "libretro-database" || len(l.Cheats) != 2 {
		t.Fatalf("%+v", l)
	}

	// A failing download keeps the lists and shows the error.
	fail.Store(true)
	if err := svc.RefreshCheatDB(ctx); err == nil {
		t.Fatal("failed download reported as success")
	}
	st, _ = svc.CheatDBStatus(ctx)
	if st.LastError == "" || st.Lists != 3 {
		t.Fatalf("%+v", st)
	}
	if l, _ := svc.GameCheats(ctx, admin.ID, g.ID); len(l.Cheats) != 2 {
		t.Fatal("lists lost after a failed download")
	}

	// The admin setting wins over the flag; it is validated; "none" switches the refresh off but keeps serving the lists.
	for _, bad := range []string{"", "http://insecure.example/db.zip", "ftp://x/y", "not a url", "https://"} {
		if err := svc.SetCheatDBURL(ctx, bad); err == nil {
			t.Fatalf("%q accepted", bad)
		}
	}
	if err := svc.SetCheatDBURL(ctx, "https://mirror.example/db.zip"); err != nil {
		t.Fatal(err)
	}
	if u, _ := svc.CheatDBURL(ctx); u != "https://mirror.example/db.zip" {
		t.Fatal(u)
	}
	before := hits.Load()
	if err := svc.SetCheatDBURL(ctx, "none"); err != nil {
		t.Fatal(err)
	}
	st, _ = svc.CheatDBStatus(ctx)
	if st.Enabled || st.URL != "none" || st.Lists != 3 {
		t.Fatalf("%+v", st)
	}
	if err := svc.RefreshCheatDB(ctx); !errors.Is(err, hub.ErrCheatDBOff) {
		t.Fatalf("refresh while off: %v", err)
	}
	if hits.Load() != before {
		t.Fatal("a download went out while the database is off")
	}
	if l, _ := svc.GameCheats(ctx, admin.ID, g.ID); len(l.Cheats) != 2 {
		t.Fatal("imported lists must stay usable on an offline server")
	}
}

func TestParseGatewayList(t *testing.T) {
	in := "\xef\xbb\xbf*citra_enabled\n[First]\n*citra_enabled\n00123456 000000ff\nD3000000 00000000\nnot a code\n\n[No code]\n[ ]\n0000AAAA 00000001\n"
	got := hub.ParseGatewayList([]byte(in))
	if len(got) != 2 || got[0].Name != "First" || got[0].Code != "00123456 000000FF\nD3000000 00000000" || got[0].FormatGuess != hub.CheatGateway {
		t.Fatalf("%+v", got)
	}
	if got[1].Name != "Cheat 3" || got[1].Code != "0000AAAA 00000001" {
		t.Fatalf("%+v", got[1])
	}
	if len(hub.ParseGatewayList([]byte("00123456 000000FF\n"))) != 0 {
		t.Fatal("a code before any header is dropped")
	}
}

func TestCheat3DSDownload(t *testing.T) {
	srv := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Write(zipOf(t, map[string]string{
			"pack/sub/0004000000ABCDEF.txt": gatewayOf("From URL", "00123456 000000FF"),
			"pack/readme.txt":               "x",
		}))
	}))
	defer srv.Close()
	svc, _ := hubtest.New(t, func(o *hub.Options) {
		o.CoreHTTPClient = srv.Client()
		o.CheatDBURL = "none"
		o.CheatDB3DSURL = srv.URL + "/3ds.zip"
	})
	admin, _ := svc.CreateAdmin(ctx, "admin", "secret-12345")
	g3, err := svc.AddROM(ctx, bytes.NewReader(ncsdROM(0x0004000000ABCDEF)), "three.3ds", "", "", admin.ID)
	if err != nil {
		t.Fatal(err)
	}
	st, _ := svc.CheatDBStatus(ctx)
	if !st.Enabled || st.URL != "none" || st.URL3DS != srv.URL+"/3ds.zip" {
		t.Fatalf("%+v", st)
	}
	if err := svc.RefreshCheatDB(ctx); err != nil {
		t.Fatal(err)
	}
	if st, _ = svc.CheatDBStatus(ctx); st.Lists3DS != 1 || st.LastError != "" {
		t.Fatalf("%+v", st)
	}
	l, err := svc.GameCheats(ctx, admin.ID, g3.ID)
	if err != nil || l.MatchedBy != "title_id" || l.ListName != "0004000000ABCDEF" || l.Source != "libretro-database" || len(l.Cheats) != 1 || l.Cheats[0].Name != "From URL" {
		t.Fatalf("%+v %v", l, err)
	}
	// An imported archive without a 3ds/ folder does not remove the downloaded 3DS lists; a bad URL is refused.
	if err := svc.SetCheatDB3DSURL(ctx, "http://insecure.example/x.zip"); err == nil {
		t.Fatal("http accepted")
	}
	if err := svc.SetCheatDB3DSURL(ctx, "none"); err != nil {
		t.Fatal(err)
	}
	if err := svc.RefreshCheatDB(ctx); !errors.Is(err, hub.ErrCheatDBOff) {
		t.Fatalf("%v", err)
	}
	if l, _ = svc.GameCheats(ctx, admin.ID, g3.ID); len(l.Cheats) != 1 {
		t.Fatal("downloaded 3DS lists stay served while the source is off")
	}
}

// Regional matching with synthetic names: a game finds the list of its own region by base title; a list of another
// region is never used and only named as a hint.
func TestCheatDBRegionMatching(t *testing.T) {
	svc, _, admin := newAdmin(t)
	const dir = "libretro-database-master/cht/Nintendo - Nintendo DS/"
	if err := svc.ImportCheats(ctx, writeZip(t, map[string]string{
		dir + "Synth Quest (USA) (En,Fr).cht":             chtOf("US only", "02000001 00000001"),
		dir + "Synth Quest (Europe) (En,Fr,De,Es,It).cht": chtOf("EU only", "02000002 00000002"),
		dir + "Lonely Game (USA).cht":                     chtOf("US lonely", "02000003 00000003"),
		dir + "Worldwide Game (World).cht":                chtOf("Everywhere", "02000004 00000004"),
	})); err != nil {
		t.Fatal(err)
	}
	add := func(rom []byte, file, title string) hub.CheatList {
		t.Helper()
		g, err := svc.AddROM(ctx, bytes.NewReader(rom), file, title, "", admin.ID)
		if err != nil {
			t.Fatal(err)
		}
		l, err := svc.GameCheats(ctx, admin.ID, g.ID)
		if err != nil {
			t.Fatal(err)
		}
		return l
	}
	// EU file name without the language tag finds the EU list, not the US one.
	if l := add([]byte("eu one"), "Synth Quest (Europe).nds", ""); l.MatchedBy != "name" || len(l.Cheats) != 1 || l.Cheats[0].Name != "EU only" {
		t.Fatalf("eu by tag: %+v", l)
	}
	// No region in the name: the region letter of the game code decides (P = Europe, E = USA).
	if l := add(ndsROM("ASQP"), "synthquest.nds", "Synth Quest"); len(l.Cheats) != 1 || l.Cheats[0].Name != "EU only" {
		t.Fatalf("eu by code: %+v", l)
	}
	if l := add(ndsROM("ASQE"), "synthquest2.nds", "Synth Quest"); len(l.Cheats) != 1 || l.Cheats[0].Name != "US only" {
		t.Fatalf("us by code: %+v", l)
	}
	// Only another region listed: not assumed, but named.
	l := add([]byte("eu lonely"), "Lonely Game (Europe).nds", "")
	if l.MatchedBy != "" || len(l.Cheats) != 0 || len(l.SimilarLists) != 1 || l.SimilarLists[0] != "Lonely Game (USA)" {
		t.Fatalf("other region used: %+v", l)
	}
	// Unknown region: an explicitly regional list is not assumed either.
	if l := add([]byte("who knows"), "Lonely Game.nds", ""); len(l.Cheats) != 0 || len(l.SimilarLists) != 1 {
		t.Fatalf("unknown region: %+v", l)
	}
	// A World list fits every region.
	if l := add([]byte("w"), "Worldwide Game (Japan).nds", ""); len(l.Cheats) != 1 || l.Cheats[0].Name != "Everywhere" {
		t.Fatalf("world: %+v", l)
	}
}
