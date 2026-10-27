package hub

import (
	"archive/zip"
	"context"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

// Tools catalog (0.21, ADR 0031 D3): external programs the server offers to apps the way it offers cores. The only tool
// is the randomizer engine (UPR FVX). The server downloads a release zip per platform from the official source, pins
// its SHA-256 at the first download, keeps a version while a variant or run uses it and imports offline. Apps download
// tools only from their server; the server never runs a tool.

// Handshake features of the tools catalog and of game variants.
const (
	FeatureToolsV1    = "tools_v1"
	FeatureVariantsV1 = "variants_v1"
)

// ToolRandomizer is the id of the randomizer engine.
const ToolRandomizer = "randomizer"

// DefaultToolsSourceURL is the GitHub releases API of the randomizer engine; "none" turns the source off.
const DefaultToolsSourceURL = "https://api.github.com/repos/upr-fvx/universal-pokemon-randomizer-fvx/releases"

// Tool origins of a cached file.
const (
	ToolOriginGitHub = "github"
	ToolOriginImport = "import"
)

// Tool source states.
const (
	ToolSourceOff   = "off"   // no source configured
	ToolSourceNever = "never" // configured, not asked yet
	ToolSourceOK    = "ok"
	ToolSourceError = "error" // the last check failed (last_error)
)

const (
	defaultMaxToolZipBytes = int64(400) << 20 // one downloaded platform zip
	toolListMaxBytes       = 8 << 20          // release list
	toolFileTimeout        = 30 * time.Minute
	maxToolZipEntries      = 100000
)

// Platforms a tool is offered for (the platform names of the core catalog).
var toolPlatforms = []string{"windows-x64", "linux-x64", "linux-arm64"}

// toolAssetRe matches the release zips of the randomizer engine: UPR_FVX-v1_6_2-Windows.zip, -Linux_x86.zip, -Linux_ARM.zip.
var toolAssetRe = regexp.MustCompile(`^UPR_FVX-v(\d{1,4})_(\d{1,4})_(\d{1,4})-(Windows|Linux_x86|Linux_ARM)\.zip$`)

var toolVersionRe = regexp.MustCompile(`^\d{1,4}\.\d{1,4}\.\d{1,4}$`)

var toolAssetPlatform = map[string]string{"Windows": "windows-x64", "Linux_x86": "linux-x64", "Linux_ARM": "linux-arm64"}

type toolDef struct {
	ID, Name, License, SourceURL string
}

var toolDefs = []toolDef{{ID: ToolRandomizer, Name: "Randomizer engine", License: "GPL-3.0",
	SourceURL: "https://github.com/upr-fvx/universal-pokemon-randomizer-fvx"}}

func findToolDef(id string) (toolDef, bool) {
	for _, d := range toolDefs {
		if d.ID == id {
			return d, true
		}
	}
	return toolDef{}, false
}

func validToolPlatform(p string) bool {
	for _, x := range toolPlatforms {
		if x == p {
			return true
		}
	}
	return false
}

type toolState struct {
	srcURL string // "" = off
	maxZip int64

	mu       sync.Mutex   // serializes install, remove, import and sync
	usageMu  sync.RWMutex // variant and run creation (R) against removing a version (W): "in use" is checked under it
	kickSync chan struct{}
}

func (s *Service) initTools(o Options) {
	u := strings.TrimSpace(o.ToolsSourceURL)
	if strings.EqualFold(u, "none") {
		u = ""
	}
	s.tools.srcURL = u
	s.tools.maxZip = o.ToolMaxZipBytes
	if s.tools.maxZip <= 0 {
		s.tools.maxZip = defaultMaxToolZipBytes
	}
	s.tools.kickSync = make(chan struct{}, 1)
}

// toolDir is <data>/tools/<tool>/<version>, or "" for an unknown tool or a malformed version. The path is built from
// the catalog's own tool id and a version that matched toolVersionRe, never from raw input.
func (s *Service) toolDir(toolID, version string) string {
	def, ok := findToolDef(toolID)
	if !ok || !toolVersionRe.MatchString(version) {
		return ""
	}
	return filepath.Join(s.dataDir, "tools", def.ID, version)
}

// toolFile is the cached platform zip, or "" when toolDir is "" or the platform is not one of toolPlatforms.
func (s *Service) toolFile(toolID, version, platform string) string {
	dir := s.toolDir(toolID, version)
	if dir == "" {
		return ""
	}
	for _, p := range toolPlatforms {
		if p == platform {
			return filepath.Join(dir, p+".zip")
		}
	}
	return ""
}

// ---- model ----

// ToolUsage counts what pins a tool version: variants (not deleted) and runs that are not finished or abandoned.
type ToolUsage struct {
	Variants int `json:"variants"`
	Runs     int `json:"runs"`
}

// ToolPlatform is one platform zip of a tool version.
type ToolPlatform struct {
	Platform string
	Size     int64
	SHA256   string // pinned at the first download or import; empty while never downloaded
	Cached   bool   // the zip is in the server cache
	Origin   string // github or import
	// Root is the folder inside the zip that holds UPR-FVX.jar ("" = the zip root); empty while not cached.
	Root string
	URL  string // download URL of the release (empty for imports without a known release)
}

// ToolVersion is one version of a tool.
type ToolVersion struct {
	Version   string
	Platforms []ToolPlatform
	Installed bool // at least one platform is cached
	Complete  bool // every known platform is cached
	UsedBy    ToolUsage
}

// ToolSource is the state of the release source of a tool.
type ToolSource struct {
	URL         string
	State       string
	LastCheck   *time.Time
	LastSuccess *time.Time
	LastError   string
}

// Tool is a tool of the catalog with its versions (newest first).
type Tool struct {
	ID, Name, License, SourceURL string
	Offered                      bool
	// LatestVersion is the newest release the source listed (empty before the first successful sync).
	LatestVersion string
	Versions      []ToolVersion
	Source        ToolSource
}

// NewestCached returns the newest version with a cached platform ("" when there is none).
func (t Tool) NewestCached() string {
	for _, v := range t.Versions {
		if v.Installed {
			return v.Version
		}
	}
	return ""
}

// UpdateAvailable reports a release newer than the newest cached version (or a release when nothing is cached).
func (t Tool) UpdateAvailable() bool {
	n := t.NewestCached()
	return t.LatestVersion != "" && (n == "" || compareVersions(t.LatestVersion, n) > 0)
}

// compareVersions compares dotted numeric versions ("1.6.2"); non-numeric parts count as 0.
func compareVersions(a, b string) int {
	pa, pb := strings.Split(a, "."), strings.Split(b, ".")
	for i := 0; i < max(len(pa), len(pb)); i++ {
		var x, y int
		if i < len(pa) {
			x, _ = strconv.Atoi(pa[i])
		}
		if i < len(pb) {
			y, _ = strconv.Atoi(pb[i])
		}
		if x != y {
			if x < y {
				return -1
			}
			return 1
		}
	}
	return 0
}

type toolRow struct {
	version, platform, url, sha, root, origin string
	size                                      int64
	cached                                    bool
}

// ---- reads ----

// ListTools returns every tool with all versions (the admin view). Apps use AppTools.
func (s *Service) ListTools(ctx context.Context) ([]Tool, error) {
	out := make([]Tool, 0, len(toolDefs))
	for _, d := range toolDefs {
		t, err := s.loadTool(ctx, d)
		if err != nil {
			return nil, err
		}
		out = append(out, t)
	}
	return out, nil
}

// GetTool returns one tool (admin view); ErrNotFound for an unknown id.
func (s *Service) GetTool(ctx context.Context, id string) (Tool, error) {
	d, ok := findToolDef(id)
	if !ok {
		return Tool{}, ErrNotFound
	}
	return s.loadTool(ctx, d)
}

// AppTools returns what apps may use: offered tools only, and of those only the versions and platforms that are cached.
func (s *Service) AppTools(ctx context.Context) ([]Tool, error) {
	all, err := s.ListTools(ctx)
	if err != nil {
		return nil, err
	}
	out := []Tool{}
	for _, t := range all {
		if !t.Offered {
			continue
		}
		vs := []ToolVersion{}
		for _, v := range t.Versions {
			var ps []ToolPlatform
			for _, p := range v.Platforms {
				if p.Cached {
					ps = append(ps, p)
				}
			}
			if len(ps) == 0 {
				continue
			}
			v.Platforms = ps
			vs = append(vs, v)
		}
		t.Versions = vs
		out = append(out, t)
	}
	return out, nil
}

func (s *Service) loadTool(ctx context.Context, d toolDef) (Tool, error) {
	t := Tool{ID: d.ID, Name: d.Name, License: d.License, SourceURL: d.SourceURL, Versions: []ToolVersion{}}
	t.Source.URL = s.tools.srcURL
	t.Source.State = ToolSourceNever
	if s.tools.srcURL == "" {
		t.Source.State = ToolSourceOff
	}
	var offered int
	var check, success sql.NullInt64
	err := s.db.QueryRowContext(ctx, `SELECT offered, latest_version, last_check, last_success, last_error FROM tool_settings WHERE tool_id = ?`, d.ID).
		Scan(&offered, &t.LatestVersion, &check, &success, &t.Source.LastError)
	if err != nil && !errors.Is(err, sql.ErrNoRows) {
		return Tool{}, internal(err)
	}
	t.Offered = offered == 1
	if check.Valid {
		v := unixTime(check.Int64)
		t.Source.LastCheck = &v
		if s.tools.srcURL != "" {
			t.Source.State = ToolSourceOK
			if t.Source.LastError != "" {
				t.Source.State = ToolSourceError
			}
		}
	}
	if success.Valid {
		v := unixTime(success.Int64)
		t.Source.LastSuccess = &v
	}
	rows, err := s.queryToolRows(ctx, d.ID, "")
	if err != nil {
		return Tool{}, err
	}
	usage, err := s.toolUsageAll(ctx, d.ID)
	if err != nil {
		return Tool{}, err
	}
	byVer := map[string]*ToolVersion{}
	for _, r := range rows {
		v := byVer[r.version]
		if v == nil {
			v = &ToolVersion{Version: r.version, UsedBy: usage[r.version], Complete: true}
			byVer[r.version] = v
		}
		v.Platforms = append(v.Platforms, ToolPlatform{Platform: r.platform, Size: r.size, SHA256: r.sha, Cached: r.cached,
			Origin: r.origin, Root: r.root, URL: r.url})
		if r.cached {
			v.Installed = true
		} else {
			v.Complete = false
		}
	}
	for _, v := range byVer {
		sort.Slice(v.Platforms, func(i, j int) bool { return v.Platforms[i].Platform < v.Platforms[j].Platform })
		t.Versions = append(t.Versions, *v)
	}
	sort.Slice(t.Versions, func(i, j int) bool { return compareVersions(t.Versions[i].Version, t.Versions[j].Version) > 0 })
	return t, nil
}

// queryToolRows reads the platform rows of a tool (one version when version != ""). cached is true only when the
// file is really there with the recorded size (a restored backup has no tools directory).
func (s *Service) queryToolRows(ctx context.Context, toolID, version string) ([]toolRow, error) {
	q := `SELECT version, platform, url, size, sha256, zip_root, cached_at, origin FROM tool_versions WHERE tool_id = ?`
	args := []any{toolID}
	if version != "" {
		q += ` AND version = ?`
		args = append(args, version)
	}
	rows, err := s.db.QueryContext(ctx, q, args...)
	if err != nil {
		return nil, internal(err)
	}
	defer rows.Close()
	var out []toolRow
	for rows.Next() {
		var r toolRow
		var cachedAt sql.NullInt64
		if err := rows.Scan(&r.version, &r.platform, &r.url, &r.size, &r.sha, &r.root, &cachedAt, &r.origin); err != nil {
			return nil, internal(err)
		}
		if cachedAt.Valid {
			if st, err := os.Stat(s.toolFile(toolID, r.version, r.platform)); err == nil && st.Mode().IsRegular() && st.Size() == r.size {
				r.cached = true
			}
		}
		out = append(out, r)
	}
	return out, rows.Err()
}

func (s *Service) toolUsageAll(ctx context.Context, toolID string) (map[string]ToolUsage, error) {
	out := map[string]ToolUsage{}
	rows, err := s.db.QueryContext(ctx, `SELECT tool_version, COUNT(*) FROM game_variants WHERE tool_id = ? AND deleted_at IS NULL GROUP BY tool_version`, toolID)
	if err != nil {
		return nil, internal(err)
	}
	for rows.Next() {
		var v string
		var n int
		if err := rows.Scan(&v, &n); err != nil {
			rows.Close()
			return nil, internal(err)
		}
		u := out[v]
		u.Variants = n
		out[v] = u
	}
	rows.Close()
	rows, err = s.db.QueryContext(ctx, `SELECT rr.tool_version, COUNT(*) FROM run_randomizer rr JOIN runs r ON r.id = rr.run_id
		WHERE rr.tool_id = ? AND r.status NOT IN ('finished','abandoned') GROUP BY rr.tool_version`, toolID)
	if err != nil {
		return nil, internal(err)
	}
	defer rows.Close()
	for rows.Next() {
		var v string
		var n int
		if err := rows.Scan(&v, &n); err != nil {
			return nil, internal(err)
		}
		u := out[v]
		u.Runs = n
		out[v] = u
	}
	return out, rows.Err()
}

// toolVersionUsage counts what pins one version.
func (s *Service) toolVersionUsage(ctx context.Context, toolID, version string) (ToolUsage, error) {
	all, err := s.toolUsageAll(ctx, toolID)
	return all[version], err
}

// ToolVersionKnown reports whether the catalog has the version (offered or not, cached or not). Variants may pin a
// version that is no longer offered, so old recipes stay valid.
func (s *Service) ToolVersionKnown(ctx context.Context, toolID, version string) (bool, error) {
	var n int
	err := s.db.QueryRowContext(ctx, `SELECT COUNT(*) FROM tool_versions WHERE tool_id = ? AND version = ?`, toolID, version).Scan(&n)
	return n > 0, internalOrNil(err)
}

// ToolVersionOffered reports whether apps can get the version now: the tool is offered and a platform zip is cached.
func (s *Service) ToolVersionOffered(ctx context.Context, toolID, version string) (bool, error) {
	if _, ok := findToolDef(toolID); !ok || !toolVersionRe.MatchString(version) {
		return false, nil
	}
	var offered int
	err := s.db.QueryRowContext(ctx, `SELECT offered FROM tool_settings WHERE tool_id = ?`, toolID).Scan(&offered)
	if errors.Is(err, sql.ErrNoRows) || (err == nil && offered != 1) {
		return false, nil
	}
	if err != nil {
		return false, internal(err)
	}
	rows, err := s.queryToolRows(ctx, toolID, version)
	if err != nil {
		return false, err
	}
	for _, r := range rows {
		if r.cached {
			return true, nil
		}
	}
	return false, nil
}

func internalOrNil(err error) error {
	if err == nil {
		return nil
	}
	return internal(err)
}

// SetToolOffered switches whether apps can see and download the tool. The default is off: the admin decides.
func (s *Service) SetToolOffered(ctx context.Context, id string, offered bool) (_ Tool, err error) {
	defer s.publishOK(&err, TopicSystems)
	if _, ok := findToolDef(id); !ok {
		return Tool{}, ErrNotFound
	}
	v := 0
	if offered {
		v = 1
	}
	if _, err := s.db.ExecContext(ctx, `INSERT INTO tool_settings(tool_id, offered) VALUES (?, ?)
		ON CONFLICT(tool_id) DO UPDATE SET offered = excluded.offered`, id, v); err != nil {
		return Tool{}, internal(err)
	}
	return s.GetTool(ctx, id)
}

// OpenToolFile opens a cached platform zip for an app: the tool must be offered and the zip cached (ErrNotFound
// otherwise). The caller closes the file.
func (s *Service) OpenToolFile(ctx context.Context, id, version, platform string) (*os.File, ToolPlatform, error) {
	if _, ok := findToolDef(id); !ok || !toolVersionRe.MatchString(version) || !validToolPlatform(platform) {
		return nil, ToolPlatform{}, ErrNotFound
	}
	var offered int
	err := s.db.QueryRowContext(ctx, `SELECT offered FROM tool_settings WHERE tool_id = ?`, id).Scan(&offered)
	if errors.Is(err, sql.ErrNoRows) || (err == nil && offered != 1) {
		return nil, ToolPlatform{}, ErrNotFound
	}
	if err != nil {
		return nil, ToolPlatform{}, internal(err)
	}
	rows, err := s.queryToolRows(ctx, id, version)
	if err != nil {
		return nil, ToolPlatform{}, err
	}
	for _, r := range rows {
		if r.platform != platform || !r.cached {
			continue
		}
		f, err := os.Open(s.toolFile(id, version, platform))
		if err != nil {
			return nil, ToolPlatform{}, ErrNotFound
		}
		return f, ToolPlatform{Platform: platform, Size: r.size, SHA256: r.sha, Cached: true, Origin: r.origin, Root: r.root}, nil
	}
	return nil, ToolPlatform{}, ErrNotFound
}

// ---- remove ----

// RemoveToolVersion deletes a version with its cached files. Refused with tool_in_use (409, UsedBy set) while a
// variant that is not deleted or a run that is not finished or abandoned pins it.
func (s *Service) RemoveToolVersion(ctx context.Context, id, version string) (err error) {
	defer s.publishOK(&err, TopicSystems)
	if _, ok := findToolDef(id); !ok || !toolVersionRe.MatchString(version) {
		return ErrNotFound
	}
	s.tools.mu.Lock()
	defer s.tools.mu.Unlock()
	s.tools.usageMu.Lock()
	defer s.tools.usageMu.Unlock()
	known, err := s.ToolVersionKnown(ctx, id, version)
	if err != nil {
		return err
	}
	if !known {
		return ErrNotFound
	}
	use, err := s.toolVersionUsage(ctx, id, version)
	if err != nil {
		return err
	}
	if use.Variants+use.Runs > 0 {
		return &Error{Code: CodeToolInUse, Message: "A variant or a run still uses this version", UsedBy: &use}
	}
	if _, err := s.db.ExecContext(ctx, `DELETE FROM tool_versions WHERE tool_id = ? AND version = ?`, id, version); err != nil {
		return internal(err)
	}
	if dir := s.toolDir(id, version); dir != "" {
		os.RemoveAll(dir)
		os.Remove(filepath.Dir(dir)) // only succeeds when empty
	}
	return nil
}

// ---- zip inspection ----

// inspectToolZip checks that a platform zip is the randomizer engine: UPR-FVX.jar and java/bin/java or
// java/bin/java.exe either at the zip root or below exactly one top folder (the release zips differ). It returns that
// folder with a trailing slash ("" = root). Nothing is extracted; entry names with ".." or absolute paths are refused.
func inspectToolZip(p string) (string, error) {
	zr, err := zip.OpenReader(p)
	if err != nil {
		return "", fmt.Errorf("not a zip file: %w", err)
	}
	defer zr.Close()
	if len(zr.File) > maxToolZipEntries {
		return "", errors.New("the zip has too many entries")
	}
	names := map[string]bool{}
	for _, f := range zr.File {
		n := f.Name
		if strings.ContainsRune(n, '\\') || strings.HasPrefix(n, "/") {
			return "", errors.New("the zip has an unsafe entry name")
		}
		for _, seg := range strings.Split(n, "/") {
			if seg == ".." {
				return "", errors.New("the zip has an unsafe entry name")
			}
		}
		names[n] = true
	}
	var roots []string
	for n := range names {
		if n == "UPR-FVX.jar" {
			roots = append(roots, "")
		} else if d, f := path.Split(n); f == "UPR-FVX.jar" && strings.Count(d, "/") == 1 {
			roots = append(roots, d)
		}
	}
	sort.Strings(roots)
	for _, r := range roots {
		if names[r+"java/bin/java"] || names[r+"java/bin/java.exe"] {
			return r, nil
		}
	}
	if len(roots) == 0 {
		return "", errors.New("UPR-FVX.jar is missing in the zip")
	}
	return "", errors.New("java/bin/java (or java/bin/java.exe) is missing in the zip")
}

// ---- store ----

// storeToolFile moves a verified temp zip into the cache and records it. A pinned hash that differs is refused.
// The caller holds tools.mu.
func (s *Service) storeToolFile(ctx context.Context, id, version, platform, tmp, sum string, size int64, origin, url string) error {
	root, err := inspectToolZip(tmp)
	if err != nil {
		return badRequest("This is not the randomizer engine: %v", err)
	}
	var pinned string
	err = s.db.QueryRowContext(ctx, `SELECT sha256 FROM tool_versions WHERE tool_id = ? AND version = ? AND platform = ?`, id, version, platform).Scan(&pinned)
	if err != nil && !errors.Is(err, sql.ErrNoRows) {
		return internal(err)
	}
	if pinned != "" && pinned != sum {
		return &Error{Code: CodeToolHashMismatch, Message: fmt.Sprintf("%s %s for %s has another SHA-256 than the one pinned at the first download; refused", id, version, platform)}
	}
	dst := s.toolFile(id, version, platform)
	if dst == "" {
		return ErrNotFound
	}
	if err := os.MkdirAll(filepath.Dir(dst), 0o750); err != nil {
		return internal(err)
	}
	if err := os.Rename(tmp, dst); err != nil {
		return internal(err)
	}
	now := s.now().Unix()
	if _, err := s.db.ExecContext(ctx, `INSERT INTO tool_versions(tool_id, version, platform, url, size, sha256, zip_root, cached_at, origin, added_at)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
		ON CONFLICT(tool_id, version, platform) DO UPDATE SET size = excluded.size, sha256 = excluded.sha256, zip_root = excluded.zip_root,
			cached_at = excluded.cached_at, origin = excluded.origin, url = CASE WHEN excluded.url <> '' THEN excluded.url ELSE url END`,
		id, version, platform, url, size, sum, root, now, origin, now); err != nil {
		os.Remove(dst)
		return internal(err)
	}
	return nil
}

func (s *Service) toolTempFile() (*os.File, error) {
	dir := filepath.Join(s.dataDir, "tmp")
	if err := os.MkdirAll(dir, 0o750); err != nil {
		return nil, internal(err)
	}
	f, err := os.CreateTemp(dir, "tool-*.zip")
	if err != nil {
		return nil, internal(err)
	}
	return f, nil
}

// ---- install ----

// InstallToolVersion downloads every platform zip of a version that is not cached yet (https only, size limit),
// pins the SHA-256 at the first download and stores the zips. Download problems are plain errors (shown to the admin);
// a hash that differs from the pinned one is tool_hash_mismatch and the file is not stored. Platforms that are
// cached already are skipped, so a failed install can be repeated.
func (s *Service) InstallToolVersion(ctx context.Context, id, version string) (_ Tool, err error) {
	defer s.publishOK(&err, TopicSystems)
	if _, ok := findToolDef(id); !ok || !toolVersionRe.MatchString(version) {
		return Tool{}, ErrNotFound
	}
	s.tools.mu.Lock()
	defer s.tools.mu.Unlock()
	rows, err := s.queryToolRows(ctx, id, version)
	if err != nil {
		return Tool{}, err
	}
	if len(rows) == 0 {
		return Tool{}, ErrNotFound
	}
	for _, r := range rows {
		if r.cached {
			continue
		}
		if r.url == "" {
			return Tool{}, badRequest("%s %s for %s has no download URL; import the file instead", id, version, r.platform)
		}
		if err := s.downloadTool(ctx, id, version, r); err != nil {
			return Tool{}, err
		}
	}
	return s.GetTool(ctx, id)
}

func (s *Service) downloadTool(ctx context.Context, id, version string, r toolRow) error {
	if err := checkHTTPS(r.url); err != nil {
		return badRequest("The download URL must be https")
	}
	ctx, cancel := context.WithTimeout(ctx, toolFileTimeout)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, r.url, nil)
	if err != nil {
		return badRequest("Invalid download URL")
	}
	resp, err := s.cores.client.Do(req)
	if err != nil {
		return fmt.Errorf("download of %s failed: %w", r.platform, err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("download of %s failed: HTTP %d", r.platform, resp.StatusCode)
	}
	f, err := s.toolTempFile()
	if err != nil {
		return err
	}
	tmp := f.Name()
	defer os.Remove(tmp)
	h := sha256.New()
	n, err := io.Copy(io.MultiWriter(f, h), io.LimitReader(resp.Body, s.tools.maxZip+1))
	if cerr := f.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		return fmt.Errorf("download of %s failed: %w", r.platform, err)
	}
	if n > s.tools.maxZip {
		return fmt.Errorf("download of %s is larger than %d MiB", r.platform, s.tools.maxZip>>20)
	}
	sum := hex.EncodeToString(h.Sum(nil))
	return s.storeToolFile(ctx, id, version, r.platform, tmp, sum, n, ToolOriginGitHub, r.url)
}

