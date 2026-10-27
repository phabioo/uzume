package hub_test

import (
	"bytes"
	"context"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/phabioo/uzume/server/internal/hub"
	"github.com/phabioo/uzume/server/internal/hub/hubtest"
	"github.com/phabioo/uzume/server/internal/pluginpkg"
)

func pluginHub(t *testing.T, mod func(*hub.Options)) *hub.Service {
	t.Helper()
	svc, _ := hubtest.New(t, func(o *hub.Options) {
		o.CoreTrustKeys = hubtest.PluginKeys()
		if mod != nil {
			mod(o)
		}
	})
	return svc
}

func stage(t *testing.T, svc *hub.Service, data []byte) hub.PluginStaged {
	t.Helper()
	st, err := svc.StagePluginUpload(ctx, bytes.NewReader(data))
	if err != nil {
		t.Fatal(err)
	}
	return st
}

func TestPluginUploadUnverifiedNeedsEveryTick(t *testing.T) {
	svc := pluginHub(t, nil)
	admin, _ := svc.CreateAdmin(ctx, "fabio", "secret-12345")
	data := hubtest.PluginZip(t, "net.example.notes", "0.2.0", []string{"ui", "memory.read"}, nil)
	st := stage(t, svc, data)
	if st.Official || st.Signed || st.Name != "Plugin net.example.notes" || len(st.Permissions) != 2 || st.AlreadyOffered || st.Conflict {
		t.Fatalf("staged %+v", st)
	}
	for _, ticks := range [][]string{nil, {"ui"}, {"memory.read"}, {"other", "x"}} {
		if _, err := svc.OfferStagedPlugin(ctx, st.StageID, ticks, admin.ID); !errors.Is(err, hub.ErrBadRequest) {
			t.Fatalf("ticks %v: %v", ticks, err)
		}
	}
	if ps, _ := svc.ListPlugins(ctx); len(ps) != 0 {
		t.Fatal("offered without all ticks")
	}
	p, err := svc.OfferStagedPlugin(ctx, st.StageID, []string{"memory.read", "ui"}, admin.ID)
	if err != nil {
		t.Fatal(err)
	}
	if p.Origin != hub.PluginOriginUnverified || p.Source != hub.PluginSourceUpload || p.UploadedBy != admin.ID || p.UploadedByName == "" ||
		p.SHA256 != pluginpkg.SHA256Hex(data) || p.Size != int64(len(data)) || p.Name == "" || p.Manifest.Provides[0] != "test.iface" ||
		p.Manifest.Requires["net.example.base"] != "^1.0" || len(p.Manifest.Games) != 1 {
		t.Fatalf("plugin %+v", p)
	}
	// The staged file is gone, the stored file is byte-identical.
	if _, err := svc.OfferStagedPlugin(ctx, st.StageID, []string{"ui", "memory.read"}, admin.ID); err == nil {
		t.Fatal("stage reused")
	}
	f, _, err := svc.OpenPluginFile(ctx, "net.example.notes", "0.2.0")
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	var got bytes.Buffer
	got.ReadFrom(f)
	if !bytes.Equal(got.Bytes(), data) {
		t.Fatal("stored file differs")
	}
	if _, _, err := svc.OpenPluginFile(ctx, "net.example.notes", "9.9.9"); !errors.Is(err, hub.ErrNotFound) {
		t.Fatalf("unknown version: %v", err)
	}
	if _, _, err := svc.OpenPluginFile(ctx, "../etc", "1.0.0"); !errors.Is(err, hub.ErrNotFound) {
		t.Fatalf("path id: %v", err)
	}
}

