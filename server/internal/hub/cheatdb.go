package hub

import (
	"archive/zip"
	"context"
	"crypto/sha1"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"hash/crc32"
	"io"
	"log/slog"
	"net/http"
	"net/url"
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

// Cheat database (0.20 package 3, feature cheat_db_v1): the server keeps the RetroArch cheat lists (libretro-database
// cht/) and the No-Intro DATs of the systems it serves under <data>/cheat-db/, downloads them at run time from a
// configurable URL (a zip of the libretro-database repository) and refreshes them in the background. Offline servers use
// `uzume-server import-cheats <zip>`. A game is matched by the hash of its stored ROM (SHA-1 or CRC32 in the DAT), for
// the 3DS by the title ID (libretro-database has no 3DS lists: Gateway lists `<TITLEID>.txt` come from a `3ds/` folder
// of an imported archive or from the optional second URL), and as a fallback by the file name or title. The lists are public data, not user data: they are
// not part of backups and can always be downloaded again.

// FeatureCheatDBV1 is the handshake feature flag for GET /games/{game_id}/cheats and the admin cheat database endpoints.
const FeatureCheatDBV1 = "cheat_db_v1"

// DefaultCheatDBURL is the libretro-database repository as a zip (GitHub redirects it to its archive host).
const DefaultCheatDBURL = "https://github.com/libretro/libretro-database/archive/refs/heads/master.zip"

// CheatDBOff is the URL value that turns the cheat database off.
const CheatDBOff = "none"

// Settings keys of the cheat database state.
const (
	settingCheatDBURL         = "cheat_db_url" // admin override of the flag; "none" = off
	settingCheatDB3DSURL      = "cheat_db_3ds_url"
	settingCheatDBLastCheck   = "cheat_db_last_check"
	settingCheatDBLastSuccess = "cheat_db_last_success"
	settingCheatDBLastError   = "cheat_db_last_error"
)

const (
	// CheatDBRefreshEvery is how old the lists get before the background loop downloads them again.
	CheatDBRefreshEvery = 7 * 24 * time.Hour
	// cheatDBRetryEvery is the wait after a failed refresh.
	cheatDBRetryEvery = 6 * time.Hour

	maxCheatDBZipBytes  = int64(1) << 30 // downloaded archive
	maxCheatListBytes   = 4 << 20
	maxCheatDatBytes    = 128 << 20
	maxCheatDBTotal     = int64(512) << 20 // extracted files
	maxCheatHashROM     = int64(512) << 20 // larger ROMs are not hashed
	cheatNameMaxLen     = 200
	cheatSourceDownload = "libretro-database"
	cheatSourceImport   = "imported"
)

// cheatSystemDirs maps the system ids of the server to the folder names of libretro-database (cht/ and dat/).
var cheatSystemDirs = map[string]string{
	"gb":  "Nintendo - Game Boy",
	"gbc": "Nintendo - Game Boy Color",
	"gba": "Nintendo - Game Boy Advance",
	"nds": "Nintendo - Nintendo DS",
}

var (
	// ErrCheatDBOff: the cheat database is switched off (URL "none").
	ErrCheatDBOff = errors.New("the cheat database is switched off")
	// ErrNoCheatLists: an archive without a single cheat list of a supported system.
	ErrNoCheatLists = errors.New("the archive holds no cheat lists of a supported system")
)

type cheatDBState struct {
	mu     sync.Mutex // serializes installs
	url    string     // from the options (flag); the admin setting wins
	url3   string     // 3DS source from the options
	kick   chan struct{}
	idxMu  sync.Mutex
	gen    int
	dats   map[string]*DatIndex         // system id -> DAT index
	names  map[string]map[string]string // system id -> lowercase list name -> file name
	loaded map[string]bool
}

func (c *cheatDBState) init(o Options) {
	c.url = strings.TrimSpace(o.CheatDBURL)
	c.url3 = strings.TrimSpace(o.CheatDB3DSURL)
	c.kick = make(chan struct{}, 1)
	c.reset()
}

func (c *cheatDBState) reset() {
	c.idxMu.Lock()
	c.gen++
	c.dats, c.names, c.loaded = map[string]*DatIndex{}, map[string]map[string]string{}, map[string]bool{}
	c.idxMu.Unlock()
}

func (s *Service) cheatDir() string { return filepath.Join(s.dataDir, "cheat-db") }

// cheat3DSDir holds the Gateway lists downloaded from the 3DS URL (<TITLEID>.txt); the 3ds/ folder of an imported
// archive lives in cheatDir.
func (s *Service) cheat3DSDir() string { return filepath.Join(s.dataDir, "cheat-db-3ds") }

type cheatMeta struct {
	Source    string    `json:"source"`
	URL       string    `json:"url,omitempty"`
	UpdatedAt time.Time `json:"updated_at"`
	SizeBytes int64     `json:"size_bytes"`
	Lists     int       `json:"lists"`
	Dats      int       `json:"dats"`
}

func (s *Service) cheatMeta() (cheatMeta, bool) { return s.cheatMetaAt(s.cheatDir()) }

func (s *Service) cheatMetaAt(dir string) (cheatMeta, bool) {
	b, err := os.ReadFile(filepath.Join(dir, "meta.json"))
	if err != nil {
		return cheatMeta{}, false
	}
	var m cheatMeta
	if json.Unmarshal(b, &m) != nil {
		return cheatMeta{}, false
	}
	return m, true
}

// CheatDBURL returns the effective source URL: the admin setting, else the flag, else the default. "none" = off.
func (s *Service) CheatDBURL(ctx context.Context) (string, error) {
	v, ok, err := s.getSetting(ctx, settingCheatDBURL)
	if err != nil {
		return "", err
	}
	if ok && strings.TrimSpace(v) != "" {
		return strings.TrimSpace(v), nil
	}
	if s.cheat.url != "" {
		return s.cheat.url, nil
	}
	return DefaultCheatDBURL, nil
}

// CheatDB3DSURL returns the effective 3DS source URL: the admin setting, else the flag, else "none".
func (s *Service) CheatDB3DSURL(ctx context.Context) (string, error) {
	v, ok, err := s.getSetting(ctx, settingCheatDB3DSURL)
	if err != nil {
		return "", err
	}
	if ok && strings.TrimSpace(v) != "" {
		return strings.TrimSpace(v), nil
	}
	if s.cheat.url3 != "" {
		return s.cheat.url3, nil
	}
	return CheatDBOff, nil
}

// SetCheatDB3DSURL saves the admin's 3DS source URL ("none" = off) and asks the background loop to refresh.
func (s *Service) SetCheatDB3DSURL(ctx context.Context, v string) error {
	v = strings.TrimSpace(v)
	if !ValidCheatDBURL(v) {
		return badRequest("The 3DS cheat URL must be https or \"none\"")
	}
	if err := s.setSetting(ctx, settingCheatDB3DSURL, v); err != nil {
		return err
	}
	if v != CheatDBOff {
		s.TriggerCheatDBRefresh()
	}
	return nil
}

// ValidCheatDBURL accepts "none" and https URLs.
func ValidCheatDBURL(v string) bool {
	if v == CheatDBOff {
		return true
	}
	u, err := url.Parse(v)
	return err == nil && u.Scheme == "https" && u.Host != ""
}

// SetCheatDBURL saves the admin's source URL ("none" turns the database off) and asks the background loop to refresh.
func (s *Service) SetCheatDBURL(ctx context.Context, v string) error {
	v = strings.TrimSpace(v)
	if !ValidCheatDBURL(v) {
		return badRequest("The cheat database URL must be https or \"none\"")
	}
	if err := s.setSetting(ctx, settingCheatDBURL, v); err != nil {
		return err
	}
	if v != CheatDBOff {
		s.TriggerCheatDBRefresh()
	}
	return nil
}

// CheatDBStatus is the state of the cheat database for the admin.
type CheatDBStatus struct {
	URL         string
	URL3DS      string
	Lists3DS    int // Gateway lists downloaded from URL3DS
	Enabled     bool
	Source      string // "", cheatSourceDownload or cheatSourceImport
	UpdatedAt   *time.Time
	SizeBytes   int64
	Lists, Dats int
	LastCheck   *time.Time
	LastSuccess *time.Time
	LastError   string
}

func (s *Service) settingTime(ctx context.Context, key string) (*time.Time, error) {
	v, ok, err := s.getSetting(ctx, key)
	if err != nil || !ok {
		return nil, err
	}
	n, perr := strconv.ParseInt(v, 10, 64)
	if perr != nil {
		return nil, nil
	}
	t := time.Unix(n, 0).UTC()
	return &t, nil
}

// CheatDBStatus returns the URL, the size and age of the stored lists and the result of the last refresh.
func (s *Service) CheatDBStatus(ctx context.Context) (CheatDBStatus, error) {
	var st CheatDBStatus
	var err error
	if st.URL, err = s.CheatDBURL(ctx); err != nil {
		return st, err
	}
	if st.URL3DS, err = s.CheatDB3DSURL(ctx); err != nil {
		return st, err
	}
	st.Enabled = st.URL != CheatDBOff || st.URL3DS != CheatDBOff
	if m, ok := s.cheatMetaAt(s.cheat3DSDir()); ok {
		st.Lists3DS = m.Lists
	}
	if m, ok := s.cheatMeta(); ok {
		t := m.UpdatedAt
		st.Source, st.UpdatedAt, st.SizeBytes, st.Lists, st.Dats = m.Source, &t, m.SizeBytes, m.Lists, m.Dats
	}
	if st.LastCheck, err = s.settingTime(ctx, settingCheatDBLastCheck); err != nil {
		return st, err
	}
	if st.LastSuccess, err = s.settingTime(ctx, settingCheatDBLastSuccess); err != nil {
		return st, err
	}
	st.LastError, _, err = s.getSetting(ctx, settingCheatDBLastError)
	return st, err
}

// TriggerCheatDBRefresh asks the background loop for a refresh now; it never blocks.
func (s *Service) TriggerCheatDBRefresh() {
	select {
	case s.cheat.kick <- struct{}{}:
	default:
	}
}

func (s *Service) recordCheatRefresh(ctx context.Context, err error) {
	now := strconv.FormatInt(s.now().Unix(), 10)
	_ = s.setSetting(ctx, settingCheatDBLastCheck, now)
	msg := ""
	if err != nil {
		msg = err.Error()
	} else {
		_ = s.setSetting(ctx, settingCheatDBLastSuccess, now)
	}
	_ = s.setSetting(ctx, settingCheatDBLastError, msg)
}

// RefreshCheatDB downloads the archive from the effective URL and installs it. A failure keeps the stored lists, is
// recorded for the admin page and returned. ErrCheatDBOff when the database is switched off.
func (s *Service) RefreshCheatDB(ctx context.Context) error {
	u, err := s.CheatDBURL(ctx)
	if err != nil {
		return err
	}
	u3, err := s.CheatDB3DSURL(ctx)
	if err != nil {
		return err
	}
	if u == CheatDBOff && u3 == CheatDBOff {
		return ErrCheatDBOff
	}
	var errs []error
	if u != CheatDBOff {
		errs = append(errs, s.downloadAndInstallCheats(ctx, u, cheatSourceDownload, false))
	}
	if u3 != CheatDBOff {
		if err := s.downloadAndInstallCheats(ctx, u3, cheatSourceDownload, true); err != nil {
			errs = append(errs, fmt.Errorf("3DS lists: %w", err))
		}
	}
	err = errors.Join(errs...)
	s.recordCheatRefresh(ctx, err)
	return err
}

func (s *Service) downloadAndInstallCheats(ctx context.Context, u, source string, three bool) error {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, u, nil)
	if err != nil {
		return err
	}
	resp, err := s.cores.client.Do(req)
	if err != nil {
		return fmt.Errorf("download: %w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("download: %s answered %s", req.URL.Host, resp.Status)
	}
	tmp, err := os.CreateTemp(filepath.Join(s.dataDir, "tmp"), "cheatdb-*.zip")
	if err != nil {
		return err
	}
	defer os.Remove(tmp.Name())
	n, err := io.Copy(tmp, io.LimitReader(resp.Body, maxCheatDBZipBytes+1))
	if cerr := tmp.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		return fmt.Errorf("download: %w", err)
	}
	if n > maxCheatDBZipBytes {
		return errors.New("download: the archive is larger than 1 GiB")
	}
	return s.installCheatZip(tmp.Name(), source, u, three)
}

