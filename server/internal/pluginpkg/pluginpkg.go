// Package pluginpkg is the .uzplugin file format of Uzume plugins (0.11, docs/architecture/12-plugins.md
// section 16.10): validation of the zip, the manifest rules shared with the app (client/plugins/manifest.cpp),
// the optional SIGNATURE.json with its Ed25519 signature, and the signed plugin index.
package pluginpkg

import (
	"archive/zip"
	"bytes"
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"regexp"
	"slices"
	"sort"
	"strings"
	"unicode/utf8"

	"github.com/phabioo/uzume/server/internal/corepkg"
	"github.com/phabioo/uzume/server/internal/updates"
)

// Limits of a .uzplugin file (identical in the server and the app).
const (
	MaxCompressedBytes   = 32 << 20
	MaxUncompressedBytes = 64 << 20
	MaxEntries           = 2000
	MaxNameBytes         = 255
	MaxManifestBytes     = 256 << 10
	MaxSignatureBytes    = 2 << 20
)

// Reserved file names at the root of the zip.
const (
	ManifestName  = "manifest.json"
	SignatureName = "SIGNATURE.json"
)

// SignaturePrefix starts the signed message (see SignedMessage).
const SignaturePrefix = "uzume-plugin-v1\n"

// APIVersion is the only plugin API version this server knows.
const APIVersion = 1

// ErrInvalid wraps every validation failure of a file (errors.Is).
var ErrInvalid = errors.New("invalid plugin file")

func invalid(format string, a ...any) error {
	return fmt.Errorf("%w: %s", ErrInvalid, fmt.Sprintf(format, a...))
}

var idRe = regexp.MustCompile(`^[a-z0-9-]+(\.[a-z0-9-]+)+$`)

// ValidID reports whether id is a valid plugin id (reverse DNS, lowercase letters, digits, '-' and '.').
func ValidID(id string) bool { return len(id) <= 128 && idRe.MatchString(id) }

// Game is one entry of the manifest field "games".
type Game struct {
	System string   `json:"system"`
	Codes  []string `json:"codes"`
}

// Manifest is the part of manifest.json the server needs. The unmodified bytes are kept in Package.RawManifest.
type Manifest struct {
	API         int               `json:"api"`
	ID          string            `json:"id"`
	Name        string            `json:"name"`
	Version     string            `json:"version"`
	Description string            `json:"description"`
	Entry       string            `json:"entry"`
	Permissions []string          `json:"permissions"`
	Requires    map[string]string `json:"requires"`
	Provides    []string          `json:"provides"`
	Games       []Game            `json:"games"`
	// RunSettings is the optional manifest field run_settings (validated; entries normalized to the known fields).
	RunSettings []RunSetting `json:"run_settings,omitempty"`
	// RunUpdateFrom is the optional manifest field run_update_from (SemVer): the lowest pinned version from which this
	// version may take over a running run. Empty = any older version.
	RunUpdateFrom string `json:"run_update_from,omitempty"`
	// IssuesURL is the optional manifest field issues_url: the GitHub repository for error reports of the plugin
	// (ADR 0035 D4). Empty when absent or invalid.
	IssuesURL string `json:"issues_url,omitempty"`
	// Warnings are problems that do not stop the plugin from loading (an invalid issues_url).
	Warnings []string `json:"-"`
}

// Package is a validated .uzplugin file held in memory (at most 32 MiB).
type Package struct {
	Manifest    Manifest
	RawManifest []byte
	// Files are the names of all non-directory entries in zip order (SIGNATURE.json included).
	Files []string
	// Hashes maps every non-directory entry to the lowercase hex SHA-256 of its uncompressed content.
	Hashes map[string]string

	data    []byte
	zr      *zip.Reader
	rawSig  []byte
	hasSig  bool
	entries map[string]*zip.File
}

