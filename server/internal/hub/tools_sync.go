package hub

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"regexp"
	"strings"
	"time"
)

// ErrToolSourceOff: no tools source is configured (-tools-source-url none).
var ErrToolSourceOff = &Error{Code: CodeBadRequest, Message: "No tools source is configured"}

// toolRelease is one release of the engine as the sync reads it from the GitHub releases list.
type toolRelease struct {
	Version string
	Assets  map[string]toolAsset // platform -> asset
}

type toolAsset struct {
	URL  string
	Size int64
}

// toolTagRe matches the release tags of the randomizer engine: v1.6.2 and the real vFVX1.6.2 (prefix case-insensitive).
var toolTagRe = regexp.MustCompile(`^(?i:v?(?:fvx)?)(\d{1,4}\.\d{1,4}\.\d{1,4})$`)

// toolTagVersion returns the plain version (1.6.2) of a release tag, or "" when the tag is not a release tag. Only the
// stored/displayed version is derived here; downloads use the asset URLs of the release list, never a rebuilt tag.
func toolTagVersion(tag string) string {
	m := toolTagRe.FindStringSubmatch(strings.TrimSpace(tag))
	if m == nil {
		return ""
	}
	return m[1]
}

// parseToolReleases reads the GitHub releases JSON (an array of releases with tag_name, draft, prerelease and assets
// with name, browser_download_url and size). Drafts and prereleases are skipped, as are assets whose name does not
// follow UPR_FVX-v<x>_<y>_<z>-<Windows|Linux_x86|Linux_ARM>.zip, whose URL is not https or whose version differs
// from the release tag. A release without a usable asset is left out. Newest version first.
func parseToolReleases(data []byte) ([]toolRelease, error) {
	var raw []struct {
		TagName    string `json:"tag_name"`
		Draft      bool   `json:"draft"`
		Prerelease bool   `json:"prerelease"`
		Assets     []struct {
			Name string `json:"name"`
			URL  string `json:"browser_download_url"`
			Size int64  `json:"size"`
		} `json:"assets"`
	}
	if err := json.Unmarshal(data, &raw); err != nil {
		return nil, fmt.Errorf("the release list is not valid JSON: %w", err)
	}
	var out []toolRelease
	seen := map[string]bool{}
	for _, r := range raw {
		if r.Draft || r.Prerelease {
			continue
		}
		version := toolTagVersion(r.TagName)
		if version == "" || seen[version] {
			continue
		}
		rel := toolRelease{Version: version, Assets: map[string]toolAsset{}}
		for _, a := range r.Assets {
			m := toolAssetRe.FindStringSubmatch(a.Name)
			if m == nil || m[1]+"."+m[2]+"."+m[3] != version || checkHTTPS(a.URL) != nil || a.Size < 0 {
				continue
			}
			rel.Assets[toolAssetPlatform[m[4]]] = toolAsset{URL: a.URL, Size: a.Size}
		}
		if len(rel.Assets) == 0 {
			continue
		}
		seen[version] = true
		out = append(out, rel)
	}
	for i := 1; i < len(out); i++ { // newest first (insertion sort, the list is short)
		for j := i; j > 0 && compareVersions(out[j].Version, out[j-1].Version) > 0; j-- {
			out[j], out[j-1] = out[j-1], out[j]
		}
	}
	return out, nil
}

func (s *Service) fetchToolList(ctx context.Context) ([]byte, error) {
	if err := checkHTTPS(s.tools.srcURL); err != nil {
		return nil, errors.New("the tools source URL must be https")
	}
	ctx, cancel := context.WithTimeout(ctx, coreFetchTimeout)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, s.tools.srcURL, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("Accept", "application/vnd.github+json")
	req.Header.Set("User-Agent", "uzume-server")
	resp, err := s.cores.client.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("HTTP %d", resp.StatusCode)
	}
	b, err := io.ReadAll(io.LimitReader(resp.Body, toolListMaxBytes+1))
	if err != nil {
		return nil, err
	}
	if len(b) > toolListMaxBytes {
		return nil, errors.New("the release list is too large")
	}
	return b, nil
}