// ---- import ----

// ToolImport is the result for one imported file.
type ToolImport struct {
	File     string
	ToolID   string
	Version  string
	Platform string
	Status   string // added, unchanged or rejected
	Err      string
}

// ParseToolFileName reads the version and platform from a release zip name such as UPR_FVX-v1_6_2-Linux_x86.zip.
func ParseToolFileName(name string) (toolID, version, platform string, ok bool) {
	m := toolAssetRe.FindStringSubmatch(filepath.Base(name))
	if m == nil {
		return "", "", "", false
	}
	return ToolRandomizer, m[1] + "." + m[2] + "." + m[3], toolAssetPlatform[m[4]], true
}

// ImportToolFiles adds release zips to the cache offline (version and platform from the file name, content
// checked like a download, SHA-256 pinned). The offered switch is not touched. One result per file.
func (s *Service) ImportToolFiles(ctx context.Context, files []string) []ToolImport {
	s.tools.mu.Lock()
	defer s.tools.mu.Unlock()
	var out []ToolImport
	changed := false
	for _, file := range files {
		r := ToolImport{File: file}
		id, version, platform, ok := ParseToolFileName(file)
		if !ok {
			r.Status, r.Err = "rejected", "the file name is not UPR_FVX-v<x>_<y>_<z>-<Windows|Linux_x86|Linux_ARM>.zip"
			out = append(out, r)
			continue
		}
		r.ToolID, r.Version, r.Platform = id, version, platform
		status, err := s.importToolFile(ctx, file, id, version, platform)
		if err != nil {
			r.Status, r.Err = "rejected", err.Error()
		} else {
			r.Status = status
			changed = changed || status == "added"
		}
		out = append(out, r)
	}
	if changed {
		s.Publish(TopicSystems)
	}
	return out
}