// ImportCheats installs an archive from a local file (offline servers, `uzume-server import-cheats`). It works while the
// database is switched off, and is shown as the source "imported".
func (s *Service) ImportCheats(ctx context.Context, zipPath string) (err error) {
	err = s.installCheatZip(zipPath, cheatSourceImport, "", false)
	s.recordCheatRefresh(ctx, err)
	return err
}

var gatewayFileRe = regexp.MustCompile(`^[0-9A-Fa-f]{16}\.txt$`)

// cheatEntryTarget maps a zip entry to its place below cheat-db/ ("" = not wanted). The archive may carry a top folder
// (libretro-database-master/) or start with cht/ directly; a 3ds/ folder holds Gateway lists <TITLEID>.txt. With only3DS
// (the 3DS URL) every <TITLEID>.txt anywhere in the archive is taken and placed at the top.
func cheatEntryTarget(name string, only3DS bool) string {
	name = strings.ReplaceAll(name, "\\", "/")
	parts := strings.Split(name, "/")
	for _, p := range parts {
		if p == ".." || p == "." || strings.ContainsAny(p, "\x00") {
			return ""
		}
	}
	if only3DS {
		if base := parts[len(parts)-1]; gatewayFileRe.MatchString(base) {
			return strings.ToUpper(strings.TrimSuffix(base, ".txt")) + ".txt"
		}
		return ""
	}
	supported := map[string]bool{}
	for _, d := range cheatSystemDirs {
		supported[d] = true
	}
	for off := 0; off <= 1 && off < len(parts); off++ {
		p := parts[off:]
		base := p[len(p)-1]
		if base == "" || len(base) > cheatNameMaxLen {
			return ""
		}
		switch {
		case len(p) == 2 && p[0] == "3ds" && gatewayFileRe.MatchString(base):
			return "3ds/" + strings.ToUpper(strings.TrimSuffix(base, ".txt")) + ".txt"
		case len(p) == 3 && p[0] == "cht" && supported[p[1]] && strings.HasSuffix(strings.ToLower(base), ".cht"):
			return "cht/" + p[1] + "/" + base
		case len(p) == 2 && p[0] == "dat" && strings.HasSuffix(base, ".dat") && supported[strings.TrimSuffix(base, ".dat")]:
			return "dat/" + base
		case len(p) == 3 && p[0] == "metadat" && p[1] == "no-intro" && strings.HasSuffix(base, ".dat") && supported[strings.TrimSuffix(base, ".dat")]:
			return "nointro/" + base
		}
	}
	return ""
}

