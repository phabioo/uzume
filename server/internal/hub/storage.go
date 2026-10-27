package hub

import (
	"context"
	"errors"
	"io/fs"
	"log/slog"
	"os"
	"path/filepath"
	"sort"
	"time"

	"github.com/phabioo/uzume/server/internal/backup"
)

// Admin storage overview with clean-up actions (0.19 package 7). Measured on request; directory walks never run
// inside a database transaction. Clean-up never touches saves except old automatic history versions (see
// cleanupOldSnapshots).

// Storage kinds in the fixed order of the overview.
const (
	StorageGames       = "games"
	StorageSaves       = "saves"
	StorageSnapshots   = "snapshots"
	StorageStates      = "states"
	StorageScreenshots = "screenshots"
	StorageCores       = "cores"
	StorageFirmware    = "firmware"
	StorageBackups     = "backups"
)

// Clean-up ids in the fixed order of the overview.
const (
	CleanupOldSnapshots   = "old_snapshots"
	CleanupUploadLeftover = "upload_leftovers"
	CleanupUnusedCores    = "unused_cores"
	// CleanupDeletedGameStates removes the save states of games that are no longer in the library.
	CleanupDeletedGameStates = "deleted_game_states"
)

const (
	// OldSnapshotAge: automatic history versions older than this are offered for clean-up.
	OldSnapshotAge = 90 * 24 * time.Hour
	// UploadLeftoverAge: resumable upload sessions idle for this long are offered for clean-up.
	UploadLeftoverAge = 7 * 24 * time.Hour
	// StorageScreenshotRows caps the per-user and game screenshot list.
	StorageScreenshotRows = 20
)

// ValidCleanupID reports whether id names a clean-up.
func ValidCleanupID(id string) bool {
	switch id {
	case CleanupOldSnapshots, CleanupUploadLeftover, CleanupUnusedCores, CleanupDeletedGameStates:
		return true
	}
	return false
}

// StorageKind is the size of one kind of data. Count is the number of files.
type StorageKind struct {
	ID    string
	Bytes int64
	Count int
}

// StorageScreenshotUsage is the screenshot storage of one user and game.
type StorageScreenshotUsage struct {
	UserID, UserName, GameID, GameName string
	Bytes                              int64
	Count                              int
}

// StorageCleanup is what a clean-up would free now. Count is the number of versions, upload sessions or core versions.
type StorageCleanup struct {
	ID    string
	Bytes int64
	Count int
}

// StorageOverview is the data of the admin Storage page, without the disks (the HTTP layer reads those).
type StorageOverview struct {
	MeasuredAt  time.Time
	Kinds       []StorageKind
	Screenshots []StorageScreenshotUsage
	Cleanup     []StorageCleanup
}

// StorageCleanupResult is what a clean-up freed.
type StorageCleanupResult struct {
	ID         string
	FreedBytes int64
	Removed    int
}

// dirStats sums the regular files below root (a missing directory is empty; symlinks are not followed).
func dirStats(root string) (bytes int64, count int) {
	_ = filepath.WalkDir(root, func(_ string, d fs.DirEntry, err error) error {
		if err != nil || !d.Type().IsRegular() {
			return nil
		}
		if info, err := d.Info(); err == nil {
			bytes += info.Size()
			count++
		}
		return nil
	})
	return bytes, count
}

