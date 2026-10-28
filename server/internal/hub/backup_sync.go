package hub

import (
	"context"
	"errors"
	"path/filepath"
	"time"

	"github.com/phabioo/uzume/server/internal/backup"
)

// SafetyBackup makes a safety backup into dir and waits for it (restore from the CLI). Safety backups are never
// removed by the retention.
func (s *Service) SafetyBackup(ctx context.Context, dir string, p func(backup.Progress)) (backup.Result, error) {
	set := s.backupSettings(ctx)
	set.Folder = filepath.Clean(dir)
	return backup.Create(ctx, s.backupOptions(set, true, p))
}

// SafetyBackupWithoutROMs is SafetyBackup that leaves the ROM library out whatever the backup settings say (the
// safety backup of `uzume-server reset`, which keeps saves, users and settings but not the games).
func (s *Service) SafetyBackupWithoutROMs(ctx context.Context, dir string, p func(backup.Progress)) (backup.Result, error) {
	set := s.backupSettings(ctx)
	set.Folder = filepath.Clean(dir)
	set.IncludeROMs = false
	return backup.Create(ctx, s.backupOptions(set, true, p))
}

// BackupNow makes one backup and waits for it (the CLI). out is the target folder ("" = the configured folder);
// roms overrides "Include ROMs" when not nil. A backup into the configured folder follows the normal rules: the
// result is stored for the status and the retention runs after it succeeded. A backup into another folder is
// just a copy: nothing is stored and nothing is deleted.
func (s *Service) BackupNow(ctx context.Context, out string, roms *bool, by string, p func(backup.Progress)) (backup.Result, error) {
	set := s.backupSettings(ctx)
	configured := set.Folder
	if out == "" {
		out = configured
	}
	if out == "" {
		return backup.Result{}, badRequest("No backup folder: pass --out DIR or set one with 'uzume-server backup config --folder DIR'")
	}
	if !filepath.IsAbs(out) {
		abs, err := filepath.Abs(out)
		if err != nil {
			return backup.Result{}, badRequest("%v", err)
		}
		out = abs
	}
	set.Folder = filepath.Clean(out)
	if roms != nil {
		set.IncludeROMs = *roms
	}
	if err := s.ValidateBackupFolder(set.Folder); err != nil {
		return backup.Result{}, err
	}
	started := time.Now()
	res, err := backup.Create(ctx, s.backupOptions(set, false, p))
	if set.Folder != filepath.Clean(configured) || configured == "" {
		return res, err
	}
	run := BackupRun{Started: started, Finished: time.Now(), By: by}
	if err != nil {
		if !errors.Is(err, context.Canceled) {
			run.Error = err.Error()
			var ns *backup.NoSpaceError
			run.NoSpace = errors.As(err, &ns)
			s.writeRun(context.Background(), settingBackupLast, run)
			s.RecordServerError(context.Background(), ErrKindServerError, "Backup failed", "Backup failed: "+err.Error())
		}
		return res, err
	}
	run.OK, run.Name, run.Size, run.Skipped = true, res.Name, res.Size, res.Manifest.Skipped
	if _, perr := backup.Prune(set.Folder, set.Keep); perr != nil {
		run.PruneNote = "Older backups could not be removed: " + perr.Error()
	}
	s.writeRun(context.Background(), settingBackupLast, run)
	s.writeRun(context.Background(), settingBackupLastOK, run)
	return res, nil
}
