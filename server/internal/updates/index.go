package updates

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"net/url"
	"regexp"
	"slices"
	"sort"
	"strings"
	"time"
	"unicode/utf8"
)

// Schema is the supported updates index schema version.
const Schema = 1

// DefaultIndexURL is the fixed source of the signed updates index; the signature is at the same URL + ".sig".
const DefaultIndexURL = "https://github.com/phabioo/uzume/releases/download/updates-index/updates-index.json"

// DefaultPluginIndexURL is the fixed source of the official signed plugin index (release "plugin-index"); the
// signature is at the same URL + ".sig". The server value "none" disables the plugin index.
const DefaultPluginIndexURL = "https://github.com/phabioo/uzume/releases/download/plugin-index/plugin-index.json"

// Limits.
const (
	MaxIndexBytes    = 4 << 20
	MaxSigBytes      = 4 << 10
	MaxArtifactSize  = 1 << 30
	MaxReleases      = 2048
	MaxArtifacts     = 16
	maxTextLen       = 512
	maxSchemaVersion = 100000
	// MaxNotesBytes is the limit of Release.Notes (UTF-8 bytes).
	MaxNotesBytes = 4000
)

// Products, channels, platforms and kinds.
const (
	ProductHub    = "hub"
	ProductPlayer = "player"

	ChannelStable = "stable"
	ChannelBeta   = "beta"
	ChannelDev    = "dev" // compiled-in default only; never part of an index

	KindDeb       = "deb"
	KindMSI       = "msi"
	KindBinary    = "binary"
	KindInstaller = "installer"
	KindZip       = "zip"
	KindAppImage  = "appimage"
)