func (s *Service) installCheatZip(zipPath, source, srcURL string, three bool) error {
	target := s.cheatDir()
	if three {
		target = s.cheat3DSDir()
	}
	s.cheat.mu.Lock()
	defer s.cheat.mu.Unlock()
	zr, err := zip.OpenReader(zipPath)
	if err != nil {
		return fmt.Errorf("not a zip archive: %w", err)
	}
	defer zr.Close()
	newDir := target + ".new"
	if err := os.RemoveAll(newDir); err != nil {
		return err
	}
	if err := os.MkdirAll(newDir, 0o750); err != nil {
		return err
	}
	ok := false
	defer func() {
		if !ok {
			os.RemoveAll(newDir)
		}
	}()
	meta := cheatMeta{Source: source, URL: srcURL, UpdatedAt: s.now().UTC()}
	seen := map[string]bool{}
	for _, f := range zr.File {
		if f.FileInfo().IsDir() || !f.Mode().IsRegular() {
			continue
		}
		rel := cheatEntryTarget(f.Name, three)
		if rel == "" || seen[strings.ToLower(rel)] {
			continue
		}
		limit := int64(maxCheatListBytes)
		if strings.HasSuffix(rel, ".dat") {
			limit = maxCheatDatBytes
		}
		if int64(f.UncompressedSize64) > limit {
			continue
		}
		if meta.SizeBytes+int64(f.UncompressedSize64) > maxCheatDBTotal {
			return errors.New("the cheat lists are larger than 512 MiB")
		}
		seen[strings.ToLower(rel)] = true
		dst := filepath.Join(newDir, filepath.FromSlash(rel))
		// cheatEntryTarget already drops ".." and "." segments; this keeps every write inside newDir regardless.
		if !strings.HasPrefix(dst, filepath.Clean(newDir)+string(os.PathSeparator)) {
			continue
		}
		if err := os.MkdirAll(filepath.Dir(dst), 0o750); err != nil {
			return err
		}
		n, err := copyZipEntry(f, dst, limit)
		if err != nil {
			return fmt.Errorf("%s: %w", f.Name, err)
		}
		meta.SizeBytes += n
		if strings.HasSuffix(rel, ".dat") {
			meta.Dats++
		} else {
			meta.Lists++
		}
	}
	if meta.Lists == 0 {
		return ErrNoCheatLists
	}
	b, _ := json.Marshal(meta)
	if err := os.WriteFile(filepath.Join(newDir, "meta.json"), b, 0o640); err != nil {
		return err
	}
	cur, old := target, target+".old"
	if err := os.RemoveAll(old); err != nil {
		return err
	}
	if _, err := os.Stat(cur); err == nil {
		if err := os.Rename(cur, old); err != nil {
			return err
		}
	}
	if err := os.Rename(newDir, cur); err != nil {
		_ = os.Rename(old, cur) // put the previous lists back
		return err
	}
	ok = true
	os.RemoveAll(old)
	s.cheat.reset()
	return nil
}