func TestPluginUploadSignedIsOfficialAndBadSignatureUnverified(t *testing.T) {
	svc := pluginHub(t, nil)
	raw := hubtest.PluginZip(t, "net.example.signed", "1.0.0", nil, nil)
	st := stage(t, svc, hubtest.SignPlugin(t, raw))
	if !st.Official || !st.Signed || st.SignatureProblem != "" {
		t.Fatalf("%+v", st)
	}
	// No permissions: nothing to tick.
	p, err := svc.OfferStagedPlugin(ctx, st.StageID, nil, "")
	if err != nil || p.Origin != hub.PluginOriginOfficial {
		t.Fatalf("%v %+v", err, p)
	}
	// A SIGNATURE.json that does not verify: reported, and offered as unverified only.
	bad := hubtest.PluginZip(t, "net.example.other", "1.0.0", nil, map[string]string{"SIGNATURE.json": `{"format":1,"files":{},"signature":"AAAA"}`})
	st = stage(t, svc, bad)
	if st.Official || !st.Signed || st.SignatureProblem == "" {
		t.Fatalf("%+v", st)
	}
	p, err = svc.OfferStagedPlugin(ctx, st.StageID, nil, "")
	if err != nil || p.Origin != hub.PluginOriginUnverified {
		t.Fatalf("%v %+v", err, p)
	}
}

func TestPluginUploadRules(t *testing.T) {
	svc := pluginHub(t, nil)
	if _, err := svc.StagePluginUpload(ctx, strings.NewReader("not a zip")); !errors.Is(err, hub.ErrBadRequest) {
		t.Fatalf("garbage: %v", err)
	}
	if _, err := svc.StagePluginUpload(ctx, bytes.NewReader(make([]byte, pluginpkg.MaxCompressedBytes+1))); !errors.Is(err, hub.ErrPayloadTooLarge) {
		t.Fatalf("too large: %v", err)
	}
	for _, id := range []string{"", "../x", strings.Repeat("a", 32), "ZZ" + strings.Repeat("a", 30)} {
		if _, err := svc.StagedPlugin(ctx, id); !errors.Is(err, hub.ErrBadRequest) {
			t.Fatalf("stage id %q: %v", id, err)
		}
	}
	a := hubtest.PluginZip(t, "net.example.same", "1.0.0", nil, nil)
	b := hubtest.PluginZip(t, "net.example.same", "1.0.0", nil, map[string]string{"extra.txt": "different"})
	p1, err := svc.OfferStagedPlugin(ctx, stage(t, svc, a).StageID, nil, "")
	if err != nil {
		t.Fatal(err)
	}
	// Same file again: fine, nothing changes. A different file with the same id+version is refused.
	st := stage(t, svc, a)
	if !st.AlreadyOffered {
		t.Fatalf("%+v", st)
	}
	if p2, err := svc.OfferStagedPlugin(ctx, st.StageID, nil, ""); err != nil || p2.SHA256 != p1.SHA256 {
		t.Fatalf("repeat: %v", err)
	}
	st = stage(t, svc, b)
	if !st.Conflict {
		t.Fatalf("%+v", st)
	}
	if _, err := svc.OfferStagedPlugin(ctx, st.StageID, nil, ""); !errors.Is(err, hub.ErrConflict) || !strings.Contains(err.Error(), "different file") {
		t.Fatalf("conflict: %v", err)
	}
	if ps, _ := svc.ListPlugins(ctx); len(ps) != 1 || ps[0].SHA256 != p1.SHA256 {
		t.Fatalf("plugins %+v", ps)
	}
}