func (s *Service) importToolFile(ctx context.Context, file, id, version, platform string) (string, error) {
	src, err := os.Open(file)
	if err != nil {
		return "", errors.New("the file cannot be read")
	}
	defer src.Close()
	if st, err := src.Stat(); err != nil || !st.Mode().IsRegular() {
		return "", errors.New("not a regular file")
	}
	f, err := s.toolTempFile()
	if err != nil {
		return "", err
	}
	tmp := f.Name()
	defer os.Remove(tmp)
	h := sha256.New()
	n, err := io.Copy(io.MultiWriter(f, h), io.LimitReader(src, s.tools.maxZip+1))
	if cerr := f.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		return "", errors.New("the file cannot be read")
	}
	if n > s.tools.maxZip {
		return "", fmt.Errorf("the file is larger than %d MiB", s.tools.maxZip>>20)
	}
	sum := hex.EncodeToString(h.Sum(nil))
	rows, err := s.queryToolRows(ctx, id, version)
	if err != nil {
		return "", err
	}
	for _, r := range rows {
		if r.platform == platform && r.cached && r.sha == sum {
			return "unchanged", nil
		}
	}
	if err := s.storeToolFile(ctx, id, version, platform, tmp, sum, n, ToolOriginImport, ""); err != nil {
		var he *Error
		if errors.As(err, &he) {
			return "", errors.New(he.Message)
		}
		return "", err
	}
	return "added", nil
}
