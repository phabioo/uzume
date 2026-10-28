package hub

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/phabioo/uzume/server/internal/backup"
	"github.com/phabioo/uzume/server/internal/store"
	"github.com/phabioo/uzume/server/internal/sysmetrics"
)

// Server backup and restore (ADR 0029 D2). The archive format and the swap live in internal/backup; this file holds
// the settings, the one running job, the schedule and the status the interfaces show.

const (
	settingBackupMode    = "backup_mode"
	settingBackupWeekday = "backup_weekday"
	settingBackupTime    = "backup_time"
	settingBackupKeep    = "backup_keep"
	settingBackupROMs    = "backup_roms"
	settingBackupDir     = "backup_dir"
	settingBackupLast    = "backup_last"
	settingBackupLastOK  = "backup_last_ok"

	// DefaultBackupKeep is the number of regular backups kept by default.
	DefaultBackupKeep = 7
	// MaxBackupKeep bounds "Keep last N".
	MaxBackupKeep = 365

	folderProbeTTL = 30 * time.Second
)

// BackupSettings are the backup settings stored in the database.
type BackupSettings struct {
	Mode        string // backup.ModeOff, ModeDaily, ModeWeekly
	Weekday     int    // 0 = Sunday, used by weekly
	Time        string // HH:MM in the server's local time zone
	Keep        int
	IncludeROMs bool
	Folder      string // "" = no folder: backups are off
}

// BackupSettingsUpdate changes the given fields only.
type BackupSettingsUpdate struct {
	Mode        *string
	Weekday     *int
	Time        *string
	Keep        *int
	IncludeROMs *bool
	Folder      *string
}

// BackupRun is the outcome of one backup.
type BackupRun struct {
	OK        bool      `json:"ok"`
	Started   time.Time `json:"started"`
	Finished  time.Time `json:"finished"`
	Name      string    `json:"name,omitempty"`
	Size      int64     `json:"size,omitempty"`
	Error     string    `json:"error,omitempty"`
	NoSpace   bool      `json:"no_space,omitempty"`
	By        string    `json:"by,omitempty"` // "schedule" or the admin's username
	Skipped   int       `json:"skipped,omitempty"`
	PruneNote string    `json:"prune_note,omitempty"`
}

// BackupProgress describes the running job.
type BackupProgress struct {
	Kind        string // "backup" or "restore"
	Phase       string
	Step        int // restore: 1 safety backup, 2 verify and prepare, 3 restarting
	Done, Total int64
	Started     time.Time
	By          string
	ETA         time.Duration // 0 = unknown
}

// BackupStatus is everything a status card needs.
type BackupStatus struct {
	Settings   BackupSettings
	FolderSet  bool
	WriteError string // not empty: the folder cannot be written
	DiskTotal  uint64
	DiskFree   uint64
	DiskOK     bool
	Running    *BackupProgress
	Last       *BackupRun // the last attempt
	LastOK     *BackupRun // the last good backup
	Next       time.Time  // zero: nothing scheduled
	Restore    *backup.RestoreResult
	CanRestart bool
}

// BackupEntry is a backup file in the folder.
type BackupEntry struct {
	Name, Path    string
	Size          int64
	Created       time.Time
	Safety        bool
	Contents      string
	IncludesROMs  bool
	HubVersion    string
	SchemaVersion int
	Err           string // not empty: the file is damaged or unreadable
	Restored      bool   // the last restore used this file
}

type backupJob struct {
	cancel  context.CancelFunc
	kind    string
	by      string
	started time.Time
	phase   atomic.Value // string
	step    atomic.Int32
	done    atomic.Int64
	total   atomic.Int64
}

type backupState struct {
	mu      sync.Mutex
	job     *backupJob
	base    context.Context
	wg      sync.WaitGroup
	wake    chan struct{}
	extras  []backup.Extra
	free    backup.FreeFunc
	restart func()
	probe   struct {
		at  time.Time
		dir string
		err error
	}
}