// StorageOverview measures the data directory.
func (s *Service) StorageOverview(ctx context.Context) (StorageOverview, error) {
	out := StorageOverview{MeasuredAt: s.Now().UTC(), Screenshots: []StorageScreenshotUsage{}}
	games, gamesN := dirStats(filepath.Join(s.dataDir, "roms"))
	saves, savesN, snaps, snapsN, err := s.saveBlobStats(ctx)
	if err != nil {
		return StorageOverview{}, err
	}
	states, statesN := dirStats(filepath.Join(s.dataDir, "states")) // blobs and previews
	shots, err := s.ScreenshotUsage(ctx)
	if err != nil {
		return StorageOverview{}, err
	}
	cores, coresN := dirStats(filepath.Join(s.dataDir, "cores"))
	fw, fwN := dirStats(filepath.Join(s.dataDir, "firmware"))
	out.Kinds = []StorageKind{
		{StorageGames, games, gamesN}, {StorageSaves, saves, savesN}, {StorageSnapshots, snaps, snapsN},
		{StorageStates, states, statesN}, {StorageScreenshots, shots.TotalBytes, shots.TotalCount},
		{StorageCores, cores, coresN}, {StorageFirmware, fw, fwN},
	}
	if folder := s.backupSettings(ctx).Folder; folder != "" {
		var n int64
		var c int
		if ents, err := os.ReadDir(folder); err == nil {
			for _, e := range ents {
				if _, _, ok := backup.ParseName(e.Name()); !ok || !e.Type().IsRegular() {
					continue
				}
				if info, err := e.Info(); err == nil {
					n += info.Size()
					c++
				}
			}
		}
		out.Kinds = append(out.Kinds, StorageKind{StorageBackups, n, c})
	}

	rows := shots.Rows
	sort.SliceStable(rows, func(i, j int) bool {
		if rows[i].Bytes != rows[j].Bytes {
			return rows[i].Bytes > rows[j].Bytes
		}
		if rows[i].Count != rows[j].Count {
			return rows[i].Count > rows[j].Count
		}
		if rows[i].UserID != rows[j].UserID {
			return rows[i].UserID < rows[j].UserID
		}
		return rows[i].GameID < rows[j].GameID
	})
	if len(rows) > StorageScreenshotRows {
		rows = rows[:StorageScreenshotRows]
	}
	for _, r := range rows {
		u := StorageScreenshotUsage{UserID: r.UserID, GameID: r.GameID, Bytes: r.Bytes, Count: r.Count, GameName: "(removed game)"}
		_ = s.db.QueryRowContext(ctx, `SELECT display_name FROM users WHERE id = ?`, r.UserID).Scan(&u.UserName)
		_ = s.db.QueryRowContext(ctx, `SELECT title FROM games WHERE id = ?`, r.GameID).Scan(&u.GameName)
		out.Screenshots = append(out.Screenshots, u)
	}

	snapsOld, err := s.oldSnapshotStats(ctx)
	if err != nil {
		return StorageOverview{}, err
	}
	upIDs, err := s.idleUploads(ctx, UploadLeftoverAge)
	if err != nil {
		return StorageOverview{}, err
	}
	up := StorageCleanup{ID: CleanupUploadLeftover, Count: len(upIDs)}
	for _, id := range upIDs {
		if p, err := s.uploadPath(id); err == nil {
			if st, err := os.Stat(p); err == nil {
				up.Bytes += st.Size()
			}
		}
	}
	unused, err := s.unusedCoreVersions(ctx)
	if err != nil {
		return StorageOverview{}, err
	}
	uc := StorageCleanup{ID: CleanupUnusedCores, Count: len(unused)}
	for _, cv := range unused {
		b, _ := dirStats(filepath.Join(s.dataDir, "cores", cv[0], cv[1]))
		uc.Bytes += b
	}
	dgs, err := s.deletedGameStateStats(ctx)
	if err != nil {
		return StorageOverview{}, err
	}
	out.Cleanup = []StorageCleanup{snapsOld, up, uc, dgs}
	return out, nil
}

// saveBlobStats walks saves/<user>/<game>/<slot>/<sha>: a blob that a current checkpoint uses counts as a save, a blob
// only the history uses as a snapshot, any other file is not counted. The database is read before the walk.
func (s *Service) saveBlobStats(ctx context.Context) (saves int64, savesN int, snaps int64, snapsN int, err error) {
	current, err := s.blobKeys(ctx, `SELECT user_id, game_id, slot, sha256 FROM save_slots`)
	if err != nil {
		return
	}
	history, err := s.blobKeys(ctx, `SELECT DISTINCT user_id, game_id, slot, sha256 FROM save_history`)
	if err != nil {
		return
	}
	root := filepath.Join(s.dataDir, "saves")
	_ = filepath.WalkDir(root, func(p string, d fs.DirEntry, werr error) error {
		if werr != nil || !d.Type().IsRegular() {
			return nil
		}
		rel, rerr := filepath.Rel(root, p)
		if rerr != nil {
			return nil
		}
		key := filepath.ToSlash(rel)
		info, ierr := d.Info()
		if ierr != nil {
			return nil
		}
		switch {
		case current[key]:
			saves += info.Size()
			savesN++
		case history[key]:
			snaps += info.Size()
			snapsN++
		}
		return nil
	})
	return
}

