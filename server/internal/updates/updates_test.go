package updates

import (
	"context"
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/phabioo/uzume/server/internal/corepkg"
)

var (
	testSeed = make([]byte, 32)
	t0       = time.Date(2026, 10, 7, 10, 0, 0, 0, time.UTC)
)

func testKeys(t *testing.T) []ed25519.PublicKey {
	t.Helper()
	pub, err := corepkg.PublicFromSeed(testSeed)
	if err != nil {
		t.Fatal(err)
	}
	return []ed25519.PublicKey{pub}
}

const goodSHA = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

func art(platform, kind, name string) Artifact {
	return Artifact{Platform: platform, Kind: kind, Name: name, Size: 10, SHA256: goodSHA, URL: "https://example.org/" + name}
}

func rel(product, channel, version string, arts ...Artifact) Release {
	if arts == nil {
		arts = []Artifact{art("linux-amd64", KindDeb, "uzume-server_"+strings.ReplaceAll(version, "-", "~")+"_amd64.deb")}
	}
	return Release{Product: product, Channel: channel, Version: version, PublishedAt: t0, ProtocolVersion: 1, MinProtocolVersion: 1, Artifacts: arts}
}

func indexJSON(t *testing.T, schema int, rs ...Release) []byte {
	t.Helper()
	b, err := json.Marshal(Index{Schema: schema, GeneratedAt: t0, Releases: rs})
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func TestParseIndex(t *testing.T) {
	idx, errs := ParseIndex(indexJSON(t, 1, rel("hub", "beta", "0.3.0-beta.1")))
	if len(errs) != 0 || len(idx.Releases) != 1 {
		t.Fatalf("%+v %v", idx, errs)
	}
	if _, errs := ParseIndex([]byte(`{"schema":1,"generated_at":"2026-10-07T10:00:00Z","x":1,"releases":[]}`)); len(errs) != 0 {
		t.Fatal("unknown fields must be ignored:", errs)
	}
	if _, errs := ParseIndex(indexJSON(t, 2)); !Fatal(errs) {
		t.Fatal("unknown schema must be fatal")
	}
	dup := indexJSON(t, 1, rel("hub", "beta", "0.3.0-beta.1"), rel("hub", "beta", "0.3.0-beta.1"))
	if _, errs := ParseIndex(dup); !Fatal(errs) {
		t.Fatal("duplicate must reject the whole index")
	}
	if _, errs := ParseIndex([]byte("{")); !Fatal(errs) {
		t.Fatal("bad JSON")
	}
	// Same version in another channel is not a duplicate.
	if _, errs := ParseIndex(indexJSON(t, 1, rel("hub", "beta", "0.3.0"), rel("hub", "stable", "0.3.0"))); len(errs) != 0 {
		t.Fatal(errs)
	}
}

func TestParseIndexSkipsInvalidReleases(t *testing.T) {
	badURL := rel("hub", "stable", "0.3.0")
	badURL.Artifacts[0].URL = "http://example.org/x.deb"
	badSHA := rel("hub", "stable", "0.3.1")
	badSHA.Artifacts[0].SHA256 = "ABC"
	badVer := rel("hub", "stable", "v1")
	badChan := rel("hub", "dev", "0.3.2")
	badPlat := rel("hub", "stable", "0.3.3", art("windows-x64", KindDeb, "a.deb"))
	badProto := rel("hub", "stable", "0.3.4")
	badProto.MinProtocolVersion = 5
	noArt := rel("hub", "stable", "0.3.5")
	noArt.Artifacts = nil
	badProduct := rel("tool", "stable", "0.3.6")
	good := rel("hub", "stable", "0.4.0")
	idx, errs := ParseIndex(indexJSON(t, 1, badURL, badSHA, badVer, badChan, badPlat, badProto, noArt, badProduct, good))
	if Fatal(errs) || len(errs) != 8 || len(idx.Releases) != 1 || idx.Releases[0].Version != "0.4.0" {
		t.Fatalf("errs=%d (%v) releases=%d", len(errs), errs, len(idx.Releases))
	}
	var re *ReleaseError
	if !errors.As(errs[0], &re) {
		t.Fatal("want ReleaseError")
	}
}

func TestSemVerPrecedence(t *testing.T) {
	// Ascending order per the SemVer 2.0 spec section 11 plus Uzume's own forms.
	asc := []string{"0.3.0-beta.2", "0.3.0-beta.10", "0.3.0-beta.10.1", "0.3.0-beta.a", "0.3.0-beta.b", "0.3.0-beta.b.1",
		"0.3.0-beta.b.2", "0.3.0-dev", "0.3.0", "0.3.1", "0.10.0", "1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta", "1.0.0-beta.2",
		"1.0.0-beta.11", "1.0.0-rc.1", "1.0.0", "2.0.0"}
	for i := range asc {
		for j := range asc {
			c, err := CompareVersions(asc[i], asc[j])
			want := 0
			if i < j {
				want = -1
			} else if i > j {
				want = 1
			}
			if err != nil || c != want {
				t.Fatalf("Compare(%s,%s)=%d,%v want %d", asc[i], asc[j], c, err, want)
			}
		}
	}
	if c, _ := CompareVersions("1.0.0+abc", "1.0.0+xyz"); c != 0 {
		t.Fatal("build metadata must be ignored")
	}
	if c, _ := CompareVersions("0.3.0-beta.9", "0.3.0-beta.57"); c != -1 {
		t.Fatal("numeric prerelease identifiers compare numerically")
	}
	for _, bad := range []string{"dev", "1.0", "01.0.0", "1.0.0-01", "1.0.0-", "v1.0.0", "", "1.0.0-a..b"} {
		if ValidSemVer(bad) {
			t.Fatalf("%q must be invalid", bad)
		}
	}
}

func TestSelectChannels(t *testing.T) {
	idx, _ := ParseIndex(indexJSON(t, 1,
		rel("hub", "stable", "0.3.0"),
		rel("hub", "beta", "0.4.0-beta.5"),
		rel("hub", "beta", "0.3.1-beta.9"),
		rel("hub", "stable", "0.2.0"),
		rel("player", "stable", "9.9.9", art("windows-x64", KindInstaller, "setup.exe")),
	))
	q := Query{Product: ProductHub, Platform: "linux-amd64", Kind: KindDeb, Current: "0.2.5"}

	q.Channel = ChannelStable
	s, err := Select(idx, q)
	if err != nil || s.Release == nil || s.Release.Version != "0.3.0" || s.Artifact.Kind != KindDeb {
		t.Fatalf("stable: %+v %v", s, err)
	}
	q.Channel = ChannelBeta // test sees stable too; the highest wins
	if s, _ = Select(idx, q); s.Release == nil || s.Release.Version != "0.4.0-beta.5" {
		t.Fatalf("test: %+v", s)
	}
	q.Current = "0.4.0-beta.5" // equal: up to date
	if s, _ = Select(idx, q); s.Release != nil || !s.UpToDate {
		t.Fatalf("equal: %+v", s)
	}
	q.Current = "0.5.0" // never a downgrade
	if s, _ = Select(idx, q); s.Release != nil || !s.UpToDate {
		t.Fatalf("newer running: %+v", s)
	}
	q.Channel = ChannelDev
	if _, err = Select(idx, q); !errors.Is(err, ErrOff) {
		t.Fatalf("dev: %v", err)
	}
	q.Channel, q.Current = ChannelBeta, "dev"
	if _, err = Select(idx, q); !errors.Is(err, ErrCurrentNotSemVer) {
		t.Fatalf("non-semver current: %v", err)
	}
	// Platform/kind without an artifact: not selected.
	q = Query{Product: ProductHub, Channel: ChannelBeta, Platform: "linux-arm64", Kind: KindDeb, Current: "0.1.0"}
	if s, _ = Select(idx, q); s.Release != nil {
		t.Fatalf("arm64: %+v", s)
	}
	// The test build 0.3.0-beta.x is older than the 0.3.0 release.
	q = Query{Product: ProductHub, Channel: ChannelStable, Platform: "linux-amd64", Kind: KindDeb, Current: "0.3.0-beta.99"}
	if s, _ = Select(idx, q); s.Release == nil || s.Release.Version != "0.3.0" {
		t.Fatalf("prerelease to release: %+v", s)
	}
}

func TestProtocolCompatibility(t *testing.T) {
	cases := []struct {
		cp, cm, p, m int
		want         bool
	}{
		{1, 1, 1, 1, true},
		{2, 2, 1, 1, false}, // candidate needs a newer counterpart
		{2, 1, 1, 1, true},
		{1, 1, 3, 2, false}, // counterpart no longer speaks the candidate's protocol
		{3, 2, 3, 2, true},
	}
	for _, c := range cases {
		if got := ProtocolCompatible(c.cp, c.cm, c.p, c.m); got != c.want {
			t.Errorf("%+v got %v", c, got)
		}
	}
	if !BreaksPlayers(2, []int{1, 2}) || BreaksPlayers(1, []int{1}) || BreaksPlayers(5, nil) {
		t.Fatal("BreaksPlayers")
	}
}

func TestPruneAndMarshalDeterministic(t *testing.T) {
	var rs []Release
	for _, v := range []string{"0.3.0-beta.2", "0.3.0-beta.10", "0.3.0-beta.1", "0.3.0-beta.9", "0.3.0-beta.3", "0.3.0-beta.4", "0.3.0-beta.11"} {
		rs = append(rs, rel("hub", "beta", v))
	}
	rs = append(rs, rel("hub", "stable", "0.2.0"))
	got := Prune(rs, 5)
	var vs []string
	for _, r := range got {
		vs = append(vs, r.Channel+":"+r.Version)
	}
	want := "beta:0.3.0-beta.3 beta:0.3.0-beta.4 beta:0.3.0-beta.9 beta:0.3.0-beta.10 beta:0.3.0-beta.11 stable:0.2.0"
	if strings.Join(vs, " ") != want {
		t.Fatalf("got %v", vs)
	}
	a, _ := Marshal(Index{Schema: 1, GeneratedAt: t0, Releases: rs})
	c, _ := Marshal(Index{Schema: 1, GeneratedAt: t0, Releases: rs})
	if string(a) != string(c) || !LooksLikeUpdatesIndex(a) || LooksLikeUpdatesIndex([]byte(`{"packages":[]}`)) {
		t.Fatal("marshal must be deterministic")
	}
}

// ---- Stage and apply ----

type fixture struct {
	dir, feed string
	src       Source
	keys      []ed25519.PublicKey
	debName   string
	debData   []byte
}

// newFixture writes a signed file:// feed with one hub release for platform linux-amd64.
func newFixture(t *testing.T, version string) *fixture {
	t.Helper()
	return newFixtureFor(t, version, "linux-amd64", KindDeb, "_amd64.deb")
}

// newFixtureFor is newFixture for another platform, artifact kind and file name suffix.
func newFixtureFor(t *testing.T, version, platform, kind, suffix string) *fixture {
	t.Helper()
	f := &fixture{dir: t.TempDir(), feed: t.TempDir(), keys: testKeys(t)}
	f.debName = "uzume-server_" + strings.ReplaceAll(version, "-", "~") + suffix
	f.debData = []byte("dummy deb payload for " + version)
	if err := os.WriteFile(filepath.Join(f.feed, f.debName), f.debData, 0o644); err != nil {
		t.Fatal(err)
	}
	sum := sha256.Sum256(f.debData)
	a := Artifact{Platform: platform, Kind: kind, Name: f.debName, Size: int64(len(f.debData)),
		SHA256: hex.EncodeToString(sum[:]), URL: FileURL(filepath.Join(f.feed, f.debName))}
	f.writeIndex(t, Release{Product: "hub", Channel: "beta", Version: version, PublishedAt: t0, ProtocolVersion: 1, MinProtocolVersion: 1,
		Artifacts: []Artifact{a}})
	return f
}

func (f *fixture) writeIndex(t *testing.T, rs ...Release) {
	t.Helper()
	data, err := Marshal(Index{Schema: 1, GeneratedAt: t0, Releases: rs})
	if err != nil {
		t.Fatal(err)
	}
	sig, _ := corepkg.Sign(data, testSeed)
	p := filepath.Join(f.feed, "updates-index.json")
	os.WriteFile(p, data, 0o644)
	os.WriteFile(p+".sig", sig, 0o644)
	f.src = Source{IndexURL: FileURL(p)}
}

func (f *fixture) stage(t *testing.T, current string) Staged {
	t.Helper()
	ctx := context.Background()
	fe, err := f.src.LoadIndex(ctx, f.keys)
	if err != nil {
		t.Fatal(err)
	}
	sel, err := Select(fe.Index, Query{Product: ProductHub, Channel: ChannelBeta, Platform: "linux-amd64", Kind: KindDeb, Current: current})
	if err != nil || sel.Release == nil {
		t.Fatalf("select: %+v %v", sel, err)
	}
	st, err := Stage(ctx, fe, *sel.Release, *sel.Artifact, f.dir)
	if err != nil {
		t.Fatal(err)
	}
	return st
}

func TestStageWritesVerifiedFiles(t *testing.T) {
	f := newFixture(t, "0.3.0-beta.5")
	st := f.stage(t, "0.3.0-beta.4")
	d := StagedDir(f.dir)
	for _, n := range []string{st.Artifact, IndexFileName, SigFileName, StagedFileName} {
		if _, err := os.Stat(filepath.Join(d, n)); err != nil {
			t.Fatal(err)
		}
	}
	got, _ := ReadStaged(f.dir)
	if got == nil || got.Version != "0.3.0-beta.5" || got.Artifact != f.debName {
		t.Fatalf("%+v", got)
	}
	// The staged index is the exact byte sequence that was verified.
	want, _ := os.ReadFile(filepath.Join(f.feed, "updates-index.json"))
	have, _ := os.ReadFile(filepath.Join(d, IndexFileName))
	if string(want) != string(have) {
		t.Fatal("index bytes differ")
	}
}

func TestStageRejectsBadDownload(t *testing.T) {
	f := newFixture(t, "0.3.0-beta.5")
	ctx := context.Background()
	fe, err := f.src.LoadIndex(ctx, f.keys)
	if err != nil {
		t.Fatal(err)
	}
	r := fe.Index.Releases[0]
	for name, mod := range map[string]func(*Artifact){
		"bad sha":    func(a *Artifact) { a.SHA256 = goodSHA },
		"size short": func(a *Artifact) { a.Size-- },
		"size long":  func(a *Artifact) { a.Size++ },
	} {
		a := r.Artifacts[0]
		mod(&a)
		if _, err := Stage(ctx, fe, r, a, f.dir); err == nil {
			t.Fatalf("%s: must be rejected", name)
		}
		if st, _ := ReadStaged(f.dir); st != nil {
			t.Fatalf("%s: nothing may stay staged", name)
		}
	}
	// file:// artifacts are refused when the index did not come from file://.
	fe.Source.IndexURL = "https://example.org/updates-index.json"
	if _, err := Stage(ctx, fe, r, r.Artifacts[0], f.dir); err == nil || !strings.Contains(err.Error(), "https") {
		t.Fatalf("file artifact with https index: %v", err)
	}
}

func TestLoadIndexRejectsBadSignatureAndURL(t *testing.T) {
	f := newFixture(t, "0.3.0")
	other := make([]byte, 32)
	other[0] = 1
	pub, _ := corepkg.PublicFromSeed(other)
	if _, err := f.src.LoadIndex(context.Background(), []ed25519.PublicKey{pub}); err == nil || !strings.Contains(err.Error(), "not trusted") {
		t.Fatalf("untrusted key: %v", err)
	}
	if _, err := f.src.LoadIndex(context.Background(), nil); !errors.Is(err, corepkg.ErrNoTrustedKey) {
		t.Fatal(err)
	}
	if ValidateIndexURL("http://x/y") == nil || ValidateIndexURL("ftp://x") == nil || ValidateIndexURL("https://x/y") != nil || ValidateIndexURL("file:///tmp/x") != nil {
		t.Fatal("ValidateIndexURL")
	}
}

type fakeRunner struct {
	calls   [][]string
	copyOK  bool
	content []byte
	err     error
}

func (r *fakeRunner) run(_ context.Context, name string, args ...string) ([]byte, error) {
	r.calls = append(r.calls, append([]string{name}, args...))
	for i, a := range args { // the package path follows "-i" (dpkg) or "/i" (msiexec)
		if (a == "-i" || a == "/i") && i+1 < len(args) {
			b, err := os.ReadFile(args[i+1])
			r.copyOK, r.content = err == nil, b
		}
	}
	return []byte("ok"), r.err
}

func (f *fixture) apply(t *testing.T, run Runner, current string, mod func(*ApplyOptions)) (Result, error) {
	t.Helper()
	req := filepath.Join(t.TempDir(), "update-request")
	os.WriteFile(req, nil, 0o600)
	o := ApplyOptions{DataDir: f.dir, RequestFile: req, Keys: f.keys, CurrentVersion: current, Platform: "linux-amd64", Run: run,
		Now: func() time.Time { return t0 }, TempDir: t.TempDir()}
	if mod != nil {
		mod(&o)
	}
	res, err := ApplyStaged(context.Background(), o)
	if _, serr := os.Stat(req); !errors.Is(serr, os.ErrNotExist) && o.RequestFile == req {
		t.Fatal("request file must be deleted first")
	}
	return res, err
}

func TestApplyStagedGoodPath(t *testing.T) {
	f := newFixture(t, "0.3.0-beta.5")
	f.stage(t, "0.3.0-beta.4")
	fr := &fakeRunner{}
	res, err := f.apply(t, fr.run, "0.3.0-beta.4", nil)
	if err != nil || !res.OK {
		t.Fatal(res, err)
	}
	if len(fr.calls) != 1 || fr.calls[0][0] != "dpkg" || fr.calls[0][1] != "-i" || !fr.copyOK || string(fr.content) != string(f.debData) {
		t.Fatalf("calls=%v", fr.calls)
	}
	if strings.HasPrefix(fr.calls[0][2], f.dir) {
		t.Fatal("dpkg must get the root-owned copy, not the data dir file")
	}
	if _, serr := os.Stat(fr.calls[0][2]); !errors.Is(serr, os.ErrNotExist) {
		t.Fatal("temp copy must be removed afterwards")
	}
	last, err := ReadResult(f.dir)
	if err != nil || last == nil || !last.OK || last.Version != "0.3.0-beta.5" {
		t.Fatalf("%+v %v", last, err)
	}
}

func TestApplyStagedRejects(t *testing.T) {
	t.Run("replay of an older or equal version", func(t *testing.T) {
		f := newFixture(t, "0.3.0-beta.5")
		f.stage(t, "0.3.0-beta.4")
		for _, cur := range []string{"0.3.0-beta.5", "0.3.0-beta.6", "0.3.0", "1.0.0"} {
			fr := &fakeRunner{}
			res, err := f.apply(t, fr.run, cur, nil)
			if err == nil || res.OK || len(fr.calls) != 0 || !strings.Contains(err.Error(), "not newer") {
				t.Fatalf("cur %s: %v %v", cur, res, err)
			}
			if last, _ := ReadResult(f.dir); last == nil || last.OK {
				t.Fatal("failure must be recorded")
			}
		}
	})
	t.Run("non-release running version", func(t *testing.T) {
		f := newFixture(t, "0.3.0")
		f.stage(t, "0.2.0")
		fr := &fakeRunner{}
		if _, err := f.apply(t, fr.run, "dev", nil); err == nil || len(fr.calls) != 0 {
			t.Fatal(err)
		}
	})
	t.Run("wrong platform", func(t *testing.T) {
		f := newFixture(t, "0.3.0")
		f.stage(t, "0.2.0")
		fr := &fakeRunner{}
		_, err := f.apply(t, fr.run, "0.2.0", func(o *ApplyOptions) { o.Platform = "linux-arm64" })
		if err == nil || len(fr.calls) != 0 {
			t.Fatal(err)
		}
	})
	t.Run("untrusted signature", func(t *testing.T) {
		f := newFixture(t, "0.3.0")
		f.stage(t, "0.2.0")
		other := make([]byte, 32)
		other[1] = 9
		pub, _ := corepkg.PublicFromSeed(other)
		fr := &fakeRunner{}
		if _, err := f.apply(t, fr.run, "0.2.0", func(o *ApplyOptions) { o.Keys = []ed25519.PublicKey{pub} }); err == nil || len(fr.calls) != 0 {
			t.Fatal(err)
		}
	})
	t.Run("tampered package in the data dir", func(t *testing.T) {
		f := newFixture(t, "0.3.0")
		st := f.stage(t, "0.2.0")
		os.WriteFile(filepath.Join(StagedDir(f.dir), st.Artifact), []byte("evil evil evil evil evil evil evil"), 0o640)
		fr := &fakeRunner{}
		if _, err := f.apply(t, fr.run, "0.2.0", nil); err == nil || len(fr.calls) != 0 {
			t.Fatal(err)
		}
		// Same size, different bytes.
		os.WriteFile(filepath.Join(StagedDir(f.dir), st.Artifact), []byte(strings.Repeat("x", len(f.debData))), 0o640)
		if _, err := f.apply(t, fr.run, "0.2.0", nil); err == nil || len(fr.calls) != 0 {
			t.Fatal(err)
		}
	})
	t.Run("staged.json names another version", func(t *testing.T) {
		f := newFixture(t, "0.3.0")
		f.stage(t, "0.2.0")
		os.WriteFile(filepath.Join(StagedDir(f.dir), StagedFileName), []byte(`{"version":"9.9.9","artifact":"`+f.debName+`"}`), 0o640)
		fr := &fakeRunner{}
		if _, err := f.apply(t, fr.run, "0.2.0", nil); err == nil || len(fr.calls) != 0 {
			t.Fatal(err)
		}
	})
	t.Run("symlinks are not followed", func(t *testing.T) {
		f := newFixture(t, "0.3.0")
		st := f.stage(t, "0.2.0")
		d := StagedDir(f.dir)
		outside := filepath.Join(t.TempDir(), "evil.deb")
		os.WriteFile(outside, f.debData, 0o600) // even with valid content
		os.Remove(filepath.Join(d, st.Artifact))
		if err := os.Symlink(outside, filepath.Join(d, st.Artifact)); err != nil {
			t.Skip(err)
		}
		fr := &fakeRunner{}
		if _, err := f.apply(t, fr.run, "0.2.0", nil); err == nil || len(fr.calls) != 0 {
			t.Fatalf("symlinked package accepted: %v", err)
		}
		// A symlinked staged directory is refused too.
		f2 := newFixture(t, "0.3.0")
		f2.stage(t, "0.2.0")
		real := filepath.Join(f2.dir, "updates", "real")
		os.Rename(StagedDir(f2.dir), real)
		os.Symlink(real, StagedDir(f2.dir))
		if _, err := f2.apply(t, fr.run, "0.2.0", nil); err == nil || len(fr.calls) != 0 {
			t.Fatalf("symlinked staged dir accepted: %v", err)
		}
		// A symlink planted as last-result.json is replaced, not followed.
		f3 := newFixture(t, "0.3.0")
		f3.stage(t, "0.2.0")
		victim := filepath.Join(t.TempDir(), "victim")
		os.WriteFile(victim, []byte("keep"), 0o600)
		os.Symlink(victim, filepath.Join(f3.dir, "updates", LastResultName))
		if _, err := f3.apply(t, (&fakeRunner{}).run, "0.2.0", nil); err != nil {
			t.Fatal(err)
		}
		if b, _ := os.ReadFile(victim); string(b) != "keep" {
			t.Fatal("symlink target was overwritten")
		}
	})
	t.Run("dpkg failure is recorded", func(t *testing.T) {
		f := newFixture(t, "0.3.0")
		f.stage(t, "0.2.0")
		fr := &fakeRunner{err: errors.New("exit status 1")}
		res, err := f.apply(t, fr.run, "0.2.0", nil)
		if err == nil || res.OK {
			t.Fatal(res, err)
		}
		if last, _ := ReadResult(f.dir); last == nil || last.OK || !strings.Contains(last.Message, "dpkg") {
			t.Fatalf("%+v", last)
		}
	})
	t.Run("nothing staged", func(t *testing.T) {
		f := &fixture{dir: t.TempDir(), keys: testKeys(t)}
		if _, err := f.apply(t, (&fakeRunner{}).run, "0.2.0", nil); err == nil {
			t.Fatal("must fail")
		}
	})
}

func TestRequestFile(t *testing.T) {
	d := t.TempDir()
	if !RequestDirWritable(d) || RequestDirWritable(filepath.Join(d, "missing")) || RequestDirWritable("") {
		t.Fatal("RequestDirWritable")
	}
	if err := WriteRequest(d, "0.3.0"); err != nil || !RequestPending(d) {
		t.Fatal(err)
	}
	if entries, _ := os.ReadDir(d); len(entries) != 1 {
		t.Fatalf("temp files left: %v", entries)
	}
	if err := WriteRequest(filepath.Join(d, "missing"), "0.3.0"); err == nil {
		t.Fatal("missing dir")
	}
}

func TestLegacyTestChannel(t *testing.T) {
	if NormalizeChannel("test") != "beta" || NormalizeChannel("beta") != "beta" || NormalizeChannel("stable") != "stable" || NormalizeChannel("dev") != "dev" {
		t.Fatal("NormalizeChannel")
	}
	if ValidSelectableChannel("test") {
		t.Fatal("test is no selectable channel")
	}
	idx, errs := ParseIndex(indexJSON(t, 1, rel("hub", "test", "0.3.0-test.1"), rel("hub", "beta", "0.3.0-beta.1")))
	if len(errs) != 1 || len(idx.Releases) != 1 || idx.Releases[0].Channel != "beta" {
		t.Fatalf("test release skipped: %v %v", errs, idx.Releases)
	}
}

func TestParseIndexDropsUnknownArtifacts(t *testing.T) {
	mixed := rel("hub", "stable", "0.9.0",
		art("linux-amd64", KindDeb, "a.deb"),
		art("freebsd-amd64", KindBinary, "b.bin"), // unknown platform
		art("linux-arm64", KindMSI, "c.msi"),      // kind not allowed on platform
		art("windows-amd64", KindMSI, "d.msi"))
	onlyUnknown := rel("hub", "stable", "0.10.0", art("freebsd-amd64", KindBinary, "e.bin"))
	idx, errs := ParseIndex(indexJSON(t, 1, mixed, onlyUnknown))
	if len(idx.Releases) != 1 || len(idx.Releases[0].Artifacts) != 2 || idx.Releases[0].Version != "0.9.0" {
		t.Fatalf("%+v", idx.Releases)
	}
	if len(errs) != 1 || Fatal(errs) {
		t.Fatalf("errs %v", errs)
	}
	var re *ReleaseError
	if !errors.As(errs[0], &re) || re.Version != "0.10.0" {
		t.Fatalf("%v", errs[0])
	}
	if err := ValidateRelease(mixed); err == nil {
		t.Fatal("ValidateRelease must stay strict")
	}
}

func TestLegacyCompanionHelpers(t *testing.T) {
	for v, want := range map[string]bool{"0.9.0-legacy": true, "0.9.0": false, "0.9.0-beta.1-legacy": false, "0.9.0+legacy": false, "x-legacy": false} {
		if IsLegacyCompanion(v) != want {
			t.Errorf("%s: want %v", v, want)
		}
	}
}

func TestReleaseNotesValidation(t *testing.T) {
	ok := []string{"", "### Added\n- one\n- zwei ü€\n\n\t- tab", strings.Repeat("a", MaxNotesBytes)}
	for _, n := range ok {
		r := rel("hub", "stable", "0.3.0")
		r.Notes = n
		if err := ValidateRelease(r); err != nil {
			t.Errorf("notes %q must be accepted: %v", n, err)
		}
	}
	bad := map[string]string{
		"too long":        strings.Repeat("a", MaxNotesBytes+1),
		"too long utf8":   strings.Repeat("€", MaxNotesBytes/3+1),
		"invalid utf8":    "ab\xffcd",
		"NUL":             "a\x00b",
		"escape":          "a\x1b[31mb",
		"carriage return": "a\r\nb",
		"DEL":             "a\x7fb",
		"C1 control":      "a\u0085b",
	}
	for name, n := range bad {
		r := rel("hub", "stable", "0.3.0")
		r.Notes = n
		if err := ValidateRelease(r); err == nil {
			t.Errorf("%s: notes must be rejected", name)
		}
	}
	// Round trip through the index keeps notes.
	r := rel("hub", "stable", "0.3.0")
	r.Notes = "### Added\n- x"
	idx, errs := ParseIndex(indexJSON(t, 1, r))
	if len(errs) != 0 || len(idx.Releases) != 1 || idx.Releases[0].Notes != r.Notes {
		t.Fatalf("%+v %v", idx.Releases, errs)
	}
}

func TestAppImageArtifactKind(t *testing.T) {
	ok := rel("player", "stable", "0.24.0", art("linux-x64", KindAppImage, "Uzume-0.24.0-x86_64.AppImage"))
	if err := ValidateRelease(ok); err != nil {
		t.Fatalf("player linux-x64 appimage must be accepted: %v", err)
	}
	for name, r := range map[string]Release{
		"hub linux-x64":        rel("hub", "stable", "0.24.0", art("linux-x64", KindAppImage, "a.AppImage")),
		"hub linux-amd64":      rel("hub", "stable", "0.24.0", art("linux-amd64", KindAppImage, "a.AppImage")),
		"player windows-x64":   rel("player", "stable", "0.24.0", art("windows-x64", KindAppImage, "a.AppImage")),
		"player linux-x64 deb": rel("player", "stable", "0.24.0", art("linux-x64", KindDeb, "a.deb")),
		"player linux-x64 msi": rel("player", "stable", "0.24.0", art("linux-x64", KindMSI, "a.msi")),
	} {
		if err := ValidateRelease(r); err == nil {
			t.Errorf("%s must be rejected", name)
		}
	}
}