var (
	sha256Re = regexp.MustCompile(`^[0-9a-f]{64}$`)
	commitRe = regexp.MustCompile(`^[0-9a-f]{40}$`)
	nameRe   = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._~+-]{0,127}$`)
)

// platformKinds lists the platforms of each product with their allowed artifact kinds.
var platformKinds = map[string]map[string][]string{
	ProductHub: {
		"linux-amd64":   {KindDeb, KindBinary},
		"linux-arm64":   {KindDeb, KindBinary},
		"windows-amd64": {KindMSI, KindBinary},
	},
	ProductPlayer: {
		"windows-x64": {KindInstaller, KindZip, KindMSI},
		"linux-x64":   {KindAppImage},
	},
}

// Artifact is one downloadable file of a release.
type Artifact struct {
	Platform string `json:"platform"`
	Kind     string `json:"kind"`
	Name     string `json:"name"`
	Size     int64  `json:"size"`
	SHA256   string `json:"sha256"`
	URL      string `json:"url"`
}

// Release is one (product, channel, version) entry of the index.
type Release struct {
	Product     string    `json:"product"`
	Channel     string    `json:"channel"`
	Version     string    `json:"version"`
	Commit      string    `json:"commit"`
	PublishedAt time.Time `json:"published_at"`
	NotesURL    string    `json:"notes_url,omitempty"`
	// Notes is the optional short plain-text "What's new" (groups like "### Added", bullets "- line"), signed with the index.
	Notes              string `json:"notes,omitempty"`
	ProtocolVersion    int    `json:"protocol_version"`
	MinProtocolVersion int    `json:"min_protocol_version"`
	// SchemaVersion is the database schema version of a server release (the newest migration its binary knows); 0 or
	// absent = unknown (releases from before rollback support). Rollback needs it: migrations are forward-only.
	SchemaVersion int `json:"schema_version,omitempty"`
	// MSIDowngrade marks a release whose MSI can be installed over a newer version of Uzume (MajorUpgrade
	// AllowDowngrades); older MSIs refuse that, so they are not offered as a rollback target on Windows.
	MSIDowngrade bool       `json:"msi_downgrade,omitempty"`
	Artifacts    []Artifact `json:"artifacts"`
}

// Index is the content of updates-index.json.
type Index struct {
	Schema      int       `json:"schema"`
	GeneratedAt time.Time `json:"generated_at"`
	// ExpiresAt is the end of the validity of this signed index (generated_at + IndexValidity when it is created).
	// Server and app refuse an index after that date, so a replayed old index cannot hide new releases forever.
	// Absent in indexes of earlier releases; such an index is accepted during the transition (ADR 0016 amendment).
	ExpiresAt time.Time `json:"expires_at,omitzero"`
	Releases  []Release `json:"releases"`
}

// IndexValidity is how long a freshly signed index stays valid. The index is re-signed at every release and at
// least weekly by the index-refresh workflow, so it only expires when publishing stops for weeks.
const IndexValidity = 60 * 24 * time.Hour

// ErrIndexExpired is returned by CheckFresh for an index past its expires_at.
var ErrIndexExpired = errors.New("update index has expired")

// CheckFresh rejects an index whose signed expires_at lies before now. An index without expires_at passes
// (transition).
func (idx Index) CheckFresh(now time.Time) error {
	if !idx.ExpiresAt.IsZero() && now.After(idx.ExpiresAt) {
		return fmt.Errorf("%w (expired %s; check the date of this machine, or the index source is stale)", ErrIndexExpired, idx.ExpiresAt.UTC().Format(time.RFC3339))
	}
	return nil
}

// ReleaseError reports one release that failed validation and was skipped. Every other error returned by
// ParseIndex is fatal: the whole index must be rejected.
type ReleaseError struct {
	Product, Channel, Version string
	Err                       error
}

func (e *ReleaseError) Error() string {
	return fmt.Sprintf("release %s %s %s: %v", e.Product, e.Channel, e.Version, e.Err)
}
func (e *ReleaseError) Unwrap() error { return e.Err }

// Fatal reports whether errs contains an error that makes the index unusable.
func Fatal(errs []error) bool {
	for _, e := range errs {
		var re *ReleaseError
		if !errors.As(e, &re) {
			return true
		}
	}
	return false
}

// ParseIndex parses and validates an index. Releases that fail validation are skipped and reported as
// *ReleaseError; fatal errors (bad JSON, unknown schema, duplicate releases) return an empty Index.
func ParseIndex(data []byte) (Index, []error) { return ParseIndexOpts(data, false) }

// ParseIndexOpts is ParseIndex with allowFile: accept file:// artifact URLs (only for an index loaded from file://).
func ParseIndexOpts(data []byte, allowFile bool) (Index, []error) {
	if len(data) > MaxIndexBytes {
		return Index{}, []error{errors.New("index is too large")}
	}
	var idx Index
	dec := json.NewDecoder(bytes.NewReader(data))
	if err := dec.Decode(&idx); err != nil {
		return Index{}, []error{fmt.Errorf("index is not valid JSON: %w", err)}
	}
	if dec.More() {
		return Index{}, []error{errors.New("index has trailing data")}
	}
	if idx.Schema != Schema {
		return Index{}, []error{fmt.Errorf("unknown index schema %d", idx.Schema)}
	}
	if len(idx.Releases) > MaxReleases {
		return Index{}, []error{errors.New("index has too many releases")}
	}
	seen := map[string]bool{}
	for _, r := range idx.Releases {
		k := r.Product + "\x00" + r.Channel + "\x00" + r.Version
		if seen[k] {
			return Index{}, []error{fmt.Errorf("duplicate release %s %s %s", r.Product, r.Channel, r.Version)}
		}
		seen[k] = true
	}
	var errs []error
	valid := idx.Releases[:0:0]
	for _, r := range idx.Releases {
		r = dropUnknownArtifacts(r)
		if err := validateRelease(r, allowFile); err != nil {
			errs = append(errs, &ReleaseError{r.Product, r.Channel, r.Version, err})
			continue
		}
		valid = append(valid, r)
	}
	idx.Releases = valid
	return idx, errs
}

// dropUnknownArtifacts removes artifacts whose platform is unknown for the product or whose kind is not allowed
// on it, so an index written for newer consumers stays readable (the consumer path only; ValidateRelease is
// strict). A release left without artifacts then fails validation and is skipped.
func dropUnknownArtifacts(r Release) Release {
	kinds, ok := platformKinds[r.Product]
	if !ok {
		return r
	}
	var keep []Artifact
	for _, a := range r.Artifacts {
		if slices.Contains(kinds[a.Platform], a.Kind) {
			keep = append(keep, a)
		}
	}
	r.Artifacts = keep
	return r
}

// legacyArtifact reports whether an artifact is understood by servers <= 0.8.0 (linux-amd64/arm64 deb/binary).
func legacyArtifact(a Artifact) bool {
	return (a.Platform == "linux-amd64" || a.Platform == "linux-arm64") && (a.Kind == KindDeb || a.Kind == KindBinary)
}

const legacySuffix = "-legacy"

// IsLegacyCompanion reports whether version is a legacy companion entry (X.Y.Z-legacy, see LegacyCompanion).
func IsLegacyCompanion(version string) bool {
	base, ok := strings.CutSuffix(version, legacySuffix)
	return ok && ValidSemVer(version) && !strings.ContainsAny(base, "-+")
}

// LegacyCompanion returns the companion entry of a hub release for servers <= 0.8.0, which reject a whole release
// when it has an artifact of an unknown platform or kind (e.g. windows-amd64) and so skip every newer release.
// The companion has version <X.Y.Z>-legacy and only the artifacts those servers understand; the deb inside is the
// regular X.Y.Z, so after installing it they report X.Y.Z and are up to date. A prerelease (not build metadata)
// is used because the app's index parser keys duplicates on the version without build metadata. It sorts
// just below X.Y.Z, so newer servers below X.Y.Z prefer X.Y.Z, and Windows servers never select it. ok is false when
// no companion is needed: not a hub release, base version has a prerelease or build metadata, all artifacts are
// legacy already, or there is no legacy deb.
func LegacyCompanion(r Release) (Release, bool) {
	if r.Product != ProductHub || strings.ContainsAny(r.Version, "-+") {
		return Release{}, false
	}
	var legacy []Artifact
	other, deb := false, false
	for _, a := range r.Artifacts {
		if legacyArtifact(a) {
			legacy = append(legacy, a)
			deb = deb || a.Kind == KindDeb
		} else {
			other = true
		}
	}
	if !other || !deb {
		return Release{}, false
	}
	r.Version += legacySuffix
	r.Artifacts = legacy
	return r, true
}

// ValidateRelease checks one release entry.
func ValidateRelease(r Release) error { return validateRelease(r, false) }

// ValidateReleaseOpts is ValidateRelease with allowFile: accept file:// artifact URLs (tests only).
func ValidateReleaseOpts(r Release, allowFile bool) error { return validateRelease(r, allowFile) }

// ValidateNotes checks the optional release notes: at most MaxNotesBytes, valid UTF-8, no control characters except
// newline and tab.
func ValidateNotes(n string) error {
	if len(n) > MaxNotesBytes {
		return errors.New("notes too long")
	}
	if !utf8.ValidString(n) {
		return errors.New("notes are not valid UTF-8")
	}
	for _, c := range n {
		if c != '\n' && c != '\t' && (c < 0x20 || (c >= 0x7f && c <= 0x9f)) {
			return errors.New("notes contain control characters")
		}
	}
	return nil
}

func validateRelease(r Release, allowFile bool) error {
	kinds, ok := platformKinds[r.Product]
	if !ok {
		return errors.New("unknown product")
	}
	if r.Channel != ChannelStable && r.Channel != ChannelBeta {
		return errors.New("channel must be stable or beta")
	}
	if !ValidSemVer(r.Version) {
		return errors.New("version is not SemVer 2.0")
	}
	if r.Commit != "" && !commitRe.MatchString(r.Commit) {
		return errors.New("commit must be empty or 40 lowercase hex characters")
	}
	if r.PublishedAt.IsZero() {
		return errors.New("published_at is missing")
	}
	if len(r.NotesURL) > maxTextLen {
		return errors.New("notes_url too long")
	}
	if r.NotesURL != "" {
		if u, err := url.Parse(r.NotesURL); err != nil || u.Scheme != "https" || u.Host == "" || u.User != nil {
			return errors.New("notes_url must be https")
		}
	}
	if err := ValidateNotes(r.Notes); err != nil {
		return err
	}
	if r.ProtocolVersion < 1 || r.MinProtocolVersion < 1 || r.MinProtocolVersion > r.ProtocolVersion {
		return errors.New("invalid protocol_version/min_protocol_version")
	}
	if r.SchemaVersion < 0 || r.SchemaVersion > maxSchemaVersion {
		return errors.New("invalid schema_version")
	}
	if len(r.Artifacts) == 0 || len(r.Artifacts) > MaxArtifacts {
		return errors.New("a release needs 1 to 16 artifacts")
	}
	type pk struct{ p, k string }
	seen := map[pk]bool{}
	for _, a := range r.Artifacts {
		allowed, ok := kinds[a.Platform]
		if !ok {
			return fmt.Errorf("artifact %s: unknown platform %q for product %s", a.Name, a.Platform, r.Product)
		}
		found := false
		for _, k := range allowed {
			found = found || k == a.Kind
		}
		if !found {
			return fmt.Errorf("artifact %s: kind %q not allowed on %s", a.Name, a.Kind, a.Platform)
		}
		if seen[pk{a.Platform, a.Kind}] {
			return fmt.Errorf("duplicate artifact for %s/%s", a.Platform, a.Kind)
		}
		seen[pk{a.Platform, a.Kind}] = true
		if !nameRe.MatchString(a.Name) || strings.Contains(a.Name, "..") {
			return fmt.Errorf("invalid artifact name %q", a.Name)
		}
		if a.Size <= 0 || a.Size > MaxArtifactSize {
			return fmt.Errorf("artifact %s: invalid size", a.Name)
		}
		if !sha256Re.MatchString(a.SHA256) {
			return fmt.Errorf("artifact %s: sha256 must be 64 lowercase hex characters", a.Name)
		}
		if len(a.URL) > 2048 {
			return fmt.Errorf("artifact %s: url too long", a.Name)
		}
		// https only; consumers accept file:// additionally when the index itself came from file:// (tests).
		if _, err := parseSourceURL(a.URL, allowFile); err != nil {
			return fmt.Errorf("artifact %s: url must be https", a.Name)
		}
		if !strings.HasPrefix(a.URL, "file:") && strings.IndexFunc(a.URL, unsafeURLRune) >= 0 {
			return fmt.Errorf("artifact %s: url contains characters that are not allowed (space, quote, shell metacharacter or control character)", a.Name)
		}
	}
	return nil
}

// unsafeURLRune reports characters that must not be in an artifact URL: the server shows "curl -fLO <url>" for
// copy and paste (HC-13), so spaces, quotes and shell metacharacters are rejected at the source.
func unsafeURLRune(r rune) bool {
	return r <= ' ' || r == 0x7f || strings.ContainsRune("\"'`$;&|<>\\(){}[]*?!#^", r)
}