func TestPluginRemoveAndInUseHook(t *testing.T) {
	svc := pluginHub(t, nil)
	for _, v := range []string{"1.10.0", "1.2.0", "1.2.0-rc.1"} {
		if _, err := svc.OfferStagedPlugin(ctx, stage(t, svc, hubtest.PluginZip(t, "net.example.rm", v, nil, nil)).StageID, nil, ""); err != nil {
			t.Fatal(err)
		}
	}
	ps, _ := svc.ListPlugins(ctx)
	if len(ps) != 3 || ps[0].Version != "1.2.0-rc.1" || ps[1].Version != "1.2.0" || ps[2].Version != "1.10.0" {
		t.Fatalf("order %+v", ps)
	}
	if used, err := svc.PluginVersionInUse(ctx, "net.example.rm", "1.2.0"); err != nil || used {
		t.Fatalf("default hook: %v %v", used, err)
	}
	svc.SetPluginInUseCheck(func(_ context.Context, id, v string) (bool, error) { return v == "1.2.0", nil })
	if err := svc.RemovePlugin(ctx, "net.example.rm", "1.2.0"); !errors.Is(err, hub.ErrConflict) {
		t.Fatalf("in use: %v", err)
	}
	file := filepath.Join(svc.DataDir(), "plugins", "net.example.rm", "1.2.0.uzplugin")
	if _, err := os.Stat(file); err != nil {
		t.Fatal("file removed although in use")
	}
	if err := svc.RemovePlugin(ctx, "net.example.rm", "1.10.0"); err != nil {
		t.Fatal(err)
	}
	if err := svc.RemovePlugin(ctx, "net.example.rm", "1.10.0"); !errors.Is(err, hub.ErrNotFound) {
		t.Fatalf("again: %v", err)
	}
	if err := svc.RemovePlugin(ctx, "../x", "1.0.0"); !errors.Is(err, hub.ErrNotFound) {
		t.Fatalf("bad id: %v", err)
	}
	svc.SetPluginInUseCheck(func(context.Context, string, string) (bool, error) { return false, nil })
	for _, v := range []string{"1.2.0", "1.2.0-rc.1"} {
		if err := svc.RemovePlugin(ctx, "net.example.rm", v); err != nil {
			t.Fatal(err)
		}
	}
	if _, err := os.Stat(filepath.Join(svc.DataDir(), "plugins", "net.example.rm")); err == nil {
		t.Fatal("empty plugin folder left behind")
	}
}

func TestImportPlugins(t *testing.T) {
	svc := pluginHub(t, nil)
	dir := t.TempDir()
	write := func(name string, data []byte) { os.WriteFile(filepath.Join(dir, name), data, 0o644) }
	write("a.uzplugin", hubtest.SignPlugin(t, hubtest.PluginZip(t, "net.example.a", "1.0.0", []string{"ui"}, nil)))
	write("b.FBPLUGIN", hubtest.PluginZip(t, "net.example.b", "1.0.0", nil, nil))
	write("broken.uzplugin", []byte("nope"))
	write("notes.txt", []byte("ignored"))
	sum, err := svc.ImportPlugins(ctx, dir)
	if err != nil || sum.Added != 2 || sum.Unchanged != 0 || len(sum.Problems) != 1 || !strings.HasPrefix(sum.Problems[0], "broken.uzplugin") {
		t.Fatalf("%v %+v", err, sum)
	}
	ps, _ := svc.ListPlugins(ctx)
	if len(ps) != 2 || ps[0].Origin != hub.PluginOriginOfficial || ps[1].Origin != hub.PluginOriginUnverified || ps[0].Source != hub.PluginSourceImport {
		t.Fatalf("%+v", ps)
	}
	sum, _ = svc.ImportPlugins(ctx, dir)
	if sum.Added != 0 || sum.Unchanged != 2 {
		t.Fatalf("repeat %+v", sum)
	}
	// A different file with an offered version is a problem, not an overwrite.
	write("a.uzplugin", hubtest.SignPlugin(t, hubtest.PluginZip(t, "net.example.a", "1.0.0", []string{"ui"}, map[string]string{"x": "y"})))
	sum, _ = svc.ImportPlugins(ctx, dir)
	if len(sum.Problems) != 2 || !strings.Contains(strings.Join(sum.Problems, "\n"), "different file") {
		t.Fatalf("conflict %+v", sum)
	}
	if _, err := svc.ImportPlugins(ctx, filepath.Join(dir, "missing")); err == nil {
		t.Fatal("missing dir accepted")
	}
}

