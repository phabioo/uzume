package pluginpkg

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"net/url"
	"regexp"
	"sort"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/phabioo/uzume/server/internal/updates"
)

// IndexFormat is the supported plugin index format.
const IndexFormat = 1

// Index limits.
const (
	MaxIndexBytes   = 4 << 20
	MaxIndexEntries = 2048
	maxDescription  = 1000
)

var sha256Re = regexp.MustCompile(`^[0-9a-f]{64}$`)

// IndexEntry is one (id, version) of the plugin index; the fields are copied from the manifest of the signed file.
type IndexEntry struct {
	ID          string   `json:"id"`
	Name        string   `json:"name"`
	Version     string   `json:"version"`
	API         int      `json:"api"`
	Description string   `json:"description"`
	Permissions []string `json:"permissions"`
	URL         string   `json:"url"`
	SHA256      string   `json:"sha256"`
	Size        int64    `json:"size"`
	// Replaces are the ids of plugins this one takes over (copied from the manifest); old servers ignore the field.
	Replaces []string `json:"replaces,omitempty"`
}

// Index is the content of plugin-index.json (signed by a detached plugin-index.json.sig, like the updates index).
type Index struct {
	Format      int       `json:"format"`
	GeneratedAt time.Time `json:"generated_at"`
	// ExpiresAt: see updates.Index.ExpiresAt; absent in indexes signed before the ADR 0016 amendment 2026-10-19.
	ExpiresAt time.Time    `json:"expires_at,omitzero"`
	Plugins   []IndexEntry `json:"plugins"`
}

// IndexValidity is how long a freshly signed plugin index stays valid (it is re-signed at every plugin release and weekly).
const IndexValidity = 60 * 24 * time.Hour

// ErrIndexExpired is returned by CheckFresh for an index past its expires_at.
var ErrIndexExpired = errors.New("plugin index has expired")

// CheckFresh rejects an index whose signed expires_at lies before now; an index without expires_at passes (transition).
func (idx Index) CheckFresh(now time.Time) error {
	if !idx.ExpiresAt.IsZero() && now.After(idx.ExpiresAt) {
		return fmt.Errorf("%w (expired %s)", ErrIndexExpired, idx.ExpiresAt.UTC().Format(time.RFC3339))
	}
	return nil
}

// EntryError reports one entry that failed validation and was skipped by ParseIndex.
type EntryError struct {
	ID, Version string
	Err         error
}

func (e *EntryError) Error() string { return fmt.Sprintf("plugin %s %s: %v", e.ID, e.Version, e.Err) }
func (e *EntryError) Unwrap() error { return e.Err }

// Fatal reports whether errs contains an error that makes the whole index unusable.
func Fatal(errs []error) bool {
	for _, e := range errs {
		var ee *EntryError
		if !errors.As(e, &ee) {
			return true
		}
	}
	return false
}

// LooksLikeIndex reports whether data is a JSON object with a "plugins" key (to tell the index from the others).
func LooksLikeIndex(data []byte) bool {
	var probe map[string]json.RawMessage
	if json.Unmarshal(data, &probe) != nil {
		return false
	}
	_, ok := probe["plugins"]
	return ok
}

