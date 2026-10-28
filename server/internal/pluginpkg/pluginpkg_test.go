package pluginpkg

import (
	"archive/zip"
	"bytes"
	"crypto/ed25519"
	"encoding/base64"
	"encoding/json"
	"errors"
	"os"
	"strings"
	"testing"
	"time"
)

type zf struct {
	name string
	body string
	mode os.FileMode // 0 = regular
}

const goodManifest = `{"api":1,"id":"net.example.tracker","name":"Tracker","version":"1.2.0","entry":"main.lua","description":"Tracks things","permissions":["memory.read","ui"]}`

func mkzip(t testing.TB, files ...zf) []byte {
	t.Helper()
	var b bytes.Buffer
	zw := zip.NewWriter(&b)
	for _, f := range files {
		h := &zip.FileHeader{Name: f.name, Method: zip.Deflate, Modified: time.Date(2026, 10, 12, 0, 0, 0, 0, time.UTC)}
		if f.mode != 0 {
			h.SetMode(f.mode)
		}
		w, err := zw.CreateHeader(h)
		if err != nil {
			t.Fatal(err)
		}
		w.Write([]byte(f.body))
	}
	if err := zw.Close(); err != nil {
		t.Fatal(err)
	}
	return b.Bytes()
}

func good(t testing.TB, extra ...zf) []byte {
	return mkzip(t, append([]zf{{name: "manifest.json", body: goodManifest}, {name: "main.lua", body: "return {}"}}, extra...)...)
}

func wantInvalid(t *testing.T, data []byte, substr string) {
	t.Helper()
	_, err := Open(data)
	if err == nil {
		t.Fatalf("accepted, want error containing %q", substr)
	}
	if !errors.Is(err, ErrInvalid) || !strings.Contains(err.Error(), substr) {
		t.Fatalf("error %q, want ErrInvalid containing %q", err, substr)
	}
}

func TestOpenValid(t *testing.T) {
	p, err := Open(good(t, zf{name: "lua/", mode: os.ModeDir | 0o755}, zf{name: "lua/util.lua", body: "x"}, zf{name: "i18n/en.json", body: "{}"}))
	if err != nil {
		t.Fatal(err)
	}
	if p.Manifest.ID != "net.example.tracker" || p.Manifest.Version != "1.2.0" || p.Manifest.Entry != "main.lua" ||
		len(p.Manifest.Permissions) != 2 || len(p.Files) != 4 || p.HasSignature() || p.Hashes["main.lua"] == "" {
		t.Fatalf("package %+v", p)
	}
	if v := p.Verify(nil); v.State != SigNone || v.Official() {
		t.Fatalf("verdict %+v", v)
	}
}

func TestOpenRejectsBadNames(t *testing.T) {
	for _, tc := range []struct{ name, why string }{
		{"../evil.lua", `".."`},
		{"a/../../evil.lua", `".."`},
		{"/abs.lua", "absolute"},
		{`a\b.lua`, "backslash"},
		{`C:/x.lua`, "drive"},
		{"C:x.lua", "drive"},
		{"a//b.lua", "empty path segment"},
		{"./a.lua", `"."`},
		{"a/./b.lua", `"."`},
		{"a\nb.lua", "control"},
		{strings.Repeat("a", 256), "255"},
		{"ok/../", `".."`}, // also as directory entry
		{"nul", "reserved Windows"},
		{"assets/CON.png", "reserved Windows"},
		{"aux.lua", "reserved Windows"},
		{"com1", "reserved Windows"},
		{"Lpt9.txt", "reserved Windows"},
		{"dir./x.lua", "ends in a dot"},
		{"trailing.", "ends in a dot"},
		{"trailing ", "ends in a dot or space"},
	} {
		t.Run(tc.why, func(t *testing.T) {
			wantInvalid(t, mkzip(t, zf{name: "manifest.json", body: goodManifest}, zf{name: "main.lua", body: "x"}, zf{name: tc.name, body: "x"}), tc.why)
		})
	}
}

