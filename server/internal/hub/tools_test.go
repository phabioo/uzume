package hub_test

import (
	"bytes"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/phabioo/uzume/server/internal/hub"
	"github.com/phabioo/uzume/server/internal/hub/hubtest"
)

var allToolPlatforms = []string{"windows-x64", "linux-x64", "linux-arm64"}

func toolHub(t *testing.T, src *hubtest.ToolSource) *hub.Service {
	t.Helper()
	svc, _ := hubtest.New(t, func(o *hub.Options) {
		if src != nil {
			src.Apply(o)
		}
	})
	return svc
}

func shaHex(b []byte) string { h := sha256.Sum256(b); return hex.EncodeToString(h[:]) }

func toolPlatform(t *testing.T, tool hub.Tool, version, platform string) hub.ToolPlatform {
	t.Helper()
	for _, v := range tool.Versions {
		if v.Version != version {
			continue
		}
		for _, p := range v.Platforms {
			if p.Platform == platform {
				return p
			}
		}
	}
	t.Fatalf("no %s %s in %+v", version, platform, tool.Versions)
	return hub.ToolPlatform{}
}

func TestToolSyncReadsReleases(t *testing.T) {
	src := hubtest.NewToolSource(t)
	src.AddRelease(t, "1.6.1", false, "windows-x64")
	src.AddRelease(t, "1.6.2", false, allToolPlatforms...)
	src.AddRelease(t, "1.7.0", true, allToolPlatforms...) // prerelease: skipped
	src.AddDraft("1.8.0")                                 // draft: skipped
	svc := toolHub(t, src)

	tool, err := svc.GetTool(ctx, hub.ToolRandomizer)
	if err != nil || tool.Offered || len(tool.Versions) != 0 || tool.Source.State != hub.ToolSourceNever || tool.License != "GPL-3.0" ||
		tool.SourceURL != "https://github.com/upr-fvx/universal-pokemon-randomizer-fvx" {
		t.Fatalf("%+v %v", tool, err)
	}
	tool, err = svc.SyncTool(ctx, hub.ToolRandomizer)
	if err != nil {
		t.Fatal(err)
	}
	if tool.Offered || tool.LatestVersion != "1.6.2" || tool.Source.State != hub.ToolSourceOK || tool.Source.LastSuccess == nil || len(tool.Versions) != 2 ||
		tool.Versions[0].Version != "1.6.2" || tool.Versions[1].Version != "1.6.1" || len(tool.Versions[0].Platforms) != 3 || len(tool.Versions[1].Platforms) != 1 {
		t.Fatalf("%+v", tool)
	}
	if !tool.UpdateAvailable() || tool.NewestCached() != "" {
		t.Fatalf("nothing is cached yet: %+v", tool)
	}
	for _, v := range tool.Versions {
		if v.Installed || v.Complete {
			t.Fatalf("nothing downloaded by sync: %+v", v)
		}
	}
	if src.Hits["/dl/1.6.2/"+hubtest.ToolAssetName("1.6.2", "linux-x64")] != 0 {
		t.Fatal("sync must not download")
	}
	// Not offered: apps see nothing.
	if ts, _ := svc.AppTools(ctx); len(ts) != 0 {
		t.Fatalf("%+v", ts)
	}
	if _, err := svc.SyncTool(ctx, "nope"); !errors.Is(err, hub.ErrNotFound) {
		t.Fatalf("%v", err)
	}

	// A failing source is recorded and shown, and the earlier versions stay.
	src.Down = true
	if _, err := svc.SyncTool(ctx, hub.ToolRandomizer); err == nil {
		t.Fatal("down source")
	}
	tool, _ = svc.GetTool(ctx, hub.ToolRandomizer)
	if tool.Source.State != hub.ToolSourceError || !strings.Contains(tool.Source.LastError, "503") || len(tool.Versions) != 2 {
		t.Fatalf("%+v", tool.Source)
	}
}

