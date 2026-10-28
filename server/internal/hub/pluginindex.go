package hub

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"net/http"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/phabioo/uzume/server/internal/corepkg"
	"github.com/phabioo/uzume/server/internal/pluginpkg"
	"github.com/phabioo/uzume/server/internal/updates"
)

// Settings keys of the plugin index state.
const (
	settingPluginIndexData      = "plugin_index"            // JSON of storedPluginIndex
	settingPluginIndexLastCheck = "plugin_index_last_check" // Unix seconds
	settingPluginIndexLastError = "plugin_index_last_error"
)

// PluginIndexInterval is how often the signed plugin index is refreshed.
const PluginIndexInterval = 24 * time.Hour

// ErrPluginIndexOff: no plugin index URL is configured.
var ErrPluginIndexOff = errors.New("no plugin index is configured")

type pluginIndexState struct {
	mu     sync.Mutex // serializes refreshes
	client *http.Client
	url    string
}

func (p *pluginIndexState) init(o Options) {
	p.client = o.CoreHTTPClient
	p.url = strings.TrimSpace(o.PluginIndexURL)
}

type storedPluginIndex struct {
	GeneratedAt time.Time              `json:"generated_at"`
	Plugins     []pluginpkg.IndexEntry `json:"plugins"`
}

func (s *Service) pluginSource() updates.Source {
	return updates.Source{IndexURL: s.plug.idx.url, Client: s.plug.idx.client}
}

// RefreshPluginIndex fetches the signed plugin index, verifies the detached signature with the trusted keys and stores
// the entries. A failure keeps the previous entries, is recorded for the admin page and returned. Never fatal.
func (s *Service) RefreshPluginIndex(ctx context.Context) error {
	if s.plug.idx.url == "" {
		return ErrPluginIndexOff
	}
	s.plug.idx.mu.Lock()
	defer s.plug.idx.mu.Unlock()
	err := s.fetchPluginIndex(ctx)
	if serr := s.setSetting(ctx, settingPluginIndexLastCheck, strconv.FormatInt(s.now().Unix(), 10)); serr != nil && err == nil {
		err = serr
	}
	msg := ""
	if err != nil {
		msg = err.Error()
	}
	if serr := s.setSetting(ctx, settingPluginIndexLastError, msg); serr != nil && err == nil {
		err = serr
	}
	return err
}

func (s *Service) fetchPluginIndex(ctx context.Context) error {
	src := s.pluginSource()
	data, sig, err := src.FetchSigned(ctx, src.IndexURL, pluginpkg.MaxIndexBytes)
	if err != nil {
		return fmt.Errorf("plugin index: %w", err)
	}
	if err := corepkg.VerifyContext(corepkg.ContextPluginIndex, data, sig, s.cores.keys, true); err != nil {
		return fmt.Errorf("plugin index signature rejected: %w", err)
	}
	idx, errs := pluginpkg.ParseIndex(data, src.IsFile())
	if pluginpkg.Fatal(errs) {
		return fmt.Errorf("plugin index rejected: %w", errs[0])
	}
	if err := idx.CheckFresh(s.now()); err != nil {
		return err
	}
	b, err := json.Marshal(storedPluginIndex{GeneratedAt: idx.GeneratedAt, Plugins: idx.Plugins})
	if err != nil {
		return err
	}
	return s.setSetting(ctx, settingPluginIndexData, string(b))
}

// RunPluginIndexRefresh refreshes at once (start it with go), then every interval, until ctx ends. report (may be nil) receives each result; without a configured index it returns at once.
func (s *Service) RunPluginIndexRefresh(ctx context.Context, every time.Duration, report func(error)) {
	if s.plug.idx.url == "" {
		return
	}
	do := func() {
		if err := s.RefreshPluginIndex(ctx); report != nil && ctx.Err() == nil {
			report(err)
		}
	}
	do()
	for {
		t := time.NewTimer(every)
		select {
		case <-ctx.Done():
			t.Stop()
			return
		case <-t.C:
			do()
		}
	}
}

// Catalog states of a plugin of the index.
const (
	CatalogInstall   = "install"   // no version of the plugin is offered
	CatalogUpdate    = "update"    // an older version is offered
	CatalogInstalled = "installed" // the newest indexed version is offered
	CatalogReplace   = "replace"   // not offered, but it replaces an offered plugin id (manifest field replaces)
)