// SafeName checks one zip entry name (directory entries without their trailing slash).
func SafeName(name string) error {
	if name == "" {
		return errors.New("empty name")
	}
	if len(name) > MaxNameBytes {
		return fmt.Errorf("name is longer than %d bytes", MaxNameBytes)
	}
	if !utf8.ValidString(name) {
		return errors.New("name is not valid UTF-8")
	}
	if strings.HasPrefix(name, "/") {
		return errors.New("absolute path")
	}
	if strings.Contains(name, `\`) {
		return errors.New("backslash in path")
	}
	if strings.Contains(name, ":") {
		return errors.New("drive letter or colon in path")
	}
	for _, r := range name {
		if r < 0x20 || r == 0x7f {
			return errors.New("control character in path")
		}
	}
	for _, seg := range strings.Split(name, "/") {
		switch seg {
		case "":
			return errors.New("empty path segment")
		case ".", "..":
			return fmt.Errorf("path segment %q", seg)
		}
		// Windows: trailing dots/spaces are dropped and device names (CON, NUL, AUX.lua ...) open the device.
		if strings.HasSuffix(seg, ".") || strings.HasSuffix(seg, " ") {
			return fmt.Errorf("path segment %q ends in a dot or space", seg)
		}
		if reservedWindowsName(seg) {
			return fmt.Errorf("path segment %q is a reserved Windows device name", seg)
		}
	}
	return nil
}

// reservedWindowsName reports whether the segment's stem (up to the first dot) is CON, PRN, AUX, NUL, COM1-9 or LPT1-9,
// in any case.
func reservedWindowsName(seg string) bool {
	stem := seg
	if i := strings.IndexByte(stem, '.'); i >= 0 {
		stem = stem[:i]
	}
	stem = strings.ToUpper(strings.TrimSpace(stem))
	switch stem {
	case "CON", "PRN", "AUX", "NUL":
		return true
	}
	return len(stem) == 4 && (strings.HasPrefix(stem, "COM") || strings.HasPrefix(stem, "LPT")) && stem[3] >= '1' && stem[3] <= '9'
}

// Open validates a .uzplugin file completely: zip limits and names, manifest and entry file. All files are read once
// (enforcing the uncompressed limit on the real sizes) to compute the SHA-256 of each. SIGNATURE.json is kept
// aside and judged only by Verify.
func Open(data []byte) (*Package, error) {
	if len(data) > MaxCompressedBytes {
		return nil, invalid("file is larger than %d MiB", MaxCompressedBytes>>20)
	}
	zr, err := zip.NewReader(bytes.NewReader(data), int64(len(data)))
	if err != nil {
		return nil, invalid("not a zip file")
	}
	if len(zr.File) > MaxEntries {
		return nil, invalid("more than %d entries", MaxEntries)
	}
	p := &Package{data: data, zr: zr, Hashes: map[string]string{}, entries: map[string]*zip.File{}}
	lower := map[string]string{}
	var declared uint64
	for _, f := range zr.File {
		dir := strings.HasSuffix(f.Name, "/")
		name := strings.TrimSuffix(f.Name, "/")
		if err := SafeName(name); err != nil {
			return nil, invalid("entry %q: %v", f.Name, err)
		}
		if f.Mode()&os.ModeSymlink != 0 {
			return nil, invalid("entry %q is a symbolic link", f.Name)
		}
		if dir {
			continue
		}
		if !f.Mode().IsRegular() {
			return nil, invalid("entry %q is not a regular file", f.Name)
		}
		key := strings.ToLower(name)
		if other, dup := lower[key]; dup {
			return nil, invalid("duplicate entry name %q (also %q)", f.Name, other)
		}
		lower[key] = f.Name
		declared += f.UncompressedSize64
		if declared > MaxUncompressedBytes {
			return nil, invalid("uncompressed size exceeds %d MiB", MaxUncompressedBytes>>20)
		}
		p.Files = append(p.Files, f.Name)
		p.entries[f.Name] = f
	}
	// A file cannot also be the folder of another file (extraction would fail).
	for key, name := range lower {
		for i := strings.LastIndex(key, "/"); i > 0; i = strings.LastIndex(key[:i], "/") {
			if other, ok := lower[key[:i]]; ok {
				return nil, invalid("entry %q is both a file and the folder of %q", other, name)
			}
		}
	}
	var total int64
	for _, name := range p.Files {
		f := p.entries[name]
		rc, err := f.Open()
		if err != nil {
			return nil, invalid("entry %q cannot be read", name)
		}
		h := sha256.New()
		var w io.Writer = h
		var buf *bytes.Buffer
		if name == ManifestName || name == SignatureName {
			limit := uint64(MaxManifestBytes)
			if name == SignatureName {
				limit = MaxSignatureBytes
			}
			if f.UncompressedSize64 > limit {
				rc.Close()
				return nil, invalid("%s is larger than %d KiB", name, limit>>10)
			}
			buf = &bytes.Buffer{}
			w = io.MultiWriter(h, buf)
		}
		n, err := io.Copy(w, io.LimitReader(rc, int64(f.UncompressedSize64)+1))
		rc.Close()
		if err != nil {
			return nil, invalid("entry %q is damaged", name)
		}
		if uint64(n) != f.UncompressedSize64 {
			return nil, invalid("entry %q has a wrong size", name)
		}
		if total += n; total > MaxUncompressedBytes {
			return nil, invalid("uncompressed size exceeds %d MiB", MaxUncompressedBytes>>20)
		}
		p.Hashes[name] = hex.EncodeToString(h.Sum(nil))
		switch name {
		case ManifestName:
			p.RawManifest = buf.Bytes()
		case SignatureName:
			p.rawSig, p.hasSig = buf.Bytes(), true
		}
	}
	if p.RawManifest == nil {
		return nil, invalid("manifest.json is missing")
	}
	if p.Manifest, err = ParseManifest(p.RawManifest); err != nil {
		return nil, err
	}
	if _, ok := p.entries[p.Manifest.Entry]; !ok {
		return nil, invalid("entry file %q does not exist", p.Manifest.Entry)
	}
	return p, nil
}

// Data returns the file bytes Open was given.
func (p *Package) Data() []byte { return p.data }

// HasSignature reports whether SIGNATURE.json is present.
func (p *Package) HasSignature() bool { return p.hasSig }

// ParseManifest checks manifest.json by the app rules: api == 1, id, name, version (SemVer 2.0), type script,
// entry (relative .lua path), and the shape of requires, provides, permissions, games and assets. Other fields are
// ignored.
func ParseManifest(raw []byte) (Manifest, error) {
	var m Manifest
	if len(raw) > MaxManifestBytes {
		return m, invalid("manifest.json is larger than 256 KiB")
	}
	var o map[string]json.RawMessage
	if err := json.Unmarshal(raw, &o); err != nil || o == nil {
		return m, invalid("manifest.json is not a JSON object")
	}
	apiRaw, ok := o["api"]
	if !ok {
		return m, invalid("manifest field 'api' is required")
	}
	var api float64
	if json.Unmarshal(apiRaw, &api) != nil || api != float64(int(api)) {
		return m, invalid("manifest field 'api' must be an integer")
	}
	if int(api) != APIVersion {
		return m, invalid("plugin API %d is not supported (this server knows API %d)", int(api), APIVersion)
	}
	m.API = APIVersion
	str := func(key string, required bool) (string, error) {
		v, ok := o[key]
		if !ok || string(v) == "null" {
			if required {
				return "", invalid("manifest field '%s' is required", key)
			}
			return "", nil
		}
		var s string
		if json.Unmarshal(v, &s) != nil {
			return "", invalid("manifest field '%s' must be a string", key)
		}
		if required && strings.TrimSpace(s) == "" {
			return "", invalid("manifest field '%s' must not be empty", key)
		}
		return s, nil
	}
	var err error
	if m.ID, err = str("id", true); err != nil {
		return m, err
	}
	if !ValidID(m.ID) {
		return m, invalid("invalid plugin id %q: use reverse-DNS notation with lowercase letters, digits, '-' and '.'", m.ID)
	}
	if m.Name, err = str("name", true); err != nil {
		return m, err
	}
	if utf8.RuneCountInString(m.Name) > 100 {
		return m, invalid("manifest field 'name' is longer than 100 characters")
	}
	if m.Version, err = str("version", true); err != nil {
		return m, err
	}
	if !updates.ValidSemVer(m.Version) {
		return m, invalid("invalid version %q: use SemVer, e.g. 1.0.0", m.Version)
	}
	if m.Description, err = str("description", false); err != nil {
		return m, err
	}
	if m.RunUpdateFrom, err = str("run_update_from", false); err != nil {
		return m, err
	}
	if m.RunUpdateFrom != "" && !updates.ValidSemVer(m.RunUpdateFrom) {
		return m, invalid("invalid run_update_from %q: use SemVer, e.g. 1.0.0", m.RunUpdateFrom)
	}
	typ, err := str("type", false)
	if err != nil {
		return m, err
	}
	switch typ {
	case "", "script":
	case "tool":
		return m, invalid("tool plugins are not supported yet")
	default:
		return m, invalid("unknown plugin type %q", typ)
	}
	if m.Entry, err = str("entry", true); err != nil {
		return m, err
	}
	if SafeName(m.Entry) != nil || !strings.HasSuffix(m.Entry, ".lua") {
		return m, invalid("invalid entry %q: use a relative .lua path inside the plugin", m.Entry)
	}
	if v, ok := o["requires"]; ok && string(v) != "null" {
		var req map[string]json.RawMessage
		if json.Unmarshal(v, &req) != nil || req == nil {
			return m, invalid("manifest field 'requires' must be an object of plugin id to version range")
		}
		m.Requires = map[string]string{}
		for k, rv := range req {
			if !ValidID(k) {
				return m, invalid("requires: invalid plugin id %q", k)
			}
			if k == m.ID {
				return m, invalid("a plugin cannot require itself")
			}
			var rs string
			if json.Unmarshal(rv, &rs) != nil {
				return m, invalid("requires: the range for %s must be a string", k)
			}
			if !validRange(rs) {
				return m, invalid("requires: invalid version range %q for %s (use ^1.2, ~1.2, >=1.0, 1.2.3 or *)", rs, k)
			}
			m.Requires[k] = rs
		}
	}
	list := func(key string) ([]string, error) {
		v, ok := o[key]
		if !ok || string(v) == "null" {
			return nil, nil
		}
		var l []string
		if json.Unmarshal(v, &l) != nil {
			return nil, invalid("manifest field '%s' must be an array of strings", key)
		}
		for _, e := range l {
			if e == "" {
				return nil, invalid("manifest field '%s' must be an array of non-empty strings", key)
			}
		}
		return l, nil
	}
	if m.Provides, err = list("provides"); err != nil {
		return m, err
	}
	for _, p := range m.Provides {
		if len(p) > 128 || strings.Contains(p, " ") {
			return m, invalid("invalid provided interface name %q", p)
		}
	}
	perms, err := list("permissions")
	if err != nil {
		return m, err
	}
	for _, p := range perms {
		if err := permissionError(p); err != nil {
			return m, err
		}
		dup := false
		for _, q := range m.Permissions {
			dup = dup || q == p
		}
		if !dup {
			m.Permissions = append(m.Permissions, p)
		}
	}
	if len(m.Permissions) > 32 {
		return m, invalid("too many permissions")
	}
	if v, ok := o["games"]; ok && string(v) != "null" {
		var gs []json.RawMessage
		if json.Unmarshal(v, &gs) != nil {
			return m, invalid("manifest field 'games' must be an array")
		}
		for _, gv := range gs {
			var g struct {
				System *string         `json:"system"`
				Codes  json.RawMessage `json:"codes"`
			}
			if json.Unmarshal(gv, &g) != nil || g.System == nil || *g.System == "" {
				return m, invalid("games: every entry needs a 'system' string")
			}
			out := Game{System: *g.System}
			if len(g.Codes) > 0 && string(g.Codes) != "null" {
				if json.Unmarshal(g.Codes, &out.Codes) != nil {
					return m, invalid("games: 'codes' must be an array of glob strings")
				}
				for _, c := range out.Codes {
					if c == "" {
						return m, invalid("games: 'codes' must contain non-empty strings")
					}
				}
			}
			m.Games = append(m.Games, out)
		}
	}
	if v, ok := o["assets"]; ok && string(v) != "null" {
		var l []string
		if json.Unmarshal(v, &l) != nil {
			return m, invalid("manifest field 'assets' must be an array of strings")
		}
		for _, a := range l {
			// A folder is declared with a trailing slash ("i18n/", "icons/"), like the app accepts it.
			if SafeName(strings.TrimSuffix(a, "/")) != nil {
				return m, invalid("invalid asset path %q", a)
			}
		}
	}
	if v, ok := o["issues_url"]; ok && string(v) != "null" {
		var u string
		if json.Unmarshal(v, &u) != nil || !issuesURLRe.MatchString(u) {
			m.Warnings = append(m.Warnings, "issues_url must be a https://github.com/<owner>/<repo> URL; ignored")
		} else {
			m.IssuesURL = u
		}
	}
	if v, ok := o["run_settings"]; ok && string(v) != "null" {
		if m.RunSettings, err = parseRunSettings(v); err != nil {
			return m, err
		}
	}
	if v, ok := o["ui"]; ok && string(v) != "null" {
		if err := validateUI(v); err != nil {
			return m, err
		}
		if !slices.Contains(m.Permissions, "ui") {
			return m, invalid("field `ui` needs permission `ui`")
		}
	}
	return m, nil
}

// ---- Signature ----

// SignatureFile is the content of SIGNATURE.json.
type SignatureFile struct {
	Format    int               `json:"format"`
	Files     map[string]string `json:"files"`
	Signature string            `json:"signature"`
}

// SignedMessage is the byte string that is signed: "uzume-plugin-v1\n" and, for each file sorted by path (byte
// order), "<sha256>  <path>\n" (two spaces).
func SignedMessage(files map[string]string) []byte {
	names := make([]string, 0, len(files))
	for n := range files {
		names = append(names, n)
	}
	sort.Strings(names)
	var b bytes.Buffer
	b.WriteString(SignaturePrefix)
	for _, n := range names {
		b.WriteString(files[n])
		b.WriteString("  ")
		b.WriteString(n)
		b.WriteByte('\n')
	}
	return b.Bytes()
}

// SigState is the outcome of checking SIGNATURE.json.
type SigState int

// Signature states. SigInvalid is reported as such and never counts as official.
const (
	SigNone SigState = iota
	SigValid
	SigInvalid
)

// Verdict is the result of Verify.
type Verdict struct {
	State SigState
	// KeyID is the id of the trusted key that verified the signature (SigValid).
	KeyID string
	// Reason says why the signature is invalid (SigInvalid).
	Reason string
}

// Official reports whether the plugin carries a valid signature of a trusted key.
func (v Verdict) Official() bool { return v.State == SigValid }

// Verify checks SIGNATURE.json against the trusted keys: it must list exactly the non-directory entries except
// itself with their SHA-256, and the Ed25519 signature over SignedMessage must verify with one of the keys.
func (p *Package) Verify(keys []ed25519.PublicKey) Verdict {
	if !p.hasSig {
		return Verdict{State: SigNone}
	}
	bad := func(format string, a ...any) Verdict {
		return Verdict{State: SigInvalid, Reason: fmt.Sprintf(format, a...)}
	}
	var sf SignatureFile
	dec := json.NewDecoder(bytes.NewReader(p.rawSig))
	if err := dec.Decode(&sf); err != nil || dec.More() {
		return bad("SIGNATURE.json is not valid")
	}
	if sf.Format != 1 {
		return bad("unknown signature format %d", sf.Format)
	}
	for name, h := range p.Hashes {
		if name == SignatureName {
			continue
		}
		want, ok := sf.Files[name]
		switch {
		case !ok:
			return bad("file %q is not covered by the signature", name)
		case want != h:
			return bad("file %q was changed after signing", name)
		}
	}
	for name := range sf.Files {
		if _, ok := p.Hashes[name]; !ok || name == SignatureName {
			return bad("signed file %q is missing", name)
		}
	}
	sig, err := base64.StdEncoding.DecodeString(sf.Signature)
	if err != nil || len(sig) != ed25519.SignatureSize {
		return bad("signature is not base64 of 64 bytes")
	}
	if len(keys) == 0 {
		return bad("no trusted signing key configured")
	}
	msg := SignedMessage(sf.Files)
	for _, k := range keys {
		if ed25519.Verify(k, msg, sig) {
			return Verdict{State: SigValid, KeyID: corepkg.KeyID(k)}
		}
	}
	return bad("not signed by a trusted key")
}

// Sign returns the file with SIGNATURE.json added or replaced (all other entries are copied unchanged, the new
// SIGNATURE.json comes last). The input must be a valid plugin; the output is validated again.
func Sign(data []byte, seed []byte) ([]byte, error) {
	if _, err := corepkg.PublicFromSeed(seed); err != nil {
		return nil, err
	}
	p, err := Open(data)
	if err != nil {
		return nil, err
	}
	files := map[string]string{}
	for n, h := range p.Hashes {
		if n != SignatureName {
			files[n] = h
		}
	}
	sig := ed25519.Sign(ed25519.NewKeyFromSeed(seed), SignedMessage(files))
	sj, err := json.MarshalIndent(SignatureFile{Format: 1, Files: files, Signature: base64.StdEncoding.EncodeToString(sig)}, "", "  ")
	if err != nil {
		return nil, err
	}
	var out bytes.Buffer
	zw := zip.NewWriter(&out)
	for _, f := range p.zr.File {
		if strings.TrimSuffix(f.Name, "/") == SignatureName {
			continue
		}
		if err := zw.Copy(f); err != nil {
			return nil, err
		}
	}
	w, err := zw.CreateHeader(&zip.FileHeader{Name: SignatureName, Method: zip.Deflate})
	if err != nil {
		return nil, err
	}
	if _, err := w.Write(append(sj, '\n')); err != nil {
		return nil, err
	}
	if err := zw.Close(); err != nil {
		return nil, err
	}
	if _, err := Open(out.Bytes()); err != nil {
		return nil, err
	}
	return out.Bytes(), nil
}

// SHA256Hex returns the lowercase hex SHA-256 of data.
func SHA256Hex(data []byte) string {
	h := sha256.Sum256(data)
	return hex.EncodeToString(h[:])
}