func copyZipEntry(f *zip.File, dst string, limit int64) (int64, error) {
	rc, err := f.Open()
	if err != nil {
		return 0, err
	}
	defer rc.Close()
	out, err := os.OpenFile(dst, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o640)
	if err != nil {
		return 0, err
	}
	n, err := io.Copy(out, io.LimitReader(rc, limit+1))
	if cerr := out.Close(); err == nil {
		err = cerr
	}
	if err == nil && n > limit {
		err = errors.New("too large")
	}
	return n, err
}

// RunCheatDBRefresh keeps the lists fresh until ctx ends (start it with go): it refreshes when the stored lists are
// missing or older than CheatDBRefreshEvery (retrying after a failure every few hours), and at once on
// TriggerCheatDBRefresh. A server that has the lists does not download them again at every start. report may be nil.
func (s *Service) RunCheatDBRefresh(ctx context.Context, report func(error)) {
	t := time.NewTicker(time.Hour)
	defer t.Stop()
	do := func(force bool) {
		st, err := s.CheatDBStatus(ctx)
		if err != nil || !st.Enabled {
			return
		}
		if !force {
			now := s.now()
			fresh := func(on bool, m cheatMeta, have bool) bool {
				return !on || (have && now.Sub(m.UpdatedAt) < CheatDBRefreshEvery)
			}
			m1, h1 := s.cheatMeta()
			m2, h2 := s.cheatMetaAt(s.cheat3DSDir())
			if fresh(st.URL != CheatDBOff, m1, h1) && fresh(st.URL3DS != CheatDBOff, m2, h2) {
				return
			}
			if st.LastCheck != nil && st.LastError != "" && now.Sub(*st.LastCheck) < cheatDBRetryEvery {
				return
			}
		}
		if err := s.RefreshCheatDB(ctx); report != nil && ctx.Err() == nil {
			report(err)
		}
	}
	do(false)
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			do(false)
		case <-s.cheat.kick:
			do(true)
		}
	}
}

