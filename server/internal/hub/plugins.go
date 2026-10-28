package hub

import (
	"context"
	"crypto/rand"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"sync"
	"time"

	"github.com/phabioo/uzume/server/internal/pluginpkg"
	"github.com/phabioo/uzume/server/internal/updates"
)

// FeaturePluginsV1 is the handshake feature of the plugin list and download endpoints.
const FeaturePluginsV1 = "plugins_v1"

// Plugin origins and sources as stored (columns origin and source of the plugins table).
const (
	PluginOriginOfficial   = "official"   // valid signature of a trusted Uzume key
	PluginOriginUnverified = "unverified" // unsigned (or not verifiable), offered by the admin
	PluginSourceIndex      = "index"
	PluginSourceUpload     = "upload"
	PluginSourceImport     = "import"
)

// pluginStageTTL is how long an uploaded file waits for the admin's confirmation.
const pluginStageTTL = time.Hour

var stageIDRe = regexp.MustCompile(`^[0-9a-f]{32}$`)

// ErrPluginUploadTooLarge: the uploaded file exceeds the limit of a .uzplugin file.
var ErrPluginUploadTooLarge = &Error{Code: CodePayloadTooLarge, Message: "Plugin file exceeds 32 MiB"}

type pluginState struct {
	mu sync.Mutex // serializes store changes (add, remove, import)
	// inUse reports whether a run that is not finished or abandoned uses a plugin version; see SetPluginInUseCheck.
	inUse func(ctx context.Context, pluginID, version string) (bool, error)
	idx   pluginIndexState
	// strMu guards strings: run_settings texts per file SHA-256, read lazily from the stored file (no database column).
	strMu   sync.Mutex
	strings map[string]map[string]map[string]string
}

func (s *Service) initPlugins(o Options) {
	s.plug.idx.init(o)
	s.plug.inUse = func(context.Context, string, string) (bool, error) { return false, nil }
	s.migrateLegacyPluginFiles()
}

// legacyPluginExt is the former file extension; stored files with it are renamed at start, imports still accept it.
const legacyPluginExt = ".fbplugin"

// pluginExt is the extension of a plugin package file.
const pluginExt = ".uzplugin"

// migrateLegacyPluginFiles renames <dataDir>/plugins/<id>/<version>.fbplugin to .uzplugin when the new name does not
// exist yet. Idempotent; only valid id and version names map to paths (same rule as pluginPath); errors are skipped
// (the file stays readable through the next start).
func (s *Service) migrateLegacyPluginFiles() {
	base := filepath.Join(s.dataDir, "plugins")
	dirs, err := os.ReadDir(base)
	if err != nil {
		return
	}
	for _, d := range dirs {
		if !d.IsDir() || !pluginpkg.ValidID(d.Name()) {
			continue
		}
		files, err := os.ReadDir(filepath.Join(base, d.Name()))
		if err != nil {
			continue
		}
		for _, f := range files {
			version, ok := strings.CutSuffix(f.Name(), legacyPluginExt)
			if !ok || f.IsDir() {
				continue
			}
			oldP, err := s.pluginPathExt(d.Name(), version, legacyPluginExt)
			if err != nil {
				continue
			}
			newP, err := s.pluginPathExt(d.Name(), version, pluginExt)
			if err != nil {
				continue
			}
			if _, err := os.Lstat(newP); err == nil {
				continue
			}
			os.Rename(oldP, newP)
		}
	}
}

// SetPluginInUseCheck installs the check behind PluginVersionInUse. The runs part calls it once at startup.
func (s *Service) SetPluginInUseCheck(f func(ctx context.Context, pluginID, version string) (bool, error)) {
	s.plug.mu.Lock()
	defer s.plug.mu.Unlock()
	s.plug.inUse = f
}

// PluginVersionInUse reports whether a run that is not `finished` or `abandoned` uses this plugin version. Removing
// such a version is refused (409). Runs part fills this: until it calls SetPluginInUseCheck the answer is always false.
func (s *Service) PluginVersionInUse(ctx context.Context, pluginID, version string) (bool, error) {
	s.plug.mu.Lock()
	f := s.plug.inUse
	s.plug.mu.Unlock()
	return f(ctx, pluginID, version)
}