// PluginCatalogEntry is the newest indexed version of one plugin together with what this server offers of it.
type PluginCatalogEntry struct {
	pluginpkg.IndexEntry
	State            string
	InstalledVersion string // the newest offered version, empty for CatalogInstall
	// ReplacesID, ReplacesName and ReplacesVersion describe the offered plugin a CatalogReplace entry takes over
	// (the first offered id of Replaces; newest offered version and its name). Empty for other states.
	ReplacesID, ReplacesName, ReplacesVersion string
}

// PluginIndexStatus is shown on the Plugins page.
type PluginIndexStatus struct {
	Configured bool
	URL        string
	LastCheck  *time.Time
	LastError  string
	Catalog    []PluginCatalogEntry
}

// PluginIndexStatus returns the stored index state.
func (s *Service) PluginIndexStatus(ctx context.Context) (PluginIndexStatus, error) {
	st := PluginIndexStatus{Configured: s.plug.idx.url != "", URL: s.plug.idx.url}
	if !st.Configured {
		return st, nil
	}
	if v, ok, err := s.getSetting(ctx, settingPluginIndexLastCheck); err != nil {
		return st, err
	} else if n, perr := strconv.ParseInt(v, 10, 64); ok && perr == nil {
		t := time.Unix(n, 0).UTC()
		st.LastCheck = &t
	}
	var err error
	if st.LastError, _, err = s.getSetting(ctx, settingPluginIndexLastError); err != nil {
		return st, err
	}
	idx, err := s.storedPluginIndex(ctx)
	if err != nil {
		return st, err
	}
	offered, err := s.ListPlugins(ctx)
	if err != nil {
		return st, err
	}
	newestOffered := map[string]string{}
	offeredName := map[string]string{}
	for _, p := range offered { // ListPlugins is oldest first
		newestOffered[p.PluginID] = p.Version
		offeredName[p.PluginID] = p.Name
	}
	newest := map[string]pluginpkg.IndexEntry{}
	for _, e := range idx.Plugins {
		if cur, ok := newest[e.ID]; !ok || semverLess(cur.Version, e.Version) {
			newest[e.ID] = e
		}
	}
	for _, e := range newest {
		c := PluginCatalogEntry{IndexEntry: e, State: CatalogInstall}
		if have, ok := newestOffered[e.ID]; ok {
			c.InstalledVersion = have
			c.State = CatalogInstalled
			if semverLess(have, e.Version) {
				c.State = CatalogUpdate
			}
		} else {
			for _, old := range e.Replaces {
				if v, ok := newestOffered[old]; ok {
					c.State, c.ReplacesID, c.ReplacesName, c.ReplacesVersion = CatalogReplace, old, offeredName[old], v
					break
				}
			}
		}
		st.Catalog = append(st.Catalog, c)
	}
	sortCatalog(st.Catalog)
	return st, nil
}

func sortCatalog(c []PluginCatalogEntry) {
	sort.Slice(c, func(i, j int) bool {
		if c[i].Name != c[j].Name {
			return c[i].Name < c[j].Name
		}
		return c[i].ID < c[j].ID
	})
}

func (s *Service) storedPluginIndex(ctx context.Context) (storedPluginIndex, error) {
	var idx storedPluginIndex
	v, ok, err := s.getSetting(ctx, settingPluginIndexData)
	if err != nil || !ok || v == "" {
		return idx, err
	}
	if json.Unmarshal([]byte(v), &idx) != nil {
		return storedPluginIndex{}, nil
	}
	return idx, nil
}