// SyncTool reads the release list of the tool's source and records the versions and platforms it offers
// (nothing is downloaded; Install does that) and the newest version as latest_version. A failed check is stored as
// the source's last_error and returned as a plain error; ErrToolSourceOff when no source is configured.
func (s *Service) SyncTool(ctx context.Context, id string) (_ Tool, err error) {
	defer s.publishOK(&err, TopicSystems)
	if _, ok := findToolDef(id); !ok {
		return Tool{}, ErrNotFound
	}
	if s.tools.srcURL == "" {
		return Tool{}, ErrToolSourceOff
	}
	s.tools.mu.Lock()
	defer s.tools.mu.Unlock()
	now := s.now().Unix()
	rels, ferr := func() ([]toolRelease, error) {
		b, err := s.fetchToolList(ctx)
		if err != nil {
			return nil, err
		}
		return parseToolReleases(b)
	}()
	if ferr != nil {
		msg := ferr.Error()
		if len(msg) > 300 {
			msg = msg[:300]
		}
		if _, err := s.db.ExecContext(ctx, `INSERT INTO tool_settings(tool_id, last_check, last_error) VALUES (?, ?, ?)
			ON CONFLICT(tool_id) DO UPDATE SET last_check = excluded.last_check, last_error = excluded.last_error`, id, now, msg); err != nil {
			return Tool{}, internal(err)
		}
		return Tool{}, fmt.Errorf("the release list could not be read: %w", ferr)
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return Tool{}, internal(err)
	}
	defer tx.Rollback()
	latest := ""
	for _, r := range rels {
		if latest == "" || compareVersions(r.Version, latest) > 0 {
			latest = r.Version
		}
		for platform, a := range r.Assets {
			if _, err := tx.ExecContext(ctx, `INSERT INTO tool_versions(tool_id, version, platform, url, size, origin, added_at)
				VALUES (?, ?, ?, ?, ?, 'github', ?)
				ON CONFLICT(tool_id, version, platform) DO UPDATE SET url = excluded.url,
					size = CASE WHEN sha256 = '' THEN excluded.size ELSE size END`, id, r.Version, platform, a.URL, a.Size, now); err != nil {
				return Tool{}, internal(err)
			}
		}
	}
	if _, err := tx.ExecContext(ctx, `INSERT INTO tool_settings(tool_id, latest_version, last_check, last_success, last_error) VALUES (?, ?, ?, ?, '')
		ON CONFLICT(tool_id) DO UPDATE SET latest_version = excluded.latest_version, last_check = excluded.last_check,
			last_success = excluded.last_success, last_error = ''`, id, latest, now, now); err != nil {
		return Tool{}, internal(err)
	}
	if err := tx.Commit(); err != nil {
		return Tool{}, internal(err)
	}
	return s.GetTool(ctx, id)
}

// TriggerToolSync asks the background loop (RunToolSync) for a refresh now; it never blocks.
func (s *Service) TriggerToolSync() {
	select {
	case s.tools.kickSync <- struct{}{}:
	default:
	}
}

// RunToolSync refreshes the release lists at once (in the caller's goroutine, so start it with go), then every
// `every` and on TriggerToolSync, until ctx ends. report (may be nil) receives each failure. It never fails the caller.
// Without a source it only waits for ctx.
func (s *Service) RunToolSync(ctx context.Context, every time.Duration, report func(toolID string, err error)) {
	if s.tools.srcURL == "" {
		<-ctx.Done()
		return
	}
	t := time.NewTicker(every)
	defer t.Stop()
	do := func() {
		for _, d := range toolDefs {
			if _, err := s.SyncTool(ctx, d.ID); err != nil && report != nil && ctx.Err() == nil {
				report(d.ID, err)
			}
		}
	}
	do()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			do()
		case <-s.tools.kickSync:
			do()
		}
	}
}