func (s *Service) initBackup(o Options) {
	s.bk.wake = make(chan struct{}, 1)
	s.bk.extras, s.bk.free = o.BackupExtras, o.BackupFree
}

// SetBackupRestart sets the function that restarts the server process after a restore is prepared (nil: no restore).
func (s *Service) SetBackupRestart(f func()) {
	s.bk.mu.Lock()
	s.bk.restart = f
	s.bk.mu.Unlock()
}

// stopBackup cancels a running job and waits for it (bounded).
func (s *Service) stopBackup() {
	s.bk.mu.Lock()
	if s.bk.job != nil {
		s.bk.job.cancel()
	}
	s.bk.mu.Unlock()
	done := make(chan struct{})
	go func() { s.bk.wg.Wait(); close(done) }()
	select {
	case <-done:
	case <-time.After(10 * time.Second):
	}
}

// ---- settings ----

func (s *Service) backupSettings(ctx context.Context) BackupSettings {
	st := BackupSettings{Mode: backup.ModeOff, Weekday: int(time.Sunday), Time: "03:00", Keep: DefaultBackupKeep}
	get := func(k string) (string, bool) {
		v, ok, err := s.getSetting(ctx, k)
		return v, ok && err == nil
	}
	if v, ok := get(settingBackupMode); ok && (v == backup.ModeDaily || v == backup.ModeWeekly) {
		st.Mode = v
	}
	if v, ok := get(settingBackupWeekday); ok {
		if n, err := strconv.Atoi(v); err == nil && n >= 0 && n <= 6 {
			st.Weekday = n
		}
	}
	if v, ok := get(settingBackupTime); ok {
		if _, _, ok := parseClock(v); ok {
			st.Time = v
		}
	}
	if v, ok := get(settingBackupKeep); ok {
		if n, err := strconv.Atoi(v); err == nil && n >= 1 && n <= MaxBackupKeep {
			st.Keep = n
		}
	}
	if v, ok := get(settingBackupROMs); ok {
		st.IncludeROMs = v == "1"
	}
	if v, ok := get(settingBackupDir); ok {
		st.Folder = v
	}
	return st
}

// BackupSettings returns the stored settings (defaults for unset values).
func (s *Service) BackupSettings(ctx context.Context) BackupSettings { return s.backupSettings(ctx) }

func parseClock(v string) (h, m int, ok bool) {
	hs, ms, found := strings.Cut(v, ":")
	if !found || len(hs) != 2 || len(ms) != 2 {
		return 0, 0, false
	}
	h, e1 := strconv.Atoi(hs)
	m, e2 := strconv.Atoi(ms)
	if e1 != nil || e2 != nil || h < 0 || h > 23 || m < 0 || m > 59 {
		return 0, 0, false
	}
	return h, m, true
}

// ValidateBackupFolder checks a backup folder value: absolute, not the data directory itself and not inside one of
// the directories a restore replaces (saves, plugins, firmware, tls, roms, states) or the migration backups.
func (s *Service) ValidateBackupFolder(dir string) error {
	if dir == "" {
		return nil
	}
	if !filepath.IsAbs(dir) {
		return badRequest("The backup folder must be an absolute path")
	}
	dir = filepath.Clean(dir)
	data, err := filepath.Abs(s.dataDir)
	if err != nil {
		return internal(err)
	}
	if dir == data {
		return badRequest("The backup folder cannot be the data folder itself; choose a sub folder or, better, another disk")
	}
	for _, d := range []string{"saves", "states", "plugins", "firmware", "tls", "roms", "backups", "tmp", backup.StagingDir, "restore-old"} {
		p := filepath.Join(data, d)
		if dir == p || strings.HasPrefix(dir, p+string(filepath.Separator)) {
			return badRequest("The backup folder cannot be inside %s of the data folder", d)
		}
	}
	return nil
}