// ---- matching ----

// CheatList is the cheat list found for a game.
type CheatList struct {
	Source    string // "none" when the database is off; "unavailable" when it is on but holds no lists yet (not loaded or the update failed)
	UpdatedAt *time.Time
	MatchedBy string // "hash", "title_id", "name" or "" (nothing found)
	ListName  string // name of the list file without .cht
	// SimilarLists names lists with the same base title that belong to another region than the game. They are never
	// applied (cheat addresses differ between regions); the app shows them as a hint.
	SimilarLists []string
	Cheats       []Cheat
}

// cheatIndex returns the lowercase base names of the cht files of a system and the DAT indexes, loading them once per
// installed version.
func (s *Service) cheatIndex(system string) (map[string]string, *DatIndex) {
	c := &s.cheat
	c.idxMu.Lock()
	defer c.idxMu.Unlock()
	if c.loaded[system] {
		return c.names[system], c.dats[system]
	}
	dir := cheatSystemDirs[system]
	names := map[string]string{}
	if ents, err := os.ReadDir(filepath.Join(s.cheatDir(), "cht", dir)); err == nil {
		for _, e := range ents {
			if e.Type().IsRegular() && strings.HasSuffix(strings.ToLower(e.Name()), ".cht") {
				names[strings.ToLower(strings.TrimSuffix(e.Name(), filepath.Ext(e.Name())))] = e.Name()
			}
		}
	}
	idx := newDatIndex()
	for _, p := range []string{"dat", "nointro"} {
		if b, err := readLimitedFile(filepath.Join(s.cheatDir(), p, dir+".dat"), maxCheatDatBytes); err == nil {
			d := ParseDat(b)
			for k, v := range d.SHA1 {
				idx.SHA1[k] = v
			}
			for k, v := range d.CRC {
				idx.CRC[k] = v
			}
		}
	}
	c.names[system], c.dats[system], c.loaded[system] = names, idx, true
	return names, idx
}