func TestOpenRejectsDuplicatesCaseInsensitive(t *testing.T) {
	wantInvalid(t, mkzip(t, zf{name: "manifest.json", body: goodManifest}, zf{name: "main.lua", body: "x"}, zf{name: "a/B.lua", body: "1"}, zf{name: "a/b.lua", body: "2"}), "duplicate")
	wantInvalid(t, mkzip(t, zf{name: "manifest.json", body: goodManifest}, zf{name: "main.lua", body: "x"}, zf{name: "main.lua", body: "y"}), "duplicate")
	wantInvalid(t, mkzip(t, zf{name: "manifest.json", body: goodManifest}, zf{name: "main.lua", body: "x"}, zf{name: "A", body: "1"}, zf{name: "a/b", body: "2"}), "folder")
}

func TestOpenRejectsSymlink(t *testing.T) {
	wantInvalid(t, mkzip(t, zf{name: "manifest.json", body: goodManifest}, zf{name: "main.lua", body: "x"}, zf{name: "link.lua", body: "main.lua", mode: os.ModeSymlink | 0o777}), "symbolic link")
	wantInvalid(t, mkzip(t, zf{name: "manifest.json", body: goodManifest}, zf{name: "main.lua", body: "x"}, zf{name: "dirlink/", body: "", mode: os.ModeSymlink | 0o777}), "symbolic link")
}

func TestOpenLimits(t *testing.T) {
	t.Run("entries", func(t *testing.T) {
		fs := []zf{{name: "manifest.json", body: goodManifest}, {name: "main.lua", body: "x"}}
		for i := 0; len(fs) <= MaxEntries; i++ {
			fs = append(fs, zf{name: "f/" + string(rune('a'+i%26)) + strings.Repeat("x", i/26) + ".txt", body: ""})
		}
		wantInvalid(t, mkzip(t, fs...), "entries")
	})
	t.Run("entries at the limit are fine", func(t *testing.T) {
		fs := []zf{{name: "manifest.json", body: goodManifest}, {name: "main.lua", body: "x"}}
		for i := 0; len(fs) < MaxEntries; i++ {
			fs = append(fs, zf{name: "f/" + strings.Repeat("x", i/26) + string(rune('a'+i%26)) + ".txt", body: ""})
		}
		if _, err := Open(mkzip(t, fs...)); err != nil {
			t.Fatal(err)
		}
	})
	t.Run("uncompressed", func(t *testing.T) {
		big := strings.Repeat("\x00", 33<<20) // compresses to a few KiB
		wantInvalid(t, mkzip(t, zf{name: "manifest.json", body: goodManifest}, zf{name: "main.lua", body: "x"}, zf{name: "a.bin", body: big}, zf{name: "b.bin", body: big}), "uncompressed")
	})
	t.Run("compressed", func(t *testing.T) {
		wantInvalid(t, append(good(t), make([]byte, MaxCompressedBytes)...), "larger")
	})
	t.Run("not a zip", func(t *testing.T) { wantInvalid(t, []byte("hello"), "not a zip") })
	t.Run("manifest size", func(t *testing.T) {
		m := `{"api":1,"id":"net.example.tracker","name":"T","version":"1.0.0","entry":"main.lua","x":"` + strings.Repeat("a", MaxManifestBytes) + `"}`
		wantInvalid(t, mkzip(t, zf{name: "manifest.json", body: m}, zf{name: "main.lua", body: "x"}), "256 KiB")
	})
}