// blobKeys runs a query of (user, game, slot, sha) and returns the keys as paths relative to saves/.
func (s *Service) blobKeys(ctx context.Context, query string) (map[string]bool, error) {
	rows, err := s.db.QueryContext(ctx, query)
	if err != nil {
		return nil, internal(err)
	}
	defer rows.Close()
	out := map[string]bool{}
	for rows.Next() {
		var u, g, sl, sha string
		if err := rows.Scan(&u, &g, &sl, &sha); err != nil {
			return nil, internal(err)
		}
		out[filepath.ToSlash(filepath.Join(u, g, sl, sha))] = true
	}
	return out, rows.Err()
}

// oldSnapshotCandidates is the set of history versions the old_snapshots clean-up removes: created before the cut-off,
// not a manual snapshot (users and plugins make those on purpose and they are kept forever), not in a slot with an
// open conflict. Current checkpoints are separate rows and never part of it.
const oldSnapshotCandidates = `SELECT h.user_id, h.game_id, h.slot, h.version, h.sha256, h.size FROM save_history h
	WHERE h.created_at < ?1 AND h.reason <> 'manual_snapshot' AND NOT EXISTS (SELECT 1 FROM save_conflicts c
		WHERE c.user_id = h.user_id AND c.game_id = h.game_id AND c.slot = h.slot AND c.status = 'open')`

// oldSnapshotStats counts the removable versions and the bytes of the blobs that go with them (blobs a current
// checkpoint or a kept version still uses stay).
func (s *Service) oldSnapshotStats(ctx context.Context) (StorageCleanup, error) {
	res := StorageCleanup{ID: CleanupOldSnapshots}
	cut := s.Now().Add(-OldSnapshotAge).Unix()
	if err := s.db.QueryRowContext(ctx, `SELECT COUNT(*) FROM (`+oldSnapshotCandidates+`)`, cut).Scan(&res.Count); err != nil {
		return res, internal(err)
	}
	if res.Count == 0 {
		return res, nil
	}
	err := s.db.QueryRowContext(ctx, `WITH cand AS (`+oldSnapshotCandidates+`)
		SELECT COALESCE(SUM(size), 0) FROM (SELECT DISTINCT c.user_id, c.game_id, c.slot, c.sha256, c.size FROM cand c
			WHERE NOT EXISTS (SELECT 1 FROM save_slots s WHERE s.user_id = c.user_id AND s.game_id = c.game_id AND s.slot = c.slot AND s.sha256 = c.sha256)
			AND NOT EXISTS (SELECT 1 FROM save_history h WHERE h.user_id = c.user_id AND h.game_id = c.game_id AND h.slot = c.slot AND h.sha256 = c.sha256
				AND NOT EXISTS (SELECT 1 FROM cand d WHERE d.user_id = h.user_id AND d.game_id = h.game_id AND d.slot = h.slot AND d.version = h.version)))`,
		cut).Scan(&res.Bytes)
	if err != nil {
		return res, internal(err)
	}
	return res, nil
}