// SetBackupSettings validates and stores the given settings.
func (s *Service) SetBackupSettings(ctx context.Context, u BackupSettingsUpdate) error {
	if u.Mode != nil && *u.Mode != backup.ModeOff && *u.Mode != backup.ModeDaily && *u.Mode != backup.ModeWeekly {
		return badRequest("Schedule must be off, daily or weekly")
	}
	if u.Weekday != nil && (*u.Weekday < 0 || *u.Weekday > 6) {
		return badRequest("Weekday must be 0 (Sunday) to 6 (Saturday)")
	}
	if u.Time != nil {
		if _, _, ok := parseClock(*u.Time); !ok {
			return badRequest("Time must be HH:MM")
		}
	}
	if u.Keep != nil && (*u.Keep < 1 || *u.Keep > MaxBackupKeep) {
		return badRequest("Keep must be between 1 and %d", MaxBackupKeep)
	}
	if u.Folder != nil {
		f := strings.TrimSpace(*u.Folder)
		if f != "" {
			f = filepath.Clean(f)
		}
		u.Folder = &f
		if err := s.ValidateBackupFolder(f); err != nil {
			return err
		}
	}
	set := func(k, v string) error { return s.setSetting(ctx, k, v) }
	var err error
	if u.Mode != nil {
		err = set(settingBackupMode, *u.Mode)
	}
	if err == nil && u.Weekday != nil {
		err = set(settingBackupWeekday, strconv.Itoa(*u.Weekday))
	}
	if err == nil && u.Time != nil {
		err = set(settingBackupTime, *u.Time)
	}
	if err == nil && u.Keep != nil {
		err = set(settingBackupKeep, strconv.Itoa(*u.Keep))
	}
	if err == nil && u.IncludeROMs != nil {
		v := "0"
		if *u.IncludeROMs {
			v = "1"
		}
		err = set(settingBackupROMs, v)
	}
	if err == nil && u.Folder != nil {
		err = set(settingBackupDir, *u.Folder)
		s.bk.mu.Lock()
		s.bk.probe.at = time.Time{}
		s.bk.mu.Unlock()
	}
	if err != nil {
		return err
	}
	select {
	case s.bk.wake <- struct{}{}:
	default:
	}
	return nil
}

// ---- status ----

func (s *Service) readRun(ctx context.Context, key string) *BackupRun {
	v, ok, err := s.getSetting(ctx, key)
	if err != nil || !ok || v == "" {
		return nil
	}
	var r BackupRun
	if json.Unmarshal([]byte(v), &r) != nil {
		return nil
	}
	return &r
}

func (s *Service) writeRun(ctx context.Context, key string, r BackupRun) {
	if b, err := json.Marshal(r); err == nil {
		_ = s.setSetting(ctx, key, string(b))
	}
}

// folderError probes the folder (cached for 30 s unless force) and returns the reason it cannot be written.
func (s *Service) folderError(dir string, force bool) string {
	if dir == "" {
		return ""
	}
	s.bk.mu.Lock()
	defer s.bk.mu.Unlock()
	if force || s.bk.probe.dir != dir || time.Since(s.bk.probe.at) > folderProbeTTL {
		s.bk.probe.dir, s.bk.probe.at = dir, time.Now()
		s.bk.probe.err = nil
		if s.bk.job == nil { // never create the folder under a running backup
			s.bk.probe.err = backup.CheckWritable(dir)
		}
	}
	if s.bk.probe.err == nil {
		return ""
	}
	var nw *backup.NotWritableError
	if errors.As(s.bk.probe.err, &nw) && nw.Err != nil {
		return nw.Err.Error()
	}
	return s.bk.probe.err.Error()
}

// RecheckBackupFolder probes the folder again now and returns the reason it cannot be written ("" = fine).
func (s *Service) RecheckBackupFolder(ctx context.Context) string {
	return s.folderError(s.backupSettings(ctx).Folder, true)
}

func (s *Service) progressOf(j *backupJob) *BackupProgress {
	if j == nil {
		return nil
	}
	ph, _ := j.phase.Load().(string)
	p := &BackupProgress{Kind: j.kind, Phase: ph, Step: int(j.step.Load()), Done: j.done.Load(), Total: j.total.Load(), Started: j.started, By: j.by}
	if p.Done > 0 && p.Total > p.Done {
		el := time.Since(j.started)
		p.ETA = time.Duration(float64(el) / float64(p.Done) * float64(p.Total-p.Done))
	}
	return p
}