func TestOpenManifestRules(t *testing.T) {
	m := func(s string) []byte {
		return mkzip(t, zf{name: "manifest.json", body: s}, zf{name: "main.lua", body: "x"}, zf{name: "dir/other.lua", body: "x"})
	}
	t.Run("missing manifest", func(t *testing.T) {
		wantInvalid(t, mkzip(t, zf{name: "main.lua", body: "x"}), "manifest.json is missing")
	})
	t.Run("not json", func(t *testing.T) { wantInvalid(t, m(`nope`), "JSON object") })
	t.Run("api missing", func(t *testing.T) {
		wantInvalid(t, m(`{"id":"net.example.t","name":"T","version":"1.0.0","entry":"main.lua"}`), "'api' is required")
	})
	t.Run("api 2", func(t *testing.T) {
		wantInvalid(t, m(`{"api":2,"id":"net.example.t","name":"T","version":"1.0.0","entry":"main.lua"}`), "API 2")
	})
	t.Run("api fraction", func(t *testing.T) {
		wantInvalid(t, m(`{"api":1.5,"id":"net.example.t","name":"T","version":"1.0.0","entry":"main.lua"}`), "integer")
	})
	for _, id := range []string{"tracker", "Net.Example.T", "net..t", "net.example.t_x", ".net.t", strings.Repeat("a", 126) + ".bc"} {
		t.Run("id "+id[:min(len(id), 12)], func(t *testing.T) {
			wantInvalid(t, m(`{"api":1,"id":"`+id+`","name":"T","version":"1.0.0","entry":"main.lua"}`), "id")
		})
	}
	for _, v := range []string{"1", "1.0", "v1.0.0", "01.0.0", "1.0.0-", "latest"} {
		t.Run("version "+v, func(t *testing.T) {
			wantInvalid(t, m(`{"api":1,"id":"net.example.t","name":"T","version":"`+v+`","entry":"main.lua"}`), "version")
		})
	}
	for _, e := range []string{"missing.lua", "dir/..", "../main.lua", "/main.lua", "main.txt", "dir"} {
		t.Run("entry "+e, func(t *testing.T) {
			wantInvalid(t, m(`{"api":1,"id":"net.example.t","name":"T","version":"1.0.0","entry":"`+e+`"}`), "entry")
		})
	}
	t.Run("name empty", func(t *testing.T) {
		wantInvalid(t, m(`{"api":1,"id":"net.example.t","name":" ","version":"1.0.0","entry":"main.lua"}`), "'name'")
	})
	t.Run("tool", func(t *testing.T) {
		wantInvalid(t, m(`{"api":1,"id":"net.example.t","name":"T","version":"1.0.0","entry":"main.lua","type":"tool"}`), "tool")
	})
	t.Run("bad permissions", func(t *testing.T) {
		wantInvalid(t, m(`{"api":1,"id":"net.example.t","name":"T","version":"1.0.0","entry":"main.lua","permissions":"ui"}`), "permissions")
	})
	t.Run("self require", func(t *testing.T) {
		wantInvalid(t, m(`{"api":1,"id":"net.example.t","name":"T","version":"1.0.0","entry":"main.lua","requires":{"net.example.t":"*"}}`), "itself")
	})
	t.Run("full", func(t *testing.T) {
		p, err := Open(m(`{"api":1,"id":"net.example.t","name":"T","version":"1.0.0-rc.1+b5","entry":"dir/other.lua","provides":["pokemon.core"],
			"requires":{"net.example.core":"^1.0"},"games":[{"system":"nds","codes":["IPK*"]},{"system":"gba"}],"permissions":["ui","ui"],"assets":["a.png"]}`))
		if err != nil {
			t.Fatal(err)
		}
		mf := p.Manifest
		if len(mf.Permissions) != 1 || mf.Requires["net.example.core"] != "^1.0" || len(mf.Games) != 2 || mf.Games[0].Codes[0] != "IPK*" || mf.Provides[0] != "pokemon.core" {
			t.Fatalf("%+v", mf)
		}
	})
}

// ---- Signature ----

func testSeed(b byte) []byte {
	s := make([]byte, ed25519.SeedSize)
	for i := range s {
		s[i] = b + byte(i)
	}
	return s
}

func keysOf(seeds ...[]byte) []ed25519.PublicKey {
	var ks []ed25519.PublicKey
	for _, s := range seeds {
		ks = append(ks, ed25519.NewKeyFromSeed(s).Public().(ed25519.PublicKey))
	}
	return ks
}

func mustSign(t *testing.T, data []byte, seed []byte) []byte {
	t.Helper()
	out, err := Sign(data, seed)
	if err != nil {
		t.Fatal(err)
	}
	return out
}