// unusedCoreVersions lists the core package versions (core id, version) that no installed core of any system uses.
func (s *Service) unusedCoreVersions(ctx context.Context) ([][2]string, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT DISTINCT p.core_id, p.version FROM core_packages p
		WHERE NOT EXISTS (SELECT 1 FROM system_cores c WHERE c.core_id = p.core_id AND c.version = p.version) ORDER BY p.core_id, p.version`)
	if err != nil {
		return nil, internal(err)
	}
	defer rows.Close()
	var out [][2]string
	for rows.Next() {
		var cv [2]string
		if err := rows.Scan(&cv[0], &cv[1]); err != nil {
			return nil, internal(err)
		}
		out = append(out, cv)
	}
	return out, rows.Err()
}

// StorageCleanup runs one clean-up. An unknown id is a bad request.
func (s *Service) StorageCleanup(ctx context.Context, id string) (StorageCleanupResult, error) {
	var (
		res StorageCleanupResult
		err error
	)
	switch id {
	case CleanupOldSnapshots:
		res, err = s.cleanupOldSnapshots(ctx)
	case CleanupUploadLeftover:
		res.Removed, res.FreedBytes, err = s.cleanupUploads(ctx, UploadLeftoverAge, false)
	case CleanupUnusedCores:
		res, err = s.cleanupUnusedCores(ctx)
	case CleanupDeletedGameStates:
		res, err = s.cleanupDeletedGameStates(ctx)
	default:
		return StorageCleanupResult{}, badRequest("Unknown clean-up %q", id)
	}
	res.ID = id
	if err != nil {
		return StorageCleanupResult{}, err
	}
	slog.Info("storage clean-up", "id", id, "removed", res.Removed, "freed_bytes", res.FreedBytes)
	return res, nil
}

type snapshotRow struct {
	user, game, slot, sha string
	version               int
	size                  int64
}

// cleanupOldSnapshots deletes the old automatic history versions (see oldSnapshotCandidates) in one transaction, bumps
// history_high so that version numbers are never reused, and removes the blobs nothing references any more.
func (s *Service) cleanupOldSnapshots(ctx context.Context) (res StorageCleanupResult, err error) {
	defer s.publishOK(&err, TopicSaves)
	s.saveMu.Lock()
	defer s.saveMu.Unlock()
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return res, internal(err)
	}
	defer tx.Rollback()
	rows, err := tx.QueryContext(ctx, oldSnapshotCandidates, s.Now().Add(-OldSnapshotAge).Unix())
	if err != nil {
		return res, internal(err)
	}
	var cand []snapshotRow
	for rows.Next() {
		var r snapshotRow
		if err := rows.Scan(&r.user, &r.game, &r.slot, &r.version, &r.sha, &r.size); err != nil {
			rows.Close()
			return res, internal(err)
		}
		cand = append(cand, r)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return res, internal(err)
	}
	if len(cand) == 0 {
		return res, nil
	}
	for _, r := range cand {
		if _, err := tx.ExecContext(ctx, `DELETE FROM save_history WHERE user_id = ? AND game_id = ? AND slot = ? AND version = ?`,
			r.user, r.game, r.slot, r.version); err != nil {
			return res, internal(err)
		}
		// A resolved conflict whose secured upload goes has nothing left to show (as in thinSlot).
		if _, err := tx.ExecContext(ctx, `DELETE FROM save_conflicts WHERE user_id = ? AND game_id = ? AND slot = ? AND status <> 'open'
			AND secured_version = ?`, r.user, r.game, r.slot, r.version); err != nil {
			return res, internal(err)
		}
		if _, err := tx.ExecContext(ctx, `UPDATE save_slots SET history_high = MAX(history_high, ?) WHERE user_id = ? AND game_id = ? AND slot = ?`,
			r.version, r.user, r.game, r.slot); err != nil {
			return res, internal(err)
		}
	}
	if err := tx.Commit(); err != nil {
		return res, internal(err)
	}
	res.Removed = len(cand)
	seen := map[string]bool{}
	for _, r := range cand {
		file := s.saveFile(r.user, r.game, r.slot, r.sha)
		if seen[file] {
			continue
		}
		seen[file] = true
		st, serr := os.Stat(file)
		s.dropIfUnreferenced(ctx, r.user, r.game, r.slot, r.sha)
		if serr == nil {
			if _, gone := os.Stat(file); errors.Is(gone, fs.ErrNotExist) {
				res.FreedBytes += st.Size()
			}
		}
	}
	return res, nil
}

// cleanupUnusedCores deletes the package rows of core versions that no installed core uses and their cache
// directories. It runs under the core lock, so it cannot race an install, update or removal.
func (s *Service) cleanupUnusedCores(ctx context.Context) (res StorageCleanupResult, err error) {
	s.cores.mu.Lock()
	defer s.cores.mu.Unlock()
	unused, err := s.unusedCoreVersions(ctx) // before the transaction: the pool may hold a single connection
	if err != nil {
		return res, err
	}
	if len(unused) == 0 {
		return res, nil
	}
	var ids []string
	for _, cv := range unused {
		if len(ids) == 0 || ids[len(ids)-1] != cv[0] {
			ids = append(ids, cv[0])
		}
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return res, internal(err)
	}
	defer tx.Rollback()
	type gone struct{ core, version string }
	var dropped []gone
	for _, id := range ids {
		vs, err := dropUnreferencedVersions(ctx, tx, id)
		if err != nil {
			return res, internal(err)
		}
		for _, v := range vs {
			dropped = append(dropped, gone{id, v})
		}
	}
	if err := tx.Commit(); err != nil {
		return res, internal(err)
	}
	for _, g := range dropped {
		dir := filepath.Join(s.dataDir, "cores", g.core, g.version)
		b, _ := dirStats(dir)
		if err := os.RemoveAll(dir); err != nil {
			slog.Warn("remove core cache failed", "core_id", g.core, "version", g.version, "err", err)
			continue
		}
		res.FreedBytes += b
		res.Removed++
		os.Remove(filepath.Join(s.dataDir, "cores", g.core)) // only succeeds when empty
	}
	return res, nil
}