func TestPluginIndexRefreshAndInstall(t *testing.T) {
	dir := t.TempDir()
	v1 := hubtest.SignPlugin(t, hubtest.PluginZip(t, "net.example.idx", "1.0.0", []string{"ui"}, nil))
	v2 := hubtest.SignPlugin(t, hubtest.PluginZip(t, "net.example.idx", "1.1.0", []string{"ui"}, nil))
	other := hubtest.SignPlugin(t, hubtest.PluginZip(t, "net.example.other", "0.1.0", nil, nil))
	url := hubtest.PluginIndex(t, dir, false, v1, other)
	svc := pluginHub(t, func(o *hub.Options) { o.PluginIndexURL = url })

	st, err := svc.PluginIndexStatus(ctx)
	if err != nil || !st.Configured || len(st.Catalog) != 0 || st.LastCheck != nil {
		t.Fatalf("before refresh: %v %+v", err, st)
	}
	if err := svc.RefreshPluginIndex(ctx); err != nil {
		t.Fatal(err)
	}
	st, _ = svc.PluginIndexStatus(ctx)
	if st.LastCheck == nil || st.LastError != "" || len(st.Catalog) != 2 || st.Catalog[0].ID != "net.example.idx" || st.Catalog[0].State != hub.CatalogInstall {
		t.Fatalf("after refresh: %+v", st)
	}
	p, err := svc.InstallIndexPlugin(ctx, "net.example.idx", "1.0.0")
	if err != nil || p.Origin != hub.PluginOriginOfficial || p.Source != hub.PluginSourceIndex || p.UploadedBy != "" {
		t.Fatalf("%v %+v", err, p)
	}
	st, _ = svc.PluginIndexStatus(ctx)
	if st.Catalog[0].State != hub.CatalogInstalled || st.Catalog[0].InstalledVersion != "1.0.0" {
		t.Fatalf("%+v", st.Catalog[0])
	}
	// A newer version appears in the index: Update; the old version stays offered.
	hubtest.PluginIndex(t, dir, false, v1, v2, other)
	if err := svc.RefreshPluginIndex(ctx); err != nil {
		t.Fatal(err)
	}
	st, _ = svc.PluginIndexStatus(ctx)
	if st.Catalog[0].State != hub.CatalogUpdate || st.Catalog[0].Version != "1.1.0" || st.Catalog[0].InstalledVersion != "1.0.0" {
		t.Fatalf("%+v", st.Catalog[0])
	}
	if _, err := svc.InstallIndexPlugin(ctx, "net.example.idx", "1.1.0"); err != nil {
		t.Fatal(err)
	}
	// The newer version supersedes the old one (nothing uses it).
	if ps, _ := svc.ListPlugins(ctx); len(ps) != 1 || ps[0].Version != "1.1.0" {
		t.Fatalf("%+v", ps)
	}
	if _, err := svc.InstallIndexPlugin(ctx, "net.example.idx", "9.9.9"); !errors.Is(err, hub.ErrNotFound) {
		t.Fatalf("unknown: %v", err)
	}
}