// Plugin is a plugin version offered to apps.
type Plugin struct {
	PluginID, Version, Name, Description string
	API                                  int
	// Manifest is parsed from ManifestRaw (empty when the stored text no longer passes the current rules).
	Manifest    pluginpkg.Manifest
	ManifestRaw string
	// RunSettingsStrings is language -> i18n key -> text for the keys the manifest's run_settings reference (nil without run_settings).
	RunSettingsStrings map[string]map[string]string
	SHA256             string
	Size               int64
	Origin             string // official or unverified
	Source             string // index, upload or import
	// UploadedBy is the user id (empty after the user was deleted or for index/import), UploadedByName the display name.
	UploadedBy, UploadedByName string
	CreatedAt                  time.Time
}

// Permissions returns the permissions the plugin asks for (never nil).
func (p Plugin) Permissions() []string {
	if p.Manifest.Permissions == nil {
		return []string{}
	}
	return p.Manifest.Permissions
}

const pluginCols = `p.plugin_id, p.version, p.name, p.description, p.api, p.manifest, p.sha256, p.size, p.origin, p.source,
	COALESCE(p.uploaded_by, ''), COALESCE(u.display_name, ''), p.created_at`

func scanPlugin(sc interface{ Scan(...any) error }) (Plugin, error) {
	var p Plugin
	var created int64
	if err := sc.Scan(&p.PluginID, &p.Version, &p.Name, &p.Description, &p.API, &p.ManifestRaw, &p.SHA256, &p.Size, &p.Origin, &p.Source,
		&p.UploadedBy, &p.UploadedByName, &created); err != nil {
		return p, err
	}
	p.CreatedAt = time.Unix(created, 0).UTC()
	p.Manifest, _ = pluginpkg.ParseManifest([]byte(p.ManifestRaw))
	return p, nil
}

func pluginErrNotFound() error { return &Error{Code: CodeNotFound, Message: "Plugin not found"} }