func TestToolSourceOff(t *testing.T) {
	svc := toolHub(t, nil)
	if _, err := svc.SyncTool(ctx, hub.ToolRandomizer); !errors.Is(err, hub.ErrToolSourceOff) {
		t.Fatalf("%v", err)
	}
	tool, _ := svc.GetTool(ctx, hub.ToolRandomizer)
	if tool.Source.State != hub.ToolSourceOff {
		t.Fatalf("%+v", tool.Source)
	}
}

func TestToolInstallPinsHashAndRefusesChange(t *testing.T) {
	src := hubtest.NewToolSource(t)
	src.AddRelease(t, "1.6.2", false, allToolPlatforms...)
	svc := toolHub(t, src)
	if _, err := svc.SyncTool(ctx, hub.ToolRandomizer); err != nil {
		t.Fatal(err)
	}
	tool, err := svc.InstallToolVersion(ctx, hub.ToolRandomizer, "1.6.2")
	if err != nil {
		t.Fatal(err)
	}
	v := tool.Versions[0]
	if !v.Installed || !v.Complete || tool.Offered || tool.NewestCached() != "1.6.2" || tool.UpdateAvailable() {
		t.Fatalf("%+v", tool)
	}
	for _, pl := range allToolPlatforms {
		p := toolPlatform(t, tool, "1.6.2", pl)
		want := src.Zip("1.6.2", pl)
		if !p.Cached || p.SHA256 != shaHex(want) || p.Size != int64(len(want)) || p.Origin != hub.ToolOriginGitHub {
			t.Fatalf("%s: %+v", pl, p)
		}
		wantRoot := ""
		if pl == "linux-x64" {
			wantRoot = "UPR_FVX/" // the zip nests everything below one folder
		}
		if p.Root != wantRoot {
			t.Fatalf("%s root %q", pl, p.Root)
		}
		b, err := os.ReadFile(filepath.Join(svc.DataDir(), "tools", "randomizer", "1.6.2", pl+".zip"))
		if err != nil || !bytes.Equal(b, want) {
			t.Fatalf("stored file %s: %v", pl, err)
		}
	}
	// A second install downloads nothing.
	hits := src.Hits["/dl/1.6.2/"+hubtest.ToolAssetName("1.6.2", "linux-x64")]
	if _, err := svc.InstallToolVersion(ctx, hub.ToolRandomizer, "1.6.2"); err != nil || src.Hits["/dl/1.6.2/"+hubtest.ToolAssetName("1.6.2", "linux-x64")] != hits {
		t.Fatalf("reinstall: %v", err)
	}
	if _, err := svc.InstallToolVersion(ctx, hub.ToolRandomizer, "9.9.9"); !errors.Is(err, hub.ErrNotFound) {
		t.Fatalf("unknown version: %v", err)
	}

	// Offering is the admin's decision; apps then see cached platforms only.
	if _, _, err := svc.OpenToolFile(ctx, hub.ToolRandomizer, "1.6.2", "linux-x64"); !errors.Is(err, hub.ErrNotFound) {
		t.Fatalf("not offered must be 404: %v", err)
	}
	if _, err := svc.SetToolOffered(ctx, hub.ToolRandomizer, true); err != nil {
		t.Fatal(err)
	}
	ts, _ := svc.AppTools(ctx)
	if len(ts) != 1 || !ts[0].Offered || ts[0].NewestCached() != "1.6.2" || len(ts[0].Versions[0].Platforms) != 3 {
		t.Fatalf("%+v", ts)
	}
	f, pl, err := svc.OpenToolFile(ctx, hub.ToolRandomizer, "1.6.2", "windows-x64")
	if err != nil {
		t.Fatal(err)
	}
	var got bytes.Buffer
	got.ReadFrom(f)
	f.Close()
	if !bytes.Equal(got.Bytes(), src.Zip("1.6.2", "windows-x64")) || pl.SHA256 != shaHex(got.Bytes()) {
		t.Fatal("served file differs")
	}
	for _, bad := range [][3]string{{"nope", "1.6.2", "linux-x64"}, {"randomizer", "1.6.3", "linux-x64"}, {"randomizer", "1.6.2", "macos-arm64"}, {"randomizer", "../x", "linux-x64"}} {
		if _, _, err := svc.OpenToolFile(ctx, bad[0], bad[1], bad[2]); !errors.Is(err, hub.ErrNotFound) {
			t.Fatalf("%v: %v", bad, err)
		}
	}

	// The cache loses a file (restored backup, cleaned folder): the platform counts as not cached, the pin stays, and a
	// download with other content is refused.
	path := filepath.Join(svc.DataDir(), "tools", "randomizer", "1.6.2", "windows-x64.zip")
	orig := src.Zip("1.6.2", "windows-x64")
	if err := os.Remove(path); err != nil {
		t.Fatal(err)
	}
	if tool, _ = svc.GetTool(ctx, hub.ToolRandomizer); toolPlatform(t, tool, "1.6.2", "windows-x64").Cached {
		t.Fatal("missing file must not count as cached")
	}
	src.Replace("1.6.2", "windows-x64", hubtest.ToolZip(t, "windows-x64", false, "tampered"))
	_, err = svc.InstallToolVersion(ctx, hub.ToolRandomizer, "1.6.2")
	var he *hub.Error
	if !errors.As(err, &he) || he.Code != hub.CodeToolHashMismatch {
		t.Fatalf("want tool_hash_mismatch: %v", err)
	}
	if _, serr := os.Stat(path); serr == nil {
		t.Fatal("the changed file must not be stored")
	}
	tool, _ = svc.GetTool(ctx, hub.ToolRandomizer)
	if p := toolPlatform(t, tool, "1.6.2", "windows-x64"); p.SHA256 != shaHex(orig) || p.Cached {
		t.Fatalf("pin must stay: %+v", p)
	}
	src.Replace("1.6.2", "windows-x64", orig)
	if tool, err = svc.InstallToolVersion(ctx, hub.ToolRandomizer, "1.6.2"); err != nil || !toolPlatform(t, tool, "1.6.2", "windows-x64").Cached {
		t.Fatalf("the original file installs again: %v", err)
	}
}