// romHashes returns SHA-1 and CRC32 of the stored ROM of a game, computed once per ROM (cache table rom_hashes).
// ok is false for games without a stored ROM, for the 3DS and for very large files.
func (s *Service) romHashes(ctx context.Context, g Game) (sha1hex, crchex string, ok bool) {
	if !g.OnHub || g.System == "3ds" || g.ROMSize > maxCheatHashROM {
		return "", "", false
	}
	err := s.db.QueryRowContext(ctx, `SELECT sha1, crc32 FROM rom_hashes WHERE sha256 = ?`, g.ROMSHA256).Scan(&sha1hex, &crchex)
	if err == nil {
		return sha1hex, crchex, true
	}
	if !errors.Is(err, sql.ErrNoRows) {
		return "", "", false
	}
	f, err := os.Open(s.romPath(g.ROMSHA256))
	if err != nil {
		return "", "", false
	}
	defer f.Close()
	h, c := sha1.New(), crc32.NewIEEE()
	if _, err := io.Copy(io.MultiWriter(h, c), f); err != nil {
		return "", "", false
	}
	sha1hex, crchex = hex.EncodeToString(h.Sum(nil)), fmt.Sprintf("%08x", c.Sum32())
	_, _ = s.db.ExecContext(ctx, `INSERT OR REPLACE INTO rom_hashes(sha256, sha1, crc32) VALUES (?,?,?)`, g.ROMSHA256, sha1hex, crchex)
	return sha1hex, crchex, true
}

// GameCheats returns the cheat list of a game the caller can see. Without lists (database off, no list for the game)
// the answer is empty, never an error; ErrNotFound only for an unknown game.
func (s *Service) GameCheats(ctx context.Context, userID, gameID string) (CheatList, error) {
	out, err := s.gameCheats(ctx, userID, gameID)
	if err == nil && out.Source == "none" {
		// "none" is the database being off; switched on without data (not loaded yet, update failed) is a different answer.
		if st, serr := s.CheatDBStatus(ctx); serr == nil && st.Enabled {
			out.Source = "unavailable"
		}
	}
	return out, err
}

func (s *Service) gameCheats(ctx context.Context, userID, gameID string) (CheatList, error) {
	g, err := s.GetGameFor(ctx, userID, gameID)
	if err != nil {
		return CheatList{}, err
	}
	out := CheatList{Source: "none", Cheats: []Cheat{}}
	if g.System == "3ds" {
		return s.gameCheats3DS(g, out), nil
	}
	meta, have := s.cheatMeta()
	dir, supported := cheatSystemDirs[g.System]
	if !have || !supported {
		return out, nil
	}
	out.Source = meta.Source
	t := meta.UpdatedAt
	out.UpdatedAt = &t
	names, dat := s.cheatIndex(g.System)

	file, by := "", ""
	pick := func(n, how string) bool {
		if f, ok := names[strings.ToLower(n)]; ok && n != "" {
			file, by = f, how
			return true
		}
		return false
	}
	hashed := false
	if sum, crc, ok := s.romHashes(ctx, g); ok {
		hashed = true
		found := pick(dat.SHA1[sum], "hash")
		if !found {
			pick(dat.CRC[crc], "hash")
		}
	}
	if file == "" {
		if !pick(strings.TrimSuffix(g.Filename, path.Ext(g.Filename)), "name") {
			pick(g.Title, "name")
		}
	}
	// Same base title, region checked: "Game (Europe) (En,Fr)" finds "Game (Europe) (En,Fr,De)" but never the USA list.
	if file == "" {
		var similar []string
		file, similar = matchCheatBase(names, g)
		if file != "" {
			by = "name"
		}
		out.SimilarLists = similar
	}
	if file == "" {
		slog.Info("cheat database: no list for game", "game_id", g.ID, "system", g.System, "title", g.Title, "game_code", g.GameCode,
			"region", gameRegionLabel(g), "hashed", hashed, "other_region_lists", len(out.SimilarLists), "lists", len(names), "dat_hashes", len(dat.SHA1))
		return out, nil
	}
	slog.Info("cheat database: list matched", "game_id", g.ID, "system", g.System, "title", g.Title, "by", by, "list", file)
	b, err := readLimitedFile(filepath.Join(s.cheatDir(), "cht", dir, file), maxCheatListBytes)
	if err != nil {
		return out, nil // replaced while reading, or damaged: no list
	}
	out.MatchedBy, out.ListName = by, strings.TrimSuffix(file, filepath.Ext(file))
	for _, c := range ParseCht(b) {
		c.FormatGuess = GuessCheatFormat(g.System, c.Code)
		c.Code = NormalizeCheatCode(c.Code)
		out.Cheats = append(out.Cheats, c)
	}
	return out, nil
}