// BackupStatus assembles the status. The folder probe is cached, the rest is a few small reads.
func (s *Service) BackupStatus(ctx context.Context) BackupStatus {
	st := BackupStatus{Settings: s.backupSettings(ctx)}
	st.FolderSet = st.Settings.Folder != ""
	st.WriteError = s.folderError(st.Settings.Folder, false)
	if st.FolderSet {
		if total, free, ok := sysmetrics.Disk(st.Settings.Folder); ok {
			st.DiskTotal, st.DiskFree, st.DiskOK = total, free, true
		}
	}
	s.bk.mu.Lock()
	st.Running = s.progressOf(s.bk.job)
	st.CanRestart = s.bk.restart != nil
	s.bk.mu.Unlock()
	st.Last, st.LastOK = s.readRun(ctx, settingBackupLast), s.readRun(ctx, settingBackupLastOK)
	st.Restore = backup.ReadResult(s.dataDir)
	if st.FolderSet && st.Settings.Mode != backup.ModeOff {
		h, m, _ := parseClock(st.Settings.Time)
		st.Next = backup.NextRun(s.Now(), time.Local, st.Settings.Mode, time.Weekday(st.Settings.Weekday), h, m)
	}
	return st
}

// BackupProblem is what the Status page and the sidebar badge show: a failed last backup or an unwritable folder.
type BackupProblem struct {
	Title, Detail string
}

// BackupProblem returns the open backup problem, or nil.
func (s *Service) BackupProblem(ctx context.Context) *BackupProblem {
	set := s.backupSettings(ctx)
	if set.Folder == "" {
		return nil
	}
	if e := s.folderError(set.Folder, false); e != "" {
		return &BackupProblem{Title: "Cannot write", Detail: fmt.Sprintf("The server cannot write to the backup folder %s: %s. Check that the disk is mounted, or choose another folder.", set.Folder, e)}
	}
	if last := s.readRun(ctx, settingBackupLast); last != nil && !last.OK {
		d := last.Error
		if ok := s.readRun(ctx, settingBackupLastOK); ok != nil {
			d += " The last good backup is from " + ok.Finished.Local().Format("2 Jan 15:04") + "."
		}
		return &BackupProblem{Title: "Backup failed", Detail: d}
	}
	return nil
}

// ---- list, delete, download ----

// ListBackups lists the backup files of the configured folder, newest first.
func (s *Service) ListBackups(ctx context.Context) ([]BackupEntry, error) {
	set := s.backupSettings(ctx)
	if set.Folder == "" {
		return nil, nil
	}
	infos, err := backup.List(set.Folder)
	if err != nil {
		return nil, nil // an unreadable folder shows up as the "Cannot write" state
	}
	restored := ""
	if r := backup.ReadResult(s.dataDir); r != nil && r.OK {
		restored = r.Backup
	}
	out := make([]BackupEntry, 0, len(infos))
	for _, i := range infos {
		e := BackupEntry{Name: i.Name, Path: i.Path, Size: i.Size, Created: i.Time, Safety: i.Safety, Err: i.Err, Restored: i.Name == restored}
		if m := i.Manifest; m != nil {
			e.Contents, e.IncludesROMs, e.HubVersion, e.SchemaVersion = m.ContentsText(), m.IncludesROMs, m.HubVersion, m.SchemaVersion
		}
		out = append(out, e)
	}
	return out, nil
}

