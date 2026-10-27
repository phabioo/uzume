package pluginpkg

import (
	"crypto/ed25519"
	"encoding/base64"
	"encoding/json"
	"os"
	"sort"
	"testing"
)

// The signature test vector (testdata/signature-vector.json and testdata/vector.uzplugin) lets the app
// implementation reproduce the exact bytes of the signed message and the signature. Seed: bytes 0x00..0x1f.
// Regenerate with UZUME_UPDATE_VECTOR=1 go test ./internal/pluginpkg -run TestSignatureVector.

type vectorFile struct {
	Path   string `json:"path"`
	Body   string `json:"body"`
	SHA256 string `json:"sha256"`
}

type vector struct {
	Description   string       `json:"description"`
	SeedBase64    string       `json:"seed_base64"`
	PublicKey     string       `json:"public_key_base64"`
	KeyID         string       `json:"key_id"`
	Files         []vectorFile `json:"files"`
	SignedMessage string       `json:"signed_message"`
	Signature     string       `json:"signature_base64"`
	Plugin        string       `json:"plugin_file"`
}

func vectorSeed() []byte {
	s := make([]byte, ed25519.SeedSize)
	for i := range s {
		s[i] = byte(i)
	}
	return s
}

func buildVector(t *testing.T) (vector, []byte) {
	t.Helper()
	seed := vectorSeed()
	files := []zf{
		{name: "manifest.json", body: `{"api":1,"id":"net.example.vector","name":"Vector","version":"1.0.0","entry":"main.lua","permissions":["ui"]}` + "\n"},
		{name: "main.lua", body: "-- test vector plugin\nreturn {}\n"},
		{name: "i18n/en.json", body: "{\"hello\":\"Hello\"}\n"},
	}
	signed := mustSign(t, mkzip(t, files...), seed)
	p, err := Open(signed)
	if err != nil {
		t.Fatal(err)
	}
	keys := keysOf(seed)
	if !p.Verify(keys).Official() {
		t.Fatal("vector plugin is not official")
	}
	v := vector{
		Description: "Ed25519 over the signed message; the seed is public test data, never a real key. Message = 'uzume-plugin-v1\\n' + for each file sorted by path (byte order) '<sha256>  <path>\\n'.",
		SeedBase64:  base64.StdEncoding.EncodeToString(seed),
		PublicKey:   base64.StdEncoding.EncodeToString(keys[0]),
		Plugin:      "vector.uzplugin",
	}
	for _, f := range files {
		v.Files = append(v.Files, vectorFile{Path: f.name, Body: f.body, SHA256: p.Hashes[f.name]})
	}
	sort.Slice(v.Files, func(i, j int) bool { return v.Files[i].Path < v.Files[j].Path })
	hashes := map[string]string{}
	for _, f := range v.Files {
		hashes[f.Path] = f.SHA256
	}
	v.SignedMessage = string(SignedMessage(hashes))
	var sf SignatureFile
	rc, _ := p.entries[SignatureName].Open()
	if err := json.NewDecoder(rc).Decode(&sf); err != nil {
		t.Fatal(err)
	}
	rc.Close()
	v.Signature = sf.Signature
	v.KeyID = p.Verify(keys).KeyID
	return v, signed
}

func TestSignatureVector(t *testing.T) {
	v, plugin := buildVector(t)
	want, err := json.MarshalIndent(v, "", "  ")
	if err != nil {
		t.Fatal(err)
	}
	want = append(want, '\n')
	if os.Getenv("UZUME_UPDATE_VECTOR") == "1" {
		os.WriteFile("testdata/signature-vector.json", want, 0o644)
		os.WriteFile("testdata/vector.uzplugin", plugin, 0o644)
	}
	gotJSON, err := os.ReadFile("testdata/signature-vector.json")
	if err != nil {
		t.Fatal(err)
	}
	if string(gotJSON) != string(want) {
		t.Fatalf("testdata/signature-vector.json is stale; regenerate with UZUME_UPDATE_VECTOR=1")
	}
	// The committed file verifies independently of how this test builds it.
	data, err := os.ReadFile("testdata/vector.uzplugin")
	if err != nil {
		t.Fatal(err)
	}
	p, err := Open(data)
	if err != nil {
		t.Fatal(err)
	}
	pub, _ := base64.StdEncoding.DecodeString(v.PublicKey)
	if verdict := p.Verify([]ed25519.PublicKey{pub}); !verdict.Official() {
		t.Fatalf("committed vector.uzplugin: %+v", verdict)
	}
	// And the documented message is what is signed.
	sig, _ := base64.StdEncoding.DecodeString(v.Signature)
	if !ed25519.Verify(pub, []byte(v.SignedMessage), sig) {
		t.Fatal("signature does not verify over signed_message")
	}
}