// InstallIndexPlugin downloads one indexed version, checks size and SHA-256 against the signed index and the
// embedded signature against the trusted keys, and offers it (source index, origin official). A file that is not
// signed by a trusted key is refused: an index entry promises an official plugin.
func (s *Service) InstallIndexPlugin(ctx context.Context, id, version string) (Plugin, error) {
	if s.plug.idx.url == "" {
		return Plugin{}, ErrPluginIndexOff
	}
	idx, err := s.storedPluginIndex(ctx)
	if err != nil {
		return Plugin{}, err
	}
	var e *pluginpkg.IndexEntry
	for i := range idx.Plugins {
		if idx.Plugins[i].ID == id && idx.Plugins[i].Version == version {
			e = &idx.Plugins[i]
		}
	}
	if e == nil {
		return Plugin{}, pluginErrNotFound()
	}
	src := s.pluginSource()
	var buf bytes.Buffer
	sum, err := src.Download(ctx, e.URL, e.Size, src.IsFile(), &buf)
	if err != nil {
		return Plugin{}, fmt.Errorf("download %s %s: %w", id, version, err)
	}
	if sum != e.SHA256 {
		return Plugin{}, fmt.Errorf("download %s %s: SHA-256 does not match the plugin index", id, version)
	}
	p, err := pluginpkg.Open(buf.Bytes())
	if err != nil {
		return Plugin{}, fmt.Errorf("download %s %s: %w", id, version, err)
	}
	if p.Manifest.ID != e.ID || p.Manifest.Version != e.Version {
		return Plugin{}, fmt.Errorf("download %s %s: the file contains %s %s", id, version, p.Manifest.ID, p.Manifest.Version)
	}
	if v := s.pluginVerdict(p); !v.Official() {
		reason := "it is not signed"
		if v.State == pluginpkg.SigInvalid {
			reason = "the signature is invalid: " + v.Reason
		}
		return Plugin{}, fmt.Errorf("download %s %s refused: %s", id, version, reason)
	}
	pl, _, err := s.storePlugin(ctx, p, PluginSourceIndex, "")
	if err != nil {
		return pl, err
	}
	s.replaceOldPlugins(ctx, e.ID, e.Replaces)
	return pl, nil
}

// replaceOldPlugins takes over the plugins newID replaces (issue #384): for every replaced id that is offered, the
// per-user store rows are copied to newID for users who have none for newID yet, then the old versions are removed.
// The server keeps no per-user "on" flag or permission grant (those live in the app); the store is the only per-user
// plugin data. Versions an unfinished run pins stay (RemovePlugin refuses them) and are pruned later. The new plugin is
// already stored, so a failure here only leaves the old id offered; it is logged and never fails the install.
func (s *Service) replaceOldPlugins(ctx context.Context, newID string, replaces []string) {
	for _, old := range replaces {
		offered, err := s.ListPlugins(ctx)
		if err != nil {
			slog.Warn("plugin replace: listing plugins failed", "plugin", newID, "replaces", old, "err", err)
			return
		}
		var vers []string
		for _, p := range offered {
			if p.PluginID == old {
				vers = append(vers, p.Version)
			}
		}
		if len(vers) == 0 {
			continue
		}
		moved, err := s.carryOverPluginStore(ctx, old, newID)
		if err != nil {
			slog.Warn("plugin replace: carrying over people's data failed, the old plugin stays", "plugin", newID, "replaces", old, "err", err)
			continue
		}
		removed, kept := 0, 0
		for _, v := range vers {
			if err := s.RemovePlugin(ctx, old, v); err != nil {
				kept++
				continue
			}
			removed++
		}
		slog.Info("plugin replaced", "plugin", newID, "replaces", old, "removed_versions", removed, "kept_versions", kept, "store_rows_carried", moved)
	}
}

// carryOverPluginStore copies the plugin_store rows of oldID to newID for every user who has no row for newID yet,
// in one transaction, and returns how many rows were copied. The old rows stay (a pinned old version may still use them).
func (s *Service) carryOverPluginStore(ctx context.Context, oldID, newID string) (int64, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return 0, err
	}
	defer tx.Rollback()
	res, err := tx.ExecContext(ctx, `INSERT INTO plugin_store(user_id, plugin_id, key, value, size_bytes, revision, updated_at)
		SELECT user_id, ?, key, value, size_bytes, revision, updated_at FROM plugin_store o
		WHERE o.plugin_id = ? AND NOT EXISTS (SELECT 1 FROM plugin_store n WHERE n.user_id = o.user_id AND n.plugin_id = ?)`,
		newID, oldID, newID)
	if err != nil {
		return 0, err
	}
	n, _ := res.RowsAffected()
	return n, tx.Commit()
}