// rewrite rebuilds a zip, calling edit for each file; edit may change the body, return false to drop the file.
func rewrite(t *testing.T, data []byte, edit func(name string, body []byte) ([]byte, bool), add ...zf) []byte {
	t.Helper()
	zr, err := zip.NewReader(bytes.NewReader(data), int64(len(data)))
	if err != nil {
		t.Fatal(err)
	}
	var fs []zf
	for _, f := range zr.File {
		rc, _ := f.Open()
		var b bytes.Buffer
		b.ReadFrom(rc)
		rc.Close()
		body, keep := edit(f.Name, b.Bytes())
		if keep {
			fs = append(fs, zf{name: f.Name, body: string(body)})
		}
	}
	return mkzip(t, append(fs, add...)...)
}

func keep(_ string, b []byte) ([]byte, bool) { return b, true }

func TestSignAndVerify(t *testing.T) {
	seed := testSeed(1)
	keys := keysOf(seed)
	signed := mustSign(t, good(t, zf{name: "lua/", mode: os.ModeDir | 0o755}, zf{name: "lua/a.lua", body: "a"}), seed)
	p, err := Open(signed)
	if err != nil {
		t.Fatal(err)
	}
	v := p.Verify(keys)
	if !v.Official() || v.KeyID == "" || !p.HasSignature() {
		t.Fatalf("verdict %+v", v)
	}
	// Signing again replaces the signature (also with another key) and keeps everything else.
	seed2 := testSeed(9)
	again := mustSign(t, signed, seed2)
	p2, _ := Open(again)
	if v := p2.Verify(keysOf(seed2)); !v.Official() {
		t.Fatalf("re-signed: %+v", v)
	}
	if v := p2.Verify(keys); v.State != SigInvalid {
		t.Fatalf("old key accepted the new signature: %+v", v)
	}
	if len(p2.Files) != len(p.Files) {
		t.Fatalf("files %v vs %v", p2.Files, p.Files)
	}
	// Several trusted keys: any one matches.
	if v := p.Verify(keysOf(seed2, seed)); !v.Official() {
		t.Fatalf("second key: %+v", v)
	}
}

func TestVerifyRejects(t *testing.T) {
	seed := testSeed(1)
	keys := keysOf(seed)
	signed := mustSign(t, good(t, zf{name: "lua/a.lua", body: "a"}), seed)
	check := func(t *testing.T, data []byte, wantSub string) {
		t.Helper()
		p, err := Open(data)
		if err != nil {
			t.Fatal(err)
		}
		v := p.Verify(keys)
		if v.State != SigInvalid || v.Official() || !strings.Contains(v.Reason, wantSub) {
			t.Fatalf("verdict %+v, want invalid containing %q", v, wantSub)
		}
	}
	t.Run("modified file", func(t *testing.T) {
		check(t, rewrite(t, signed, func(n string, b []byte) ([]byte, bool) {
			if n == "lua/a.lua" {
				return []byte("evil"), true
			}
			return b, true
		}), "changed")
	})
	t.Run("modified manifest", func(t *testing.T) {
		check(t, rewrite(t, signed, func(n string, b []byte) ([]byte, bool) {
			if n == "manifest.json" {
				return bytes.Replace(b, []byte("1.2.0"), []byte("9.9.9"), 1), true
			}
			return b, true
		}), "changed")
	})
	t.Run("extra file", func(t *testing.T) {
		check(t, rewrite(t, signed, keep, zf{name: "extra.lua", body: "x"}), "not covered")
	})
	t.Run("missing file", func(t *testing.T) {
		check(t, rewrite(t, signed, func(n string, b []byte) ([]byte, bool) { return b, n != "lua/a.lua" }), "missing")
	})
	t.Run("wrong key", func(t *testing.T) {
		p, _ := Open(signed)
		if v := p.Verify(keysOf(testSeed(50))); v.State != SigInvalid || !strings.Contains(v.Reason, "trusted") {
			t.Fatalf("%+v", v)
		}
	})
	t.Run("no keys", func(t *testing.T) {
		p, _ := Open(signed)
		if v := p.Verify(nil); v.State != SigInvalid || v.Official() {
			t.Fatalf("%+v", v)
		}
	})
	t.Run("broken signature value", func(t *testing.T) {
		check(t, rewrite(t, signed, func(n string, b []byte) ([]byte, bool) {
			if n == SignatureName {
				var sf SignatureFile
				json.Unmarshal(b, &sf)
				sf.Signature = base64.StdEncoding.EncodeToString(make([]byte, 64))
				out, _ := json.Marshal(sf)
				return out, true
			}
			return b, true
		}), "trusted key")
	})
	t.Run("garbage SIGNATURE.json", func(t *testing.T) {
		check(t, rewrite(t, signed, func(n string, b []byte) ([]byte, bool) {
			if n == SignatureName {
				return []byte("{"), true
			}
			return b, true
		}), "not valid")
	})
	t.Run("listed but absent from zip", func(t *testing.T) {
		check(t, rewrite(t, signed, func(n string, b []byte) ([]byte, bool) {
			if n == SignatureName {
				var sf SignatureFile
				json.Unmarshal(b, &sf)
				sf.Files["ghost.lua"] = strings.Repeat("0", 64)
				out, _ := json.Marshal(sf)
				return out, true
			}
			return b, true
		}), "missing")
	})
}