// ListPlugins returns every offered version, ordered by plugin id and SemVer (oldest first).
func (s *Service) ListPlugins(ctx context.Context) ([]Plugin, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT `+pluginCols+` FROM plugins p LEFT JOIN users u ON u.id = p.uploaded_by`)
	if err != nil {
		return nil, internal(err)
	}
	defer rows.Close()
	var out []Plugin
	for rows.Next() {
		p, err := scanPlugin(rows)
		if err != nil {
			return nil, internal(err)
		}
		out = append(out, p)
	}
	if err := rows.Err(); err != nil {
		return nil, internal(err)
	}
	for i := range out {
		out[i].RunSettingsStrings = s.runSettingsStrings(out[i])
	}
	sort.SliceStable(out, func(i, j int) bool {
		if out[i].PluginID != out[j].PluginID {
			return out[i].PluginID < out[j].PluginID
		}
		return semverLess(out[i].Version, out[j].Version)
	})
	return out, nil
}

// runSettingsStrings returns the translated run_settings texts of a plugin version, read from the stored file and
// cached in memory by SHA-256. A missing or unreadable file yields empty texts (not cached).
func (s *Service) runSettingsStrings(p Plugin) map[string]map[string]string {
	if len(p.Manifest.RunSettings) == 0 {
		return nil
	}
	s.plug.strMu.Lock()
	c, ok := s.plug.strings[p.SHA256]
	s.plug.strMu.Unlock()
	if ok {
		return c
	}
	path, err := s.pluginPath(p.PluginID, p.Version)
	if err != nil {
		return map[string]map[string]string{}
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return map[string]map[string]string{}
	}
	pkg, err := pluginpkg.Open(data)
	if err != nil || pluginpkg.SHA256Hex(data) != p.SHA256 {
		return map[string]map[string]string{}
	}
	c = pkg.RunSettingsStrings()
	if c == nil {
		c = map[string]map[string]string{}
	}
	s.plug.strMu.Lock()
	if s.plug.strings == nil {
		s.plug.strings = map[string]map[string]map[string]string{}
	}
	s.plug.strings[p.SHA256] = c
	s.plug.strMu.Unlock()
	return c
}

func semverLess(a, b string) bool {
	va, errA := updates.ParseSemVer(a)
	vb, errB := updates.ParseSemVer(b)
	if errA != nil || errB != nil {
		return a < b
	}
	if c := va.Compare(vb); c != 0 {
		return c < 0
	}
	return a < b
}

// GetPlugin returns one offered version (ErrNotFound when unknown).
func (s *Service) GetPlugin(ctx context.Context, id, version string) (Plugin, error) {
	p, err := scanPlugin(s.db.QueryRowContext(ctx, `SELECT `+pluginCols+` FROM plugins p LEFT JOIN users u ON u.id = p.uploaded_by
		WHERE p.plugin_id = ? AND p.version = ?`, id, version))
	if errors.Is(err, sql.ErrNoRows) {
		return p, pluginErrNotFound()
	}
	if err != nil {
		return p, internal(err)
	}
	return p, nil
}

// pluginPath builds the on-disk path of an offered version and refuses anything that is not a valid id and version
// or that would leave <dataDir>/plugins.
func (s *Service) pluginPath(id, version string) (string, error) {
	return s.pluginPathExt(id, version, pluginExt)
}

// pluginPathExt is pluginPath with an explicit extension (pluginExt or legacyPluginExt).
func (s *Service) pluginPathExt(id, version, ext string) (string, error) {
	if !safePluginRef(id, version) {
		return "", errors.New("invalid plugin reference")
	}
	base := filepath.Join(s.dataDir, "plugins")
	p := filepath.Clean(filepath.Join(base, id, version+ext))
	if !strings.HasPrefix(p, base+string(filepath.Separator)) {
		return "", errors.New("plugin path escapes the plugin directory")
	}
	return p, nil
}

// safePluginRef guards the file system path: only ids and versions that pass the manifest rules map to files.
func safePluginRef(id, version string) bool {
	return pluginpkg.ValidID(id) && updates.ValidSemVer(version)
}

// OpenPluginFile opens the stored .uzplugin file of an offered version; the caller closes it.
func (s *Service) OpenPluginFile(ctx context.Context, id, version string) (*os.File, Plugin, error) {
	if !safePluginRef(id, version) {
		return nil, Plugin{}, pluginErrNotFound()
	}
	p, err := s.GetPlugin(ctx, id, version)
	if err != nil {
		return nil, p, err
	}
	path, err := s.pluginPath(id, version)
	if err != nil {
		return nil, p, pluginErrNotFound()
	}
	f, err := os.Open(path)
	if errors.Is(err, os.ErrNotExist) {
		return nil, p, pluginErrNotFound()
	}
	if err != nil {
		return nil, p, internal(err)
	}
	return f, p, nil
}

// pluginVerdict checks the embedded signature against the trusted keys.
func (s *Service) pluginVerdict(p *pluginpkg.Package) pluginpkg.Verdict {
	return p.Verify(s.cores.keys)
}

// storePlugin writes the file and the row. A repeated (id, version) with the same SHA-256 is a no-op (created is
// false); a different SHA-256 is refused (409). origin comes from the signature check.
func (s *Service) storePlugin(ctx context.Context, p *pluginpkg.Package, source, uploadedBy string) (_ Plugin, created bool, err error) {
	s.plug.mu.Lock()
	defer s.plug.mu.Unlock()
	m := p.Manifest
	sum := pluginpkg.SHA256Hex(p.Data())
	if old, err := s.GetPlugin(ctx, m.ID, m.Version); err == nil {
		if old.SHA256 == sum {
			return old, false, nil
		}
		return Plugin{}, false, conflict("A different file with this version is already offered. Remove it first or change the version.")
	} else if !errors.Is(err, ErrNotFound) {
		return Plugin{}, false, err
	}
	origin := PluginOriginUnverified
	if s.pluginVerdict(p).Official() {
		origin = PluginOriginOfficial
	}
	path, err := s.pluginPath(m.ID, m.Version)
	if err != nil {
		return Plugin{}, false, badRequest("The plugin id or version is not valid.")
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o750); err != nil {
		return Plugin{}, false, internal(err)
	}
	if err := writeFileAtomic(path, p.Data()); err != nil {
		return Plugin{}, false, internal(err)
	}
	desc := strings.TrimSpace(m.Description)
	if r := []rune(desc); len(r) > 1000 {
		desc = string(r[:1000])
	}
	var by any
	if uploadedBy != "" {
		by = uploadedBy
	}
	_, err = s.db.ExecContext(ctx, `INSERT INTO plugins(plugin_id, version, name, description, api, manifest, sha256, size, origin, source, uploaded_by, created_at)
		VALUES (?,?,?,?,?,?,?,?,?,?,?,?)`, m.ID, m.Version, m.Name, desc, m.API, string(p.RawManifest), sum, len(p.Data()), origin, source, by, s.now().Unix())
	if err != nil {
		os.Remove(path)
		return Plugin{}, false, internal(err)
	}
	pl, err := s.GetPlugin(ctx, m.ID, m.Version)
	if err == nil {
		// A newer version supersedes the older ones (those a run still uses stay until it ends). A deliberately
		// uploaded older version is not the newest and removes nothing.
		s.prunePluginLocked(ctx, m.ID, true, m.Version)
	}
	return pl, true, err
}

// prunePluginLocked removes the offered versions of a plugin id that are older than the newest offered version and
// that no unfinished run uses. With onlyIfNewest set it does nothing unless version is the newest. Callers hold
// plug.mu. Errors are ignored: pruning is housekeeping and retried at the next trigger.
func (s *Service) prunePluginLocked(ctx context.Context, id string, onlyIfNewest bool, version string) {
	rows, err := s.db.QueryContext(ctx, `SELECT version FROM plugins WHERE plugin_id = ?`, id)
	if err != nil {
		return
	}
	var vers []string
	for rows.Next() {
		var v string
		if rows.Scan(&v) == nil {
			vers = append(vers, v)
		}
	}
	rows.Close()
	newest := ""
	for _, v := range vers {
		if newest == "" || semverLess(newest, v) {
			newest = v
		}
	}
	if newest == "" || (onlyIfNewest && newest != version) {
		return
	}
	for _, v := range vers {
		if v == newest || !semverLess(v, newest) {
			continue
		}
		if used, err := s.plug.inUse(ctx, id, v); err != nil || used {
			continue
		}
		s.removePluginLocked(ctx, id, v)
	}
}

// removePluginLocked deletes row and file of an offered version. Callers hold plug.mu.
func (s *Service) removePluginLocked(ctx context.Context, id, version string) error {
	if _, err := s.db.ExecContext(ctx, `DELETE FROM plugins WHERE plugin_id = ? AND version = ?`, id, version); err != nil {
		return internal(err)
	}
	if path, err := s.pluginPath(id, version); err == nil {
		os.Remove(path)
		if legacy, err := s.pluginPathExt(id, version, legacyPluginExt); err == nil {
			os.Remove(legacy)
		}
		os.Remove(filepath.Dir(path)) // only when empty
	}
	return nil
}

// PruneOldPlugins removes every offered plugin version that is older than the newest offered version of its id and
// not used by an unfinished run. It runs at server start and after a run ends or is deleted.
func (s *Service) PruneOldPlugins(ctx context.Context) {
	s.plug.mu.Lock()
	defer s.plug.mu.Unlock()
	rows, err := s.db.QueryContext(ctx, `SELECT DISTINCT plugin_id FROM plugins`)
	if err != nil {
		return
	}
	var ids []string
	for rows.Next() {
		var id string
		if rows.Scan(&id) == nil {
			ids = append(ids, id)
		}
	}
	rows.Close()
	replaced := s.replacedPluginIDsLocked(ctx)
	for _, id := range ids {
		if replaced[id] {
			s.removeAllPluginVersionsLocked(ctx, id)
			continue
		}
		s.prunePluginLocked(ctx, id, false, "")
	}
}

// replacedPluginIDsLocked returns the ids that an offered plugin replaces (manifest field replaces). Callers hold plug.mu.
func (s *Service) replacedPluginIDsLocked(ctx context.Context) map[string]bool {
	out := map[string]bool{}
	rows, err := s.db.QueryContext(ctx, `SELECT plugin_id, manifest FROM plugins`)
	if err != nil {
		return out
	}
	defer rows.Close()
	for rows.Next() {
		var id, mf string
		if rows.Scan(&id, &mf) != nil {
			continue
		}
		var m struct {
			Replaces []string `json:"replaces"`
		}
		if json.Unmarshal([]byte(mf), &m) != nil {
			continue
		}
		for _, r := range m.Replaces {
			if r != id {
				out[r] = true
			}
		}
	}
	return out
}

// removeAllPluginVersionsLocked removes every offered version of a replaced id that no unfinished run pins.
func (s *Service) removeAllPluginVersionsLocked(ctx context.Context, id string) {
	rows, err := s.db.QueryContext(ctx, `SELECT version FROM plugins WHERE plugin_id = ?`, id)
	if err != nil {
		return
	}
	var vers []string
	for rows.Next() {
		var v string
		if rows.Scan(&v) == nil {
			vers = append(vers, v)
		}
	}
	rows.Close()
	for _, v := range vers {
		if used, err := s.plug.inUse(ctx, id, v); err == nil && !used {
			s.removePluginLocked(ctx, id, v)
		}
	}
}

// RemovePlugin deletes an offered version (row and file). Apps keep their copy. Refused with a conflict while a
// run that is not finished or abandoned uses the version (PluginVersionInUse).
func (s *Service) RemovePlugin(ctx context.Context, id, version string) error {
	if !safePluginRef(id, version) {
		return pluginErrNotFound()
	}
	// Check and removal under one lock: CreateRun pins a version while holding plug.mu, so a run cannot start on
	// the version between the "in use" check and the delete.
	s.plug.mu.Lock()
	defer s.plug.mu.Unlock()
	if _, err := s.GetPlugin(ctx, id, version); err != nil {
		return err
	}
	if used, err := s.plug.inUse(ctx, id, version); err != nil {
		return err
	} else if used {
		return conflict("A run that is still going uses this plugin version. Finish or abandon the run first.")
	}
	return s.removePluginLocked(ctx, id, version)
}

// ---- Admin upload: stage, review, offer ----

// PluginStaged is an uploaded file that passed validation and waits for the admin's confirmation.
type PluginStaged struct {
	StageID                              string
	PluginID, Name, Version, Description string
	Permissions                          []string
	Official                             bool
	// Signed: SIGNATURE.json is present; SignatureProblem is set when it is present but does not verify.
	Signed           bool
	SignatureProblem string
	SHA256           string
	Size             int64
	// AlreadyOffered: the same file is already offered; Conflict: another file with this version is.
	AlreadyOffered bool
	Conflict       bool
}

func (s *Service) stageDir() string { return filepath.Join(s.dataDir, "tmp", "plugin-stage") }

// StagePluginUpload reads an uploaded .uzplugin (at most 32 MiB), validates it and keeps it for the confirmation
// step. Nothing is offered yet.
func (s *Service) StagePluginUpload(ctx context.Context, r io.Reader) (PluginStaged, error) {
	data, err := io.ReadAll(io.LimitReader(r, pluginpkg.MaxCompressedBytes+1))
	if err != nil {
		return PluginStaged{}, internal(err)
	}
	if len(data) > pluginpkg.MaxCompressedBytes {
		return PluginStaged{}, ErrPluginUploadTooLarge
	}
	p, err := pluginpkg.Open(data)
	if err != nil {
		return PluginStaged{}, badRequest("%s", strings.TrimPrefix(err.Error(), pluginpkg.ErrInvalid.Error()+": "))
	}
	if err := os.MkdirAll(s.stageDir(), 0o750); err != nil {
		return PluginStaged{}, internal(err)
	}
	s.pruneStaged()
	var raw [16]byte
	if _, err := rand.Read(raw[:]); err != nil {
		return PluginStaged{}, internal(err)
	}
	id := hex.EncodeToString(raw[:])
	if err := writeFileAtomic(filepath.Join(s.stageDir(), id+pluginExt), data); err != nil {
		return PluginStaged{}, internal(err)
	}
	st := s.describeStaged(ctx, id, p)
	return st, nil
}

func (s *Service) describeStaged(ctx context.Context, stageID string, p *pluginpkg.Package) PluginStaged {
	v := s.pluginVerdict(p)
	m := p.Manifest
	st := PluginStaged{StageID: stageID, PluginID: m.ID, Name: m.Name, Version: m.Version, Description: strings.TrimSpace(m.Description),
		Permissions: append([]string{}, m.Permissions...), Official: v.Official(), Signed: p.HasSignature(), SHA256: pluginpkg.SHA256Hex(p.Data()),
		Size: int64(len(p.Data()))}
	if v.State == pluginpkg.SigInvalid {
		st.SignatureProblem = v.Reason
	}
	if old, err := s.GetPlugin(ctx, m.ID, m.Version); err == nil {
		st.AlreadyOffered = old.SHA256 == st.SHA256
		st.Conflict = !st.AlreadyOffered
	}
	return st
}

func (s *Service) pruneStaged() {
	ents, err := os.ReadDir(s.stageDir())
	if err != nil {
		return
	}
	for _, e := range ents {
		if info, err := e.Info(); err == nil && s.now().Sub(info.ModTime()) > pluginStageTTL {
			os.Remove(filepath.Join(s.stageDir(), e.Name()))
		}
	}
}

func (s *Service) readStaged(stageID string) (*pluginpkg.Package, error) {
	if !stageIDRe.MatchString(stageID) {
		return nil, badRequest("The upload is no longer available. Please upload the file again.")
	}
	data, err := os.ReadFile(filepath.Join(s.stageDir(), stageID+pluginExt))
	if err != nil {
		return nil, badRequest("The upload is no longer available. Please upload the file again.")
	}
	p, err := pluginpkg.Open(data)
	if err != nil {
		return nil, badRequest("The upload is not a valid plugin.")
	}
	return p, nil
}

// StagedPlugin describes a staged upload again (for the confirmation page).
func (s *Service) StagedPlugin(ctx context.Context, stageID string) (PluginStaged, error) {
	p, err := s.readStaged(stageID)
	if err != nil {
		return PluginStaged{}, err
	}
	return s.describeStaged(ctx, stageID, p), nil
}

// DiscardStagedPlugin deletes a staged upload.
func (s *Service) DiscardStagedPlugin(stageID string) {
	if stageIDRe.MatchString(stageID) {
		os.Remove(filepath.Join(s.stageDir(), stageID+pluginExt))
	}
}

// OfferStagedPlugin offers a staged upload to apps. Every permission the plugin asks for must be in ticked
// (the admin reviewed it); otherwise nothing happens. The origin is official only for a valid Uzume signature,
// else unverified. The same (id, version) with a different file is refused (409).
func (s *Service) OfferStagedPlugin(ctx context.Context, stageID string, ticked []string, adminID string) (_ Plugin, err error) {
	p, err := s.readStaged(stageID)
	if err != nil {
		return Plugin{}, err
	}
	have := map[string]bool{}
	for _, t := range ticked {
		have[t] = true
	}
	for _, perm := range p.Manifest.Permissions {
		if !have[perm] {
			return Plugin{}, badRequest("Tick every permission to confirm you reviewed it.")
		}
	}
	pl, _, err := s.storePlugin(ctx, p, PluginSourceUpload, adminID)
	if err != nil {
		return Plugin{}, err
	}
	s.DiscardStagedPlugin(stageID)
	return pl, nil
}

// ---- Offline import ----

// PluginImportSummary is the result of ImportPlugins.
type PluginImportSummary struct {
	Added, Unchanged int
	Warnings         []string
	Problems         []string
}

// isPluginFileName reports a *.uzplugin or legacy *.fbplugin name (case-insensitive).
func isPluginFileName(name string) bool {
	ext := filepath.Ext(name)
	return strings.EqualFold(ext, pluginExt) || strings.EqualFold(ext, legacyPluginExt)
}

// ImportPlugins offers every *.uzplugin file (legacy *.fbplugin too) in dir (not recursive; offline servers, like import-cores). The same checks
// as for an upload apply, except the permission ticks: running the command is the admin's confirmation. A file
// without a valid Uzume signature is imported as unverified.
func (s *Service) ImportPlugins(ctx context.Context, dir string) (PluginImportSummary, error) {
	var sum PluginImportSummary
	ents, err := os.ReadDir(dir)
	if err != nil {
		return sum, err
	}
	for _, e := range ents {
		if e.IsDir() || !isPluginFileName(e.Name()) {
			continue
		}
		full := filepath.Join(dir, e.Name())
		st, err := os.Lstat(full)
		if err != nil || !st.Mode().IsRegular() {
			sum.Problems = append(sum.Problems, e.Name()+": not a regular file")
			continue
		}
		if st.Size() > pluginpkg.MaxCompressedBytes {
			sum.Problems = append(sum.Problems, e.Name()+": file is larger than 32 MiB")
			continue
		}
		data, err := os.ReadFile(full)
		if err != nil {
			sum.Problems = append(sum.Problems, fmt.Sprintf("%s: %v", e.Name(), err))
			continue
		}
		p, err := pluginpkg.Open(data)
		if err != nil {
			sum.Problems = append(sum.Problems, fmt.Sprintf("%s: %v", e.Name(), strings.TrimPrefix(err.Error(), pluginpkg.ErrInvalid.Error()+": ")))
			continue
		}
		if v := s.pluginVerdict(p); v.State == pluginpkg.SigInvalid {
			sum.Warnings = append(sum.Warnings, fmt.Sprintf("%s: signature invalid (%s), imported as unverified", e.Name(), v.Reason))
		}
		_, created, err := s.storePlugin(ctx, p, PluginSourceImport, "")
		switch {
		case err != nil:
			sum.Problems = append(sum.Problems, fmt.Sprintf("%s: %v", e.Name(), errMessage(err)))
		case created:
			sum.Added++
		default:
			sum.Unchanged++
		}
	}
	return sum, nil
}

func errMessage(err error) string {
	var he *Error
	if errors.As(err, &he) {
		return he.Message
	}
	return err.Error()
}