func TestPluginIndexRejections(t *testing.T) {
	t.Run("bad index signature", func(t *testing.T) {
		dir := t.TempDir()
		url := hubtest.PluginIndex(t, dir, true, hubtest.SignPlugin(t, hubtest.PluginZip(t, "net.example.a", "1.0.0", nil, nil)))
		svc := pluginHub(t, func(o *hub.Options) { o.PluginIndexURL = url })
		err := svc.RefreshPluginIndex(ctx)
		if err == nil || !strings.Contains(err.Error(), "signature rejected") {
			t.Fatalf("%v", err)
		}
		st, _ := svc.PluginIndexStatus(ctx)
		if st.LastError == "" || len(st.Catalog) != 0 || st.LastCheck == nil {
			t.Fatalf("%+v", st)
		}
	})
	t.Run("failure keeps the previous entries", func(t *testing.T) {
		dir := t.TempDir()
		url := hubtest.PluginIndex(t, dir, false, hubtest.SignPlugin(t, hubtest.PluginZip(t, "net.example.a", "1.0.0", nil, nil)))
		svc := pluginHub(t, func(o *hub.Options) { o.PluginIndexURL = url })
		if err := svc.RefreshPluginIndex(ctx); err != nil {
			t.Fatal(err)
		}
		os.Remove(filepath.Join(dir, "plugin-index.json.sig"))
		if err := svc.RefreshPluginIndex(ctx); err == nil {
			t.Fatal("missing signature accepted")
		}
		if st, _ := svc.PluginIndexStatus(ctx); len(st.Catalog) != 1 || st.LastError == "" {
			t.Fatalf("%+v", st)
		}
	})
	t.Run("no index configured", func(t *testing.T) {
		svc := pluginHub(t, nil)
		if err := svc.RefreshPluginIndex(ctx); !errors.Is(err, hub.ErrPluginIndexOff) {
			t.Fatalf("%v", err)
		}
		if st, _ := svc.PluginIndexStatus(ctx); st.Configured {
			t.Fatal("configured")
		}
		if _, err := svc.InstallIndexPlugin(ctx, "net.example.a", "1.0.0"); !errors.Is(err, hub.ErrPluginIndexOff) {
			t.Fatalf("%v", err)
		}
	})
	t.Run("download problems", func(t *testing.T) {
		dir := t.TempDir()
		good := hubtest.SignPlugin(t, hubtest.PluginZip(t, "net.example.a", "1.0.0", nil, nil))
		url := hubtest.PluginIndex(t, dir, false, good)
		svc := pluginHub(t, func(o *hub.Options) { o.PluginIndexURL = url })
		if err := svc.RefreshPluginIndex(ctx); err != nil {
			t.Fatal(err)
		}
		file := filepath.Join(dir, "plugin-0.uzplugin")
		// Same size, other content: SHA-256 pin fails.
		changed := bytes.Replace(good, []byte("manifest.json"), []byte("manifesT.json"), 1)
		os.WriteFile(file, changed, 0o644)
		if _, err := svc.InstallIndexPlugin(ctx, "net.example.a", "1.0.0"); err == nil || !strings.Contains(err.Error(), "SHA-256") {
			t.Fatalf("pin: %v", err)
		}
		// Different size.
		os.WriteFile(file, append(append([]byte{}, good...), 0), 0o644)
		if _, err := svc.InstallIndexPlugin(ctx, "net.example.a", "1.0.0"); err == nil || !strings.Contains(err.Error(), "size") {
			t.Fatalf("size: %v", err)
		}
		if ps, _ := svc.ListPlugins(ctx); len(ps) != 0 {
			t.Fatal("installed despite errors")
		}
	})
	t.Run("unsigned file behind a valid index entry", func(t *testing.T) {
		dir := t.TempDir()
		// The index entry is built from the unsigned file (a broken publisher), the index itself is validly signed.
		unsigned := hubtest.PluginZip(t, "net.example.u", "1.0.0", nil, nil)
		url := hubtest.PluginIndex(t, dir, false, unsigned)
		svc := pluginHub(t, func(o *hub.Options) { o.PluginIndexURL = url })
		if err := svc.RefreshPluginIndex(ctx); err != nil {
			t.Fatal(err)
		}
		if _, err := svc.InstallIndexPlugin(ctx, "net.example.u", "1.0.0"); err == nil || !strings.Contains(err.Error(), "not signed") {
			t.Fatalf("%v", err)
		}
	})
	t.Run("signed by an untrusted key", func(t *testing.T) {
		dir := t.TempDir()
		signed := hubtest.SignPlugin(t, hubtest.PluginZip(t, "net.example.k", "1.0.0", nil, nil))
		url := hubtest.PluginIndex(t, dir, false, signed)
		svc, _ := hubtest.New(t, func(o *hub.Options) { o.PluginIndexURL = url }) // test key not trusted
		if err := svc.RefreshPluginIndex(ctx); err == nil {
			t.Fatal("index signed by an untrusted key accepted")
		}
	})
}