// noise returns incompressible bytes.
func noise(t *testing.T, n int) []byte {
	t.Helper()
	b := make([]byte, n)
	if _, err := rand.Read(b); err != nil {
		t.Fatal(err)
	}
	return b
}

func TestToolInstallRejectsNonEngineAndOversizedZips(t *testing.T) {
	src := hubtest.NewToolSource(t)
	src.AddRelease(t, "1.6.2", false, "linux-x64", "windows-x64")
	svc, _ := hubtest.New(t, func(o *hub.Options) { src.Apply(o); o.ToolMaxZipBytes = 10 << 10 })
	if _, err := svc.SyncTool(ctx, hub.ToolRandomizer); err != nil {
		t.Fatal(err)
	}
	for name, zip := range map[string][]byte{
		"no jar":        hubtest.MakeZip(t, map[string][]byte{"java/bin/java": []byte("x")}),
		"no java":       hubtest.MakeZip(t, map[string][]byte{"UPR-FVX.jar": []byte("x")}),
		"jar too deep":  hubtest.MakeZip(t, map[string][]byte{"a/b/UPR-FVX.jar": []byte("x"), "a/b/java/bin/java": []byte("x")}),
		"mixed folders": hubtest.MakeZip(t, map[string][]byte{"a/UPR-FVX.jar": []byte("x"), "b/java/bin/java": []byte("x")}),
		"unsafe name":   hubtest.MakeZip(t, map[string][]byte{"UPR-FVX.jar": []byte("x"), "java/bin/java": []byte("x"), "../evil": []byte("x")}),
		"not a zip":     []byte("plain text"),
		"too large":     hubtest.MakeZip(t, map[string][]byte{"UPR-FVX.jar": noise(t, 11<<10), "java/bin/java": []byte("x")}),
	} {
		src.Replace("1.6.2", "linux-x64", zip)
		if _, err := svc.InstallToolVersion(ctx, hub.ToolRandomizer, "1.6.2"); err == nil {
			t.Fatalf("%s: installed", name)
		}
		if tool, _ := svc.GetTool(ctx, hub.ToolRandomizer); toolPlatform(t, tool, "1.6.2", "linux-x64").Cached || toolPlatform(t, tool, "1.6.2", "linux-x64").SHA256 != "" {
			t.Fatalf("%s: stored or pinned", name)
		}
	}
	// A root level layout is fine as well.
	src.Replace("1.6.2", "linux-x64", hubtest.ToolZip(t, "linux-x64", false, "flat"))
	tool, err := svc.InstallToolVersion(ctx, hub.ToolRandomizer, "1.6.2")
	if err != nil || toolPlatform(t, tool, "1.6.2", "linux-x64").Root != "" || !toolPlatform(t, tool, "1.6.2", "windows-x64").Cached {
		t.Fatalf("%v %+v", err, tool.Versions)
	}
}