// ValidateEntry checks one entry. allowFile accepts file:// URLs (only for an index loaded from file://).
func ValidateEntry(e IndexEntry, allowFile bool) error {
	switch {
	case !ValidID(e.ID):
		return errors.New("invalid id")
	case e.Name == "" || utf8.RuneCountInString(e.Name) > 100:
		return errors.New("name must be 1 to 100 characters")
	case !updates.ValidSemVer(e.Version):
		return errors.New("version is not SemVer")
	case e.API != APIVersion:
		return fmt.Errorf("api %d is not supported", e.API)
	case utf8.RuneCountInString(e.Description) > maxDescription:
		return errors.New("description is too long")
	case len(e.Permissions) > 32:
		return errors.New("too many permissions")
	case !sha256Re.MatchString(e.SHA256):
		return errors.New("sha256 must be 64 lowercase hex characters")
	case e.Size <= 0 || e.Size > MaxCompressedBytes:
		return errors.New("size must be 1 byte to 32 MiB")
	}
	if err := ValidateReplaces(e.ID, e.Replaces); err != nil {
		return err
	}
	for _, p := range e.Permissions {
		if p == "" || len(p) > 64 {
			return errors.New("invalid permission")
		}
	}
	u, err := url.Parse(e.URL)
	if err != nil {
		return errors.New("invalid url")
	}
	switch {
	case u.Scheme == "https" && u.Host != "" && u.User == nil:
	case allowFile && u.Scheme == "file" && (u.Host == "" || u.Host == "localhost") && u.Path != "":
	case allowFile:
		return errors.New("url must be https or file://")
	default:
		return errors.New("url must be https")
	}
	return nil
}

// ParseIndex parses and validates an index. Entries that fail validation are skipped and reported as *EntryError;
// fatal errors (bad JSON, unknown format, duplicate (id, version), too large) return an empty Index.
func ParseIndex(data []byte, allowFile bool) (Index, []error) {
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
	if idx.Format != IndexFormat {
		return Index{}, []error{fmt.Errorf("unknown index format %d", idx.Format)}
	}
	if len(idx.Plugins) > MaxIndexEntries {
		return Index{}, []error{errors.New("index has too many plugins")}
	}
	seen := map[string]bool{}
	for _, e := range idx.Plugins {
		k := e.ID + "\x00" + e.Version
		if seen[k] {
			return Index{}, []error{fmt.Errorf("duplicate plugin %s %s", e.ID, e.Version)}
		}
		seen[k] = true
	}
	var errs []error
	valid := idx.Plugins[:0:0]
	for _, e := range idx.Plugins {
		if err := ValidateEntry(e, allowFile); err != nil {
			errs = append(errs, &EntryError{e.ID, e.Version, err})
			continue
		}
		valid = append(valid, e)
	}
	idx.Plugins = valid
	return idx, errs
}

// SortEntries orders entries by id, then by SemVer (newest last).
func SortEntries(es []IndexEntry) {
	sort.SliceStable(es, func(i, j int) bool {
		if es[i].ID != es[j].ID {
			return es[i].ID < es[j].ID
		}
		a, errA := updates.ParseSemVer(es[i].Version)
		b, errB := updates.ParseSemVer(es[j].Version)
		if errA != nil || errB != nil {
			return es[i].Version < es[j].Version
		}
		if c := a.Compare(b); c != 0 {
			return c < 0
		}
		return es[i].Version < es[j].Version
	})
}

// MarshalIndex writes deterministic, indented JSON with a trailing newline.
func MarshalIndex(idx Index) ([]byte, error) {
	for i := range idx.Plugins {
		if idx.Plugins[i].Permissions == nil {
			idx.Plugins[i].Permissions = []string{}
		}
	}
	if idx.Plugins == nil {
		idx.Plugins = []IndexEntry{}
	}
	b, err := json.MarshalIndent(idx, "", "  ")
	if err != nil {
		return nil, err
	}
	return append(b, '\n'), nil
}

// EntryFor builds the index entry of a validated, signed package that is published at rawURL.
func EntryFor(p *Package, rawURL string) IndexEntry {
	desc := p.Manifest.Description
	if r := []rune(desc); len(r) > maxDescription {
		desc = string(r[:maxDescription])
	}
	perms := p.Manifest.Permissions
	if perms == nil {
		perms = []string{}
	}
	return IndexEntry{ID: p.Manifest.ID, Name: p.Manifest.Name, Version: p.Manifest.Version, API: p.Manifest.API,
		Description: strings.TrimSpace(desc), Permissions: perms, URL: rawURL, SHA256: SHA256Hex(p.Data()), Size: int64(len(p.Data())), Replaces: p.Manifest.Replaces}
}