// A mix of .uzplugin and legacy .fbplugin (any case) in one folder is imported; the stored name is .uzplugin.
func TestImportPluginsBothExtensions(t *testing.T) {
	svc := pluginHub(t, nil)
	dir := t.TempDir()
	os.WriteFile(filepath.Join(dir, "new.UZPLUGIN"), hubtest.PluginZip(t, "net.example.new", "1.0.0", nil, nil), 0o644)
	os.WriteFile(filepath.Join(dir, "old.fbplugin"), hubtest.PluginZip(t, "net.example.old", "1.0.0", nil, nil), 0o644)
	sum, err := svc.ImportPlugins(ctx, dir)
	if err != nil || sum.Added != 2 || len(sum.Problems) != 0 {
		t.Fatalf("%v %+v", err, sum)
	}
	for _, id := range []string{"net.example.new", "net.example.old"} {
		if _, err := os.Stat(filepath.Join(svc.DataDir(), "plugins", id, "1.0.0.uzplugin")); err != nil {
			t.Fatalf("%s: %v", id, err)
		}
	}
}

// Files stored by an older server (<version>.fbplugin) are renamed at start, once; an existing new name wins.
func TestLegacyPluginFilesMigrated(t *testing.T) {
	dir := t.TempDir()
	mod := func(o *hub.Options) { o.CoreTrustKeys = hubtest.PluginKeys() }
	svc, _, _ := hubtest.OpenIn(t, dir, mod)
	src := t.TempDir()
	for _, id := range []string{"net.example.mig", "net.example.both"} {
		os.WriteFile(filepath.Join(src, id+".uzplugin"), hubtest.PluginZip(t, id, "1.0.0", nil, nil), 0o644)
	}
	if sum, err := svc.ImportPlugins(ctx, src); err != nil || sum.Added != 2 {
		t.Fatalf("%v %+v", err, sum)
	}
	mig, both := filepath.Join(dir, "plugins", "net.example.mig"), filepath.Join(dir, "plugins", "net.example.both")
	// mig: only the legacy name; both: both names (the new one must not be overwritten).
	if err := os.Rename(filepath.Join(mig, "1.0.0.uzplugin"), filepath.Join(mig, "1.0.0.fbplugin")); err != nil {
		t.Fatal(err)
	}
	os.WriteFile(filepath.Join(both, "1.0.0.fbplugin"), []byte("stale"), 0o644)
	for i := 0; i < 2; i++ { // second start: idempotent
		svc, _, _ = hubtest.OpenIn(t, dir, mod)
		if _, err := os.Stat(filepath.Join(mig, "1.0.0.uzplugin")); err != nil {
			t.Fatalf("start %d: %v", i, err)
		}
		if _, err := os.Stat(filepath.Join(mig, "1.0.0.fbplugin")); err == nil {
			t.Fatalf("start %d: legacy file left", i)
		}
		if b, _ := os.ReadFile(filepath.Join(both, "1.0.0.uzplugin")); string(b) == "stale" || len(b) == 0 {
			t.Fatal("existing new file overwritten")
		}
	}
	f, _, err := svc.OpenPluginFile(ctx, "net.example.mig", "1.0.0")
	if err != nil {
		t.Fatal(err)
	}
	f.Close()
	// Removing a version removes both names.
	if err := svc.RemovePlugin(ctx, "net.example.both", "1.0.0"); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(both, "1.0.0.fbplugin")); err == nil {
		t.Fatal("legacy file of a removed version left")
	}
}