// gameCheats3DS looks up the Gateway list of a 3DS game by its title ID: first the lists of an imported archive (3ds/),
// then those of the 3DS URL.
func (s *Service) gameCheats3DS(g Game, out CheatList) CheatList {
	m1, have1 := s.cheatMeta()
	m2, have2 := s.cheatMetaAt(s.cheat3DSDir())
	src := m1
	if !have1 || (have2 && m2.UpdatedAt.After(m1.UpdatedAt)) {
		src = m2
	}
	if have1 || have2 {
		out.Source = src.Source
		t := src.UpdatedAt
		out.UpdatedAt = &t
	}
	if g.GameCodeKind != Game3DS || !gatewayFileRe.MatchString(g.GameCode+".txt") {
		return out
	}
	id := strings.ToUpper(g.GameCode)
	for _, p := range []string{filepath.Join(s.cheatDir(), "3ds", id+".txt"), filepath.Join(s.cheat3DSDir(), id+".txt")} {
		b, err := readLimitedFile(p, maxCheatListBytes)
		if err != nil {
			continue
		}
		if list := ParseGatewayList(b); len(list) > 0 {
			out.MatchedBy, out.ListName, out.Cheats = "title_id", id, list
			return out
		}
	}
	return out
}

var cheatTagRe = regexp.MustCompile(`\(([^()]*)\)`)

// cheatRegionNames are the region words of No-Intro names (lowercase).
var cheatRegionNames = map[string]bool{
	"usa": true, "europe": true, "japan": true, "world": true, "australia": true, "korea": true, "china": true, "taiwan": true,
	"asia": true, "germany": true, "france": true, "italy": true, "spain": true, "netherlands": true, "sweden": true,
	"denmark": true, "norway": true, "finland": true, "russia": true, "brazil": true, "canada": true, "hong kong": true,
}

// splitCheatName splits a No-Intro style name into its normalized base title (everything before the first
// parenthesis) and the regions named in its tags.
func splitCheatName(n string) (base string, regions []string, tags int) {
	b := n
	if i := strings.Index(n, "("); i >= 0 {
		b = n[:i]
	}
	base = strings.Join(strings.Fields(strings.ToLower(b)), " ")
	for _, m := range cheatTagRe.FindAllStringSubmatch(n, -1) {
		tags++
		for _, part := range strings.Split(m[1], ",") {
			if r := strings.ToLower(strings.TrimSpace(part)); cheatRegionNames[r] {
				regions = append(regions, r)
			}
		}
	}
	return base, regions, tags
}

var (
	cheatAnyTagRe = regexp.MustCompile(`[(\[]([^()\[\]]*)[)\]]`)
	cheatLangRe   = regexp.MustCompile(`(?i)^[a-z]{2}(?:-[a-z0-9]{2,4})?$`)
	cheatRevRe    = regexp.MustCompile(`(?i)^(?:rev\s*([0-9a-z]+)|v\s*(\d+(?:\.\d+)*))$`)
)