func TestToolImportFiles(t *testing.T) {
	svc := toolHub(t, nil)
	dir := t.TempDir()
	write := func(name string, b []byte) string {
		p := filepath.Join(dir, name)
		if err := os.WriteFile(p, b, 0o600); err != nil {
			t.Fatal(err)
		}
		return p
	}
	win := hubtest.ToolZip(t, "windows-x64", false, "w")
	lin := hubtest.ToolZip(t, "linux-x64", true, "l")
	arm := hubtest.ToolZip(t, "linux-arm64", false, "a")
	files := []string{
		write("UPR_FVX-v1_6_2-Windows.zip", win),
		write("UPR_FVX-v1_6_2-Linux_x86.zip", lin),
		write("UPR_FVX-v1_6_2-Linux_ARM.zip", arm),
		write("randomizer.zip", win),                                                           // name does not follow the pattern
		write("UPR_FVX-v1_6_3-Windows.zip", []byte("not a zip")),                               // content check
		write("UPR_FVX-v1_6_4-Linux_x86.zip", hubtest.MakeZip(t, map[string][]byte{"x": nil})), // not the engine
		filepath.Join(dir, "UPR_FVX-v1_6_5-Windows.zip"),                                       // missing
	}
	res := svc.ImportToolFiles(ctx, files)
	status := []string{"added", "added", "added", "rejected", "rejected", "rejected", "rejected"}
	for i, r := range res {
		if r.Status != status[i] {
			t.Fatalf("%d %+v", i, r)
		}
	}
	if res[0].Version != "1.6.2" || res[0].Platform != "windows-x64" || res[1].Platform != "linux-x64" || res[2].Platform != "linux-arm64" || res[0].ToolID != "randomizer" {
		t.Fatalf("%+v", res[:3])
	}
	tool, _ := svc.GetTool(ctx, hub.ToolRandomizer)
	if tool.Offered || len(tool.Versions) != 1 || !tool.Versions[0].Complete {
		t.Fatalf("an import does not offer the tool: %+v", tool)
	}
	if p := toolPlatform(t, tool, "1.6.2", "linux-x64"); p.Origin != hub.ToolOriginImport || p.SHA256 != shaHex(lin) || p.Root != "UPR_FVX/" || !p.Cached {
		t.Fatalf("%+v", p)
	}
	// Importing the same file again changes nothing; other content under the same name is refused (pinned hash).
	again := svc.ImportToolFiles(ctx, files[:1])
	if again[0].Status != "unchanged" {
		t.Fatalf("%+v", again)
	}
	other := write("UPR_FVX-v1_6_2-Windows.zip", hubtest.ToolZip(t, "windows-x64", false, "other"))
	if r := svc.ImportToolFiles(ctx, []string{other})[0]; r.Status != "rejected" || !strings.Contains(r.Err, "pinned") {
		t.Fatalf("%+v", r)
	}
	if _, v, pl, ok := hub.ParseToolFileName("/x/UPR_FVX-v1_6_2-Windows.zip"); !ok || v != "1.6.2" || pl != "windows-x64" {
		t.Fatal("parse")
	}
}