// SortReleases orders releases by product, channel, then SemVer ascending.
func SortReleases(rs []Release) {
	sort.SliceStable(rs, func(i, j int) bool {
		a, b := rs[i], rs[j]
		if a.Product != b.Product {
			return a.Product < b.Product
		}
		if a.Channel != b.Channel {
			return a.Channel < b.Channel
		}
		return MustCompare(a.Version, b.Version) < 0
	})
}

// Prune keeps only the newest keep releases per (product, channel) by SemVer. Input order is irrelevant; the
// result is sorted with SortReleases.
func Prune(rs []Release, keep int) []Release {
	SortReleases(rs)
	var out []Release
	for i := 0; i < len(rs); {
		j := i
		for j < len(rs) && rs[j].Product == rs[i].Product && rs[j].Channel == rs[i].Channel {
			j++
		}
		from := i
		if keep > 0 && j-i > keep {
			from = j - keep
		}
		out = append(out, rs[from:j]...)
		i = j
	}
	return out
}

// Marshal returns the deterministic index JSON (sorted releases, 2-space indent, trailing newline).
func Marshal(idx Index) ([]byte, error) {
	SortReleases(idx.Releases)
	for i := range idx.Releases {
		a := idx.Releases[i].Artifacts
		sort.SliceStable(a, func(x, y int) bool {
			if a[x].Platform != a[y].Platform {
				return a[x].Platform < a[y].Platform
			}
			return a[x].Kind < a[y].Kind
		})
	}
	if idx.Releases == nil {
		idx.Releases = []Release{}
	}
	b, err := json.MarshalIndent(idx, "", "  ")
	if err != nil {
		return nil, err
	}
	return append(b, '\n'), nil
}

// LooksLikeUpdatesIndex reports whether data is a JSON object with a "releases" key (used by uzume-sign to
// tell an updates index from a core index).
func LooksLikeUpdatesIndex(data []byte) bool {
	var probe map[string]json.RawMessage
	if json.Unmarshal(data, &probe) != nil {
		return false
	}
	_, ok := probe["releases"]
	return ok
}