// cheatBuildKey returns the build qualifiers of a No-Intro style name: every tag part in parentheses or brackets
// except regions, language codes and the good-dump mark "[!]", such as "Rev 1", "Rev A", "v1.1", "Beta", "Proto", "Demo" or "Alt". The parts
// are normalized (lowercase, single spaces, "rev 1", "v1.1") and sorted. Empty means the retail base release. The
// second result tells whether the name carries any tag at all.
func cheatBuildKey(n string) (key string, tagged bool) {
	var parts []string
	for _, m := range cheatAnyTagRe.FindAllStringSubmatch(n, -1) {
		tagged = true
		for _, part := range strings.Split(m[1], ",") {
			p := strings.Join(strings.Fields(strings.ToLower(part)), " ")
			if p == "" || p == "!" || cheatRegionNames[p] || cheatLangRe.MatchString(p) {
				continue
			}
			if r := cheatRevRe.FindStringSubmatch(p); r != nil {
				if r[1] != "" {
					p = "rev " + r[1]
				} else {
					p = "v" + r[2]
				}
			}
			parts = append(parts, p)
		}
	}
	sort.Strings(parts)
	return strings.Join(parts, "|"), tagged
}

// gameBuildKey returns the build key of a game from its file name, else its title: the first of the two that
// carries tags.
func gameBuildKey(g Game) string {
	for _, n := range []string{strings.TrimSuffix(g.Filename, path.Ext(g.Filename)), g.Title} {
		if k, tagged := cheatBuildKey(n); tagged {
			return k
		}
	}
	return ""
}

// gameRegions returns the regions of a game: from the tags of its file name or title, else from the region letter of
// the NDS or GBA game code (4th character). Empty when unknown.
func gameRegions(g Game) []string {
	for _, n := range []string{strings.TrimSuffix(g.Filename, path.Ext(g.Filename)), g.Title} {
		if _, r, _ := splitCheatName(n); len(r) > 0 {
			return r
		}
	}
	if (g.GameCodeKind == GameCodeNDS || g.GameCodeKind == GameCodeGBA) && len(g.GameCode) == 4 {
		switch g.GameCode[3] {
		case 'E':
			return []string{"usa"}
		case 'P', 'D', 'F', 'I', 'S', 'H', 'X', 'Y', 'Z', 'V':
			return []string{"europe"}
		case 'J':
			return []string{"japan"}
		case 'K':
			return []string{"korea"}
		case 'U':
			return []string{"australia"}
		case 'C':
			return []string{"china"}
		case 'T':
			return []string{"taiwan"}
		case 'A':
			return []string{"asia"}
		}
	}
	return nil
}

func gameRegionLabel(g Game) string {
	if r := gameRegions(g); len(r) > 0 {
		return strings.Join(r, ",")
	}
	return "unknown"
}

// matchCheatBase looks for a list with the same base title as the game. Lists tagged with another region are not
// returned as a match, only as similar names; a list without a region tag or tagged World fits every region. When the
// game's region is unknown only untagged and World lists fit. Build tags are kept apart: a list fits only when
// its build key equals the game's, that is the same tags other than regions and languages ("Rev 1", "v1.1", "Beta",
// "Proto", "Demo", ...); no such tag on either side means the retail release. A list of another build is only named
// as similar, like one of another region.
func matchCheatBase(names map[string]string, g Game) (file string, similar []string) {
	want := map[string]bool{}
	for _, n := range []string{strings.TrimSuffix(g.Filename, path.Ext(g.Filename)), g.Title} {
		if b, _, _ := splitCheatName(n); b != "" {
			want[b] = true
		}
	}
	if len(want) == 0 {
		return "", nil
	}
	gr := gameRegions(g)
	gkey := gameBuildKey(g)
	type cand struct {
		file string
		tags int
	}
	var fit []cand
	for lower, f := range names {
		b, regs, tags := splitCheatName(lower)
		if !want[b] {
			continue
		}
		ok := len(regs) == 0
		for _, r := range regs {
			if r == "world" {
				ok = true
			}
			for _, x := range gr {
				if x == r {
					ok = true
				}
			}
		}
		if k, _ := cheatBuildKey(lower); ok && k != gkey {
			ok = false
		}
		if ok {
			fit = append(fit, cand{f, tags})
		} else {
			similar = append(similar, strings.TrimSuffix(f, filepath.Ext(f)))
		}
	}
	sort.Strings(similar)
	if len(fit) == 0 {
		return "", similar
	}
	sort.Slice(fit, func(i, j int) bool {
		if fit[i].tags != fit[j].tags {
			return fit[i].tags < fit[j].tags
		}
		return fit[i].file < fit[j].file
	})
	return fit[0].file, similar
}