func TestToolRemoveVersion(t *testing.T) {
	src := hubtest.NewToolSource(t)
	src.AddRelease(t, "1.6.2", false, allToolPlatforms...)
	svc := toolHub(t, src)
	admin, g := variantFixture(t, svc, src)
	if _, err := svc.SetToolOffered(ctx, hub.ToolRandomizer, true); err != nil {
		t.Fatal(err)
	}
	dir := filepath.Join(svc.DataDir(), "tools", "randomizer", "1.6.2")
	if _, err := os.Stat(dir); err != nil {
		t.Fatal(err)
	}

	v, err := svc.CreateVariant(ctx, hub.CreateVariantInput{UserID: admin.ID, GameID: g.ID, Name: "Chaos", ToolID: "randomizer", ToolVersion: "1.6.2", Settings: "428abc", Seed: 5})
	if err != nil {
		t.Fatal(err)
	}
	run, err := svc.CreateRun(ctx, hub.CreateRunInput{UserID: admin.ID, Name: "R", GameID: g.ID,
		Randomizer: &hub.RunRandomizerInput{Mode: "same", Settings: "428abc", ToolVersion: "1.6.2"}})
	if err != nil {
		t.Fatal(err)
	}
	err = svc.RemoveToolVersion(ctx, "randomizer", "1.6.2")
	var he *hub.Error
	if !errors.As(err, &he) || he.Code != hub.CodeToolInUse || he.UsedBy == nil || he.UsedBy.Variants != 1 || he.UsedBy.Runs != 1 {
		t.Fatalf("%v", err)
	}
	if tool, _ := svc.GetTool(ctx, "randomizer"); tool.Versions[0].UsedBy.Variants != 1 || tool.Versions[0].UsedBy.Runs != 1 {
		t.Fatalf("%+v", tool.Versions[0])
	}
	// A deleted variant (saves kept) no longer pins the version; a finished run neither.
	if err := svc.DeleteVariant(ctx, admin.ID, "", v.ID, false); err != nil {
		t.Fatal(err)
	}
	if err := svc.RemoveToolVersion(ctx, "randomizer", "1.6.2"); !errors.As(err, &he) || he.UsedBy.Variants != 0 || he.UsedBy.Runs != 1 {
		t.Fatalf("%v", err)
	}
	if _, err := svc.SetRunStatus(ctx, admin.ID, run.ID, "active"); err != nil {
		t.Fatal(err)
	}
	if _, err := svc.SetRunStatus(ctx, admin.ID, run.ID, "finished"); err != nil {
		t.Fatal(err)
	}
	if err := svc.RemoveToolVersion(ctx, "randomizer", "1.6.2"); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(dir); !os.IsNotExist(err) {
		t.Fatal("files must be removed")
	}
	if err := svc.RemoveToolVersion(ctx, "randomizer", "1.6.2"); !errors.Is(err, hub.ErrNotFound) {
		t.Fatalf("%v", err)
	}
	// The kept recipe is not valid for a new variant of a removed version.
	if _, err := svc.CreateVariant(ctx, hub.CreateVariantInput{UserID: admin.ID, GameID: g.ID, Name: "Again", ToolID: "randomizer", ToolVersion: "1.6.2", Settings: "x"}); !errors.Is(err, hub.ErrBadRequest) {
		t.Fatalf("%v", err)
	}
}