func TestSignedMessageFormat(t *testing.T) {
	got := string(SignedMessage(map[string]string{"b.lua": "bb", "B.lua": "BB", "a/z": "az", "a.lua": "aa"}))
	// Byte order: "B.lua" < "a.lua" < "a/z" < "b.lua" ('.' 0x2e < '/' 0x2f).
	want := "uzume-plugin-v1\nBB  B.lua\naa  a.lua\naz  a/z\nbb  b.lua\n"
	if got != want {
		t.Fatalf("message %q, want %q", got, want)
	}
}

func TestSignRefusesInvalidInput(t *testing.T) {
	if _, err := Sign([]byte("x"), testSeed(1)); !errors.Is(err, ErrInvalid) {
		t.Fatalf("err %v", err)
	}
	if _, err := Sign(good(t), []byte("short")); err == nil {
		t.Fatal("short seed accepted")
	}
}

func TestManifestReplaces(t *testing.T) {
	m := func(extra string) []byte {
		return mkzip(t, zf{name: "manifest.json", body: `{"api":1,"id":"net.example.t","name":"T","version":"1.0.0","entry":"main.lua"` + extra + `}`}, zf{name: "main.lua", body: "x"})
	}
	p, err := Open(m(`,"replaces":["net.example.old","net.example.older"]`))
	if err != nil || len(p.Manifest.Replaces) != 2 || p.Manifest.Replaces[1] != "net.example.older" {
		t.Fatalf("%v %+v", err, p.Manifest.Replaces)
	}
	if p, err = Open(m(``)); err != nil || p.Manifest.Replaces != nil {
		t.Fatalf("%v %+v", err, p.Manifest.Replaces)
	}
	if p, err = Open(m(`,"replaces":[]`)); err != nil || p.Manifest.Replaces != nil {
		t.Fatalf("%v %+v", err, p.Manifest.Replaces)
	}
	many := `"net.example.a","net.example.b","net.example.c","net.example.d","net.example.e","net.example.f","net.example.g","net.example.h","net.example.i"`
	for name, c := range map[string]struct{ in, want string }{
		"not array": {`,"replaces":"net.example.old"`, "replaces"},
		"bad id":    {`,"replaces":["Old"]`, "invalid plugin id"},
		"self":      {`,"replaces":["net.example.t"]`, "itself"},
		"duplicate": {`,"replaces":["net.example.a","net.example.a"]`, "duplicate"},
		"too many":  {`,"replaces":[` + many + `]`, "at most"},
	} {
		t.Run(name, func(t *testing.T) { wantInvalid(t, m(c.in), c.want) })
	}
}