func (s *Service) backupPath(ctx context.Context, name string) (string, error) {
	set := s.backupSettings(ctx)
	if set.Folder == "" {
		return "", ErrNotFound
	}
	if strings.Contains(name, "..") || strings.ContainsAny(name, `/\`) || !filepath.IsLocal(name) {
		return "", ErrNotFound
	}
	p, err := backup.Find(set.Folder, name)
	if err != nil {
		return "", ErrNotFound
	}
	return p, nil
}

// DeleteBackup removes one backup file. Safety backups are deleted only here, on request.
func (s *Service) DeleteBackup(ctx context.Context, name string) error {
	p, err := s.backupPath(ctx, name)
	if err != nil {
		return err
	}
	s.bk.mu.Lock()
	busy := s.bk.job != nil && s.bk.job.kind == "restore"
	s.bk.mu.Unlock()
	if busy {
		return ErrConflict
	}
	if err := os.Remove(p); err != nil {
		return internal(err)
	}
	return nil
}

// OpenBackup opens a backup file for download.
func (s *Service) OpenBackup(ctx context.Context, name string) (*os.File, os.FileInfo, error) {
	p, err := s.backupPath(ctx, name)
	if err != nil {
		return nil, nil, err
	}
	f, err := os.Open(p)
	if err != nil {
		return nil, nil, ErrNotFound
	}
	st, err := f.Stat()
	if err != nil {
		f.Close()
		return nil, nil, internal(err)
	}
	return f, st, nil
}

// DismissRestoreResult hides the "Restored" / "Not restored" card.
func (s *Service) DismissRestoreResult() { backup.DismissResult(s.dataDir) }

// ---- running a backup ----

func (s *Service) backupOptions(set BackupSettings, safety bool, p func(backup.Progress)) backup.Options {
	info := s.Info()
	ver, _ := store.SchemaVersion(s.db)
	return backup.Options{DataDir: s.dataDir, TargetDir: set.Folder, HubName: info.Name, HubID: info.HubID, HubVersion: info.HubVersion,
		SchemaVersion: ver, Safety: safety, IncludeROMs: set.IncludeROMs, Extras: s.bk.extras, Progress: p, Now: s.Now, Free: s.bk.free,
		Snapshot: func(dest string) error { return store.SnapshotTo(s.db, dest) },
		Lock:     func() func() { s.saveMu.Lock(); return s.saveMu.Unlock }}
}

func (j *backupJob) report(p backup.Progress) {
	j.phase.Store(p.Phase)
	j.done.Store(p.Done)
	j.total.Store(p.Total)
}

// begin registers the one running job. The job context ends with the server.
func (s *Service) begin(kind, by string) (*backupJob, context.Context, error) {
	s.bk.mu.Lock()
	defer s.bk.mu.Unlock()
	if s.bk.job != nil {
		return nil, nil, &Error{Code: CodeConflict, Message: "A backup or restore is already running"}
	}
	base := s.bk.base
	if base == nil {
		base = context.Background()
	}
	ctx, cancel := context.WithCancel(base)
	j := &backupJob{cancel: cancel, kind: kind, by: by, started: time.Now()}
	j.phase.Store("starting")
	s.bk.job = j
	s.bk.wg.Add(1)
	return j, ctx, nil
}

func (s *Service) end(j *backupJob) {
	s.bk.mu.Lock()
	if s.bk.job == j {
		s.bk.job = nil
	}
	s.bk.mu.Unlock()
	j.cancel()
	s.bk.wg.Done()
}

// StartBackup starts a backup in the background (one at a time). by is "schedule" or the admin's name.
func (s *Service) StartBackup(ctx context.Context, by string) error {
	set := s.backupSettings(ctx)
	if set.Folder == "" {
		return badRequest("Choose a backup folder first")
	}
	if e := s.folderError(set.Folder, true); e != "" {
		return badRequest("The backup folder cannot be written: %s", e)
	}
	j, jctx, err := s.begin("backup", by)
	if err != nil {
		return err
	}
	go func() {
		defer s.end(j)
		s.runBackup(jctx, j, set, by)
	}()
	return nil
}

// runBackup is one backup with retention and the stored result.
func (s *Service) runBackup(ctx context.Context, j *backupJob, set BackupSettings, by string) {
	res, err := backup.Create(ctx, s.backupOptions(set, false, j.report))
	run := BackupRun{Started: j.started, Finished: time.Now(), By: by}
	if err != nil {
		if errors.Is(err, context.Canceled) {
			return // canceled: nothing recorded, the partial file is gone
		}
		run.Error = err.Error()
		var ns *backup.NoSpaceError
		run.NoSpace = errors.As(err, &ns)
		s.writeRun(context.Background(), settingBackupLast, run)
		s.RecordServerError(context.Background(), ErrKindServerError, "Backup failed", "Backup failed: "+err.Error())
		return
	}
	run.OK, run.Name, run.Size, run.Skipped = true, res.Name, res.Size, res.Manifest.Skipped
	// Retention: only now, after the new backup exists, and never because of free space (decision 26).
	if _, perr := backup.Prune(set.Folder, set.Keep); perr != nil {
		run.PruneNote = "Older backups could not be removed: " + perr.Error()
	}
	s.writeRun(context.Background(), settingBackupLast, run)
	s.writeRun(context.Background(), settingBackupLastOK, run)
}

// CancelBackup stops the running backup. It reports whether one was running. A restore cannot be canceled once it
// is past the safety backup.
func (s *Service) CancelBackup() bool {
	s.bk.mu.Lock()
	defer s.bk.mu.Unlock()
	if s.bk.job == nil || (s.bk.job.kind == "restore" && s.bk.job.step.Load() > 1) {
		return false
	}
	s.bk.job.cancel()
	return true
}

// ---- schedule ----

// RunBackupSchedule runs the scheduled backups until ctx ends. A run missed while the server was down is not made up.
func (s *Service) RunBackupSchedule(ctx context.Context, log func(msg string, args ...any)) {
	s.bk.mu.Lock()
	s.bk.base = ctx
	s.bk.mu.Unlock()
	for {
		set := s.backupSettings(ctx)
		var wait <-chan time.Time
		var timer *time.Timer
		due := false // the timer is the run, not just a look at the settings
		if set.Folder != "" && set.Mode != backup.ModeOff {
			h, m, _ := parseClock(set.Time)
			if next := backup.NextRun(s.Now(), time.Local, set.Mode, time.Weekday(set.Weekday), h, m); !next.IsZero() {
				d := time.Until(next)
				if due = d <= schedulePoll; !due {
					d = schedulePoll // settings changed by the CLI are noticed within this time
				}
				timer = time.NewTimer(d)
				wait = timer.C
			}
		} else {
			timer = time.NewTimer(schedulePoll)
			wait = timer.C
		}
		select {
		case <-ctx.Done():
			timer.Stop()
			return
		case <-s.bk.wake:
			timer.Stop()
		case <-wait:
			if due {
				if err := s.StartBackup(ctx, "schedule"); err != nil {
					log("scheduled backup not started", "err", err)
				}
				// Do not fire twice for the same minute.
				select {
				case <-time.After(1100 * time.Millisecond):
				case <-ctx.Done():
					return
				}
			}
		}
	}
}

// schedulePoll is how often the schedule looks at the stored settings when nothing woke it.
const schedulePoll = 5 * time.Minute

// ---- restore ----

// RestoreInfo is what the confirmation shows before a restore.
type RestoreInfo struct {
	Entry            BackupEntry
	AddedUsers       int
	AddedDevices     int
	ConnectedPlayers int
	SafetyNeed       int64 // estimated size of the safety backup (size of the last good backup, else 0)
	SafetyFolder     string
}

// RestoreInfo reads the manifest of a backup and what a restore would replace.
func (s *Service) RestoreInfo(ctx context.Context, name string) (RestoreInfo, error) {
	var ri RestoreInfo
	p, err := s.backupPath(ctx, name)
	if err != nil {
		return ri, err
	}
	m, err := backup.ReadManifest(p)
	if err != nil {
		return ri, &Error{Code: CodeBadRequest, Message: err.Error()}
	}
	if err := checkNewer(m); err != nil {
		return ri, &Error{Code: CodeBadRequest, Message: err.Error()}
	}
	st, _ := os.Stat(p)
	ri.Entry = BackupEntry{Name: name, Path: p, Created: m.Created, Safety: m.Safety, Contents: m.ContentsText(), IncludesROMs: m.IncludesROMs,
		HubVersion: m.HubVersion, SchemaVersion: m.SchemaVersion}
	if st != nil {
		ri.Entry.Size = st.Size()
	}
	_ = s.db.QueryRowContext(ctx, `SELECT COUNT(*) FROM users WHERE created_at > ?`, m.Created.Unix()).Scan(&ri.AddedUsers)
	_ = s.db.QueryRowContext(ctx, `SELECT COUNT(*) FROM devices WHERE created_at > ?`, m.Created.Unix()).Scan(&ri.AddedDevices)
	ri.ConnectedPlayers = len(s.clients())
	if ok := s.readRun(ctx, settingBackupLastOK); ok != nil {
		ri.SafetyNeed = ok.Size
	}
	ri.SafetyFolder = s.backupSettings(ctx).Folder
	return ri, nil
}

func checkNewer(m backup.Manifest) error {
	if m.Format > backup.FormatVersion {
		return &backup.NewerError{Format: true}
	}
	if known := store.KnownSchemaVersion(); m.SchemaVersion > known {
		return &backup.NewerError{Schema: m.SchemaVersion, Known: known}
	}
	return nil
}

// StartRestore restores a backup of the folder: safety backup, verify and unpack to staging, then the server restarts
// and swaps the data in before it opens the database. A newer backup is rejected here, before anything happens.
// Anything that fails later is stored as the restore result and the server runs as before.
func (s *Service) StartRestore(ctx context.Context, name, by string) error {
	set := s.backupSettings(ctx)
	p, err := s.backupPath(ctx, name)
	if err != nil {
		return err
	}
	m, err := backup.ReadManifest(p)
	if err != nil {
		return badRequest("%s", err.Error())
	}
	if err := checkNewer(m); err != nil {
		return badRequest("%s", err.Error())
	}
	s.bk.mu.Lock()
	restart := s.bk.restart
	s.bk.mu.Unlock()
	if restart == nil {
		return badRequest("This server cannot restart itself. Restore with 'uzume-server restore <file>' while the server is stopped")
	}
	if e := s.folderError(set.Folder, true); e != "" {
		return badRequest("The backup folder cannot be written, so the safety backup is not possible: %s", e)
	}
	j, jctx, err := s.begin("restore", by)
	if err != nil {
		return err
	}
	backup.DismissResult(s.dataDir)
	go func() {
		defer s.end(j)
		fail := func(cause error) {
			if errors.Is(cause, context.Canceled) {
				return
			}
			_ = backup.WriteResult(s.dataDir, backup.RestoreResult{At: s.Now(), Backup: name, BackupCreated: m.Created, Error: cause.Error()})
		}
		j.step.Store(1)
		safety, err := backup.Create(jctx, s.backupOptions(set, true, j.report))
		if err != nil {
			fail(fmt.Errorf("safety backup failed, nothing was changed: %w", err))
			return
		}
		j.step.Store(2)
		staging := filepath.Join(s.dataDir, backup.StagingDir)
		_, items, err := backup.Extract(jctx, p, staging, backup.ExtractOptions{MaxSchema: store.KnownSchemaVersion(), Progress: j.report})
		if err != nil {
			res := backup.RestoreResult{At: s.Now(), Backup: name, BackupCreated: m.Created, Safety: safety.Name, Error: err.Error()}
			if !errors.Is(err, context.Canceled) {
				_ = backup.WriteResult(s.dataDir, res)
			}
			return
		}
		if err := backup.WritePending(s.dataDir, backup.Pending{Staging: staging, Items: items, Backup: name, BackupCreated: m.Created,
			IncludesROMs: m.IncludesROMs, Safety: safety.Name, Requested: s.Now(), By: by}); err != nil {
			os.RemoveAll(staging)
			fail(err)
			return
		}
		j.step.Store(3)
		j.report(backup.Progress{Phase: "restarting"})
		restart()
		<-jctx.Done() // the process ends; keep the "restarting" state until then
	}()
	return nil
}
