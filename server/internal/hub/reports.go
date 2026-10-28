package hub

import (
	"archive/zip"
	"context"
	"database/sql"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"
	"unicode/utf8"

	"github.com/google/uuid"

	"github.com/phabioo/uzume/server/internal/redact"
)

// Problem reports (0.17, ADR 0029 D3, handshake feature `reports_v1`). An app sends a description and a few
// attachments to the server of its user; admins list, download and delete them. Files live under <data>/reports/<id>/,
// SQLite holds the metadata. Nothing is shared with another server. Text is run through the redactor again here (the
// App already removes secrets): defense in depth, never the only protection.

// FeatureReportsV1 is the handshake feature flag for problem reports.
const FeatureReportsV1 = "reports_v1"

// Limits of a report.
const (
	ReportMaxBytes         = 25 << 20 // all attachments of one report together
	ReportMaxFiles         = 12
	ReportMaxDescription   = 8000
	ReportsPerDevicePerHr  = 5   // reports one device may send per hour
	ReportsKeptMax         = 300 // reports the server keeps before it refuses new ones (until an admin deletes some)
	reportNameMax          = 80
	reportTitleMax         = 80
	reportDescriptionFile  = "report.txt" // the description in the zip archive; no attachment may use this name
	reportConnectionCheck  = "connection-check.json"
	reportStatusNew        = "new"
	reportStatusSeen       = "seen"
	reportRateWindow       = time.Hour
	reportMetaFieldMaxLine = 200
)

// Report statuses.
const (
	ReportNew  = reportStatusNew
	ReportSeen = reportStatusSeen
)

var reportNameRe = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._-]*$`)

// reportTextExt are the attachment types the server redacts; others are stored as they arrive.
var reportTextExt = map[string]bool{".txt": true, ".log": true, ".json": true, ".ini": true, ".cfg": true, ".conf": true,
	".csv": true, ".md": true, ".xml": true}

// ErrReportRateLimited: the device sent too many reports within the last hour, or the server keeps too many.
var ErrReportRateLimited = &Error{Code: CodeRateLimited, Message: "Too many reports. Try again later."}

// Report is one stored problem report.
type Report struct {
	ID                 int64
	Status             string
	Title              string
	Description        string
	UserID, User       string
	DeviceID, Device   string
	PlayerVersion, OS  string
	EventTitle         string
	EventTime          *time.Time
	CreatedAt          time.Time
	SeenAt             *time.Time
	Size               int64
	Files              []ReportFile
	HasConnectionCheck bool
}

// ReportFile is one attachment.
type ReportFile struct {
	Name string
	Size int64
}

// ReportCounts are the list chips.
type ReportCounts struct{ All, New, Seen int }

// ReportInput is what an app sends besides the files.
type ReportInput struct {
	Description   string
	EventTitle    string
	EventTime     *time.Time
	PlayerVersion string
	OS            string
}

// ReportFileSource yields the attachments one by one; Next returns io.EOF after the last one. The reader is valid
// until the next call.
type ReportFileSource interface {
	Next() (name string, r io.Reader, err error)
}

type reportState struct {
	mu   sync.Mutex
	hits map[string][]time.Time // device id -> send times within the window
}

func (s *Service) reportsDir() string { return filepath.Join(s.dataDir, "reports") }

func (s *Service) reportDir(id int64) string {
	return filepath.Join(s.reportsDir(), strconv.FormatInt(id, 10))
}

// reportRate records a send for the device and refuses the sixth within the hour. Counted per request, also when
// the upload fails later, so a loop of failing uploads cannot hammer the disk.
func (s *Service) reportRate(deviceID string) error {
	now := s.Now()
	s.rep.mu.Lock()
	defer s.rep.mu.Unlock()
	if s.rep.hits == nil {
		s.rep.hits = map[string][]time.Time{}
	}
	keep := s.rep.hits[deviceID][:0]
	for _, t := range s.rep.hits[deviceID] {
		if now.Sub(t) < reportRateWindow {
			keep = append(keep, t)
		}
	}
	if len(keep) >= ReportsPerDevicePerHr {
		s.rep.hits[deviceID] = keep
		return ErrReportRateLimited
	}
	s.rep.hits[deviceID] = append(keep, now)
	return nil
}

func cleanLine(s string, max int) string {
	s = strings.Join(strings.Fields(s), " ")
	if utf8.RuneCountInString(s) > max {
		r := []rune(s)
		s = string(r[:max])
	}
	return s
}

func reportTitle(desc string) string {
	line := strings.TrimSpace(strings.SplitN(strings.TrimSpace(desc), "\n", 2)[0])
	t := cleanLine(line, reportTitleMax)
	if t == "" {
		return "Problem report"
	}
	return t
}

// CreateReport stores a report of the authenticated device and returns its number. Errors: ErrBadRequest (empty or
// too long description, bad or duplicate file name, too many files), ErrPayloadTooLarge, ErrReportRateLimited.
func (s *Service) CreateReport(ctx context.Context, p Principal, in ReportInput, files ReportFileSource) (id int64, err error) {
	desc := strings.TrimSpace(in.Description)
	if desc == "" || utf8.RuneCountInString(desc) > ReportMaxDescription || !utf8.ValidString(desc) {
		return 0, &Error{Code: CodeBadRequest, Message: "The description is missing or longer than 8000 characters"}
	}
	if err := s.reportRate(p.Device.ID); err != nil {
		return 0, err
	}
	var kept int
	if err := s.db.QueryRowContext(ctx, `SELECT COUNT(*) FROM reports`).Scan(&kept); err != nil {
		return 0, internal(err)
	}
	if kept >= ReportsKeptMax {
		return 0, &Error{Code: CodeRateLimited, Message: "This server keeps too many reports. An admin has to delete some first."}
	}

	if err := os.MkdirAll(s.reportsDir(), 0o750); err != nil {
		return 0, internal(err)
	}
	stage := filepath.Join(s.reportsDir(), ".tmp-"+uuid.NewString())
	if err := os.Mkdir(stage, 0o750); err != nil {
		return 0, internal(err)
	}
	defer func() {
		if err != nil {
			os.RemoveAll(stage)
		}
	}()

	var stored []ReportFile
	var total int64
	seen := map[string]bool{}
	for {
		name, r, nerr := files.Next()
		if nerr == io.EOF {
			break
		}
		if nerr != nil {
			return 0, nerr
		}
		if len(stored) >= ReportMaxFiles {
			return 0, &Error{Code: CodeBadRequest, Message: "Too many attachments"}
		}
		if len(name) > reportNameMax || !reportNameRe.MatchString(name) || name == reportDescriptionFile || seen[strings.ToLower(name)] {
			return 0, &Error{Code: CodeBadRequest, Message: "Invalid or duplicate attachment name"}
		}
		seen[strings.ToLower(name)] = true
		n, werr := writeReportFile(filepath.Join(stage, name), r, ReportMaxBytes-total, reportTextExt[strings.ToLower(filepath.Ext(name))])
		if werr != nil {
			return 0, werr
		}
		total += n
		stored = append(stored, ReportFile{Name: name, Size: n})
	}

	desc = redact.RedactString(desc)
	evTitle := redact.RedactString(cleanLine(in.EventTitle, reportMetaFieldMaxLine))
	var evTime sql.NullInt64
	if in.EventTime != nil {
		evTime = sql.NullInt64{Int64: in.EventTime.Unix(), Valid: true}
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return 0, internal(err)
	}
	defer tx.Rollback()
	res, err := tx.ExecContext(ctx, `INSERT INTO reports(user_id, user_name, device_id, device_name, player_version, os, title, description,
		event_title, event_time, created_at, status, size) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)`,
		p.User.ID, p.User.DisplayName, p.Device.ID, p.Device.Name, cleanLine(in.PlayerVersion, 64), cleanLine(in.OS, 120),
		reportTitle(desc), desc, evTitle, evTime, s.Now().Unix(), reportStatusNew, total)
	if err != nil {
		return 0, internal(err)
	}
	id, err = res.LastInsertId()
	if err != nil {
		return 0, internal(err)
	}
	for _, f := range stored {
		if _, err := tx.ExecContext(ctx, `INSERT INTO report_files(report_id, name, size) VALUES (?,?,?)`, id, f.Name, f.Size); err != nil {
			return 0, internal(err)
		}
	}
	if err := os.Rename(stage, s.reportDir(id)); err != nil {
		return 0, internal(err)
	}
	if err := tx.Commit(); err != nil {
		os.RemoveAll(s.reportDir(id))
		return 0, internal(err)
	}
	s.Publish(TopicReports)
	return id, nil
}

// writeReportFile copies r to path, at most budget bytes (more: ErrPayloadTooLarge). Text types are read whole
// (the budget is the memory bound) and redacted; the returned size is what was stored.
func writeReportFile(path string, r io.Reader, budget int64, text bool) (int64, error) {
	if budget < 0 {
		budget = 0
	}
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o640)
	if err != nil {
		return 0, internal(err)
	}
	defer f.Close()
	lr := &io.LimitedReader{R: r, N: budget + 1}
	if text {
		b, err := io.ReadAll(lr)
		if err != nil {
			return 0, err
		}
		if int64(len(b)) > budget {
			return 0, tooLargeReport()
		}
		b = redact.Redact(b)
		if _, err := f.Write(b); err != nil {
			return 0, internal(err)
		}
		return int64(len(b)), nil
	}
	n, err := io.Copy(f, lr)
	if err != nil {
		return 0, err
	}
	if n > budget {
		return 0, tooLargeReport()
	}
	return n, nil
}

func tooLargeReport() error {
	return &Error{Code: CodePayloadTooLarge, Message: "A report may hold at most 25 MiB of attachments"}
}

const reportCols = `id, status, title, description, user_id, user_name, device_id, device_name, player_version, os, event_title,
	event_time, created_at, seen_at, size`

func scanReport(r scanner) (Report, error) {
	var x Report
	var evTime, created, seenAt sql.NullInt64
	if err := r.Scan(&x.ID, &x.Status, &x.Title, &x.Description, &x.UserID, &x.User, &x.DeviceID, &x.Device, &x.PlayerVersion,
		&x.OS, &x.EventTitle, &evTime, &created, &seenAt, &x.Size); err != nil {
		return x, err
	}
	x.CreatedAt = time.Unix(created.Int64, 0).UTC()
	if evTime.Valid {
		t := time.Unix(evTime.Int64, 0).UTC()
		x.EventTime = &t
	}
	if seenAt.Valid {
		t := time.Unix(seenAt.Int64, 0).UTC()
		x.SeenAt = &t
	}
	return x, nil
}

func (s *Service) loadReportFiles(ctx context.Context, rs []Report) error {
	for i := range rs {
		rows, err := s.db.QueryContext(ctx, `SELECT name, size FROM report_files WHERE report_id = ? ORDER BY name`, rs[i].ID)
		if err != nil {
			return internal(err)
		}
		for rows.Next() {
			var f ReportFile
			if err := rows.Scan(&f.Name, &f.Size); err != nil {
				rows.Close()
				return internal(err)
			}
			rs[i].Files = append(rs[i].Files, f)
			if f.Name == reportConnectionCheck {
				rs[i].HasConnectionCheck = true
			}
		}
		if err := rows.Err(); err != nil {
			rows.Close()
			return internal(err)
		}
		rows.Close()
	}
	return nil
}

// ListReports returns the reports newest first; status "" = all, otherwise ReportNew or ReportSeen. The counts
// always cover all reports.
func (s *Service) ListReports(ctx context.Context, status string) ([]Report, ReportCounts, error) {
	if status != "" && status != ReportNew && status != ReportSeen {
		return nil, ReportCounts{}, ErrBadRequest
	}
	q := `SELECT ` + reportCols + ` FROM reports`
	var args []any
	if status != "" {
		q += ` WHERE status = ?`
		args = append(args, status)
	}
	rows, err := s.db.QueryContext(ctx, q+` ORDER BY created_at DESC, id DESC`, args...)
	if err != nil {
		return nil, ReportCounts{}, internal(err)
	}
	var out []Report
	for rows.Next() {
		x, err := scanReport(rows)
		if err != nil {
			rows.Close()
			return nil, ReportCounts{}, internal(err)
		}
		out = append(out, x)
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return nil, ReportCounts{}, internal(err)
	}
	rows.Close()
	if err := s.loadReportFiles(ctx, out); err != nil {
		return nil, ReportCounts{}, err
	}
	c, err := s.ReportCounts(ctx)
	return out, c, err
}

// ReportCounts returns how many reports exist in total, new and seen.
func (s *Service) ReportCounts(ctx context.Context) (ReportCounts, error) {
	var c ReportCounts
	err := s.db.QueryRowContext(ctx, `SELECT COUNT(*), COALESCE(SUM(status = 'new'), 0), COALESCE(SUM(status = 'seen'), 0) FROM reports`).
		Scan(&c.All, &c.New, &c.Seen)
	if err != nil {
		return c, internal(err)
	}
	return c, nil
}

// NewReportCount is the number of reports not yet marked seen plus the error groups with a new event (the
// navigation badge, ADR 0035).
func (s *Service) NewReportCount(ctx context.Context) (int, error) {
	c, err := s.ReportCounts(ctx)
	if err != nil {
		return 0, err
	}
	g, err := s.NewErrorGroupCount(ctx)
	return c.New + g, err
}

// GetReport returns one report with its files. Reading does not mark it seen.
func (s *Service) GetReport(ctx context.Context, id int64) (Report, error) {
	x, err := scanReport(s.db.QueryRowContext(ctx, `SELECT `+reportCols+` FROM reports WHERE id = ?`, id))
	if errors.Is(err, sql.ErrNoRows) {
		return Report{}, ErrNotFound
	}
	if err != nil {
		return Report{}, internal(err)
	}
	rs := []Report{x}
	if err := s.loadReportFiles(ctx, rs); err != nil {
		return Report{}, err
	}
	return rs[0], nil
}

// MarkReportSeen sets the status seen (idempotent) and returns the report.
func (s *Service) MarkReportSeen(ctx context.Context, id int64) (Report, error) {
	res, err := s.db.ExecContext(ctx, `UPDATE reports SET status = 'seen', seen_at = COALESCE(seen_at, ?) WHERE id = ?`, s.Now().Unix(), id)
	if err != nil {
		return Report{}, internal(err)
	}
	if n, _ := res.RowsAffected(); n == 0 {
		return Report{}, ErrNotFound
	}
	s.Publish(TopicReports)
	return s.GetReport(ctx, id)
}

// DeleteReport removes the report and its files.
func (s *Service) DeleteReport(ctx context.Context, id int64) error {
	res, err := s.db.ExecContext(ctx, `DELETE FROM reports WHERE id = ?`, id)
	if err != nil {
		return internal(err)
	}
	if n, _ := res.RowsAffected(); n == 0 {
		return ErrNotFound
	}
	if err := os.RemoveAll(s.reportDir(id)); err != nil {
		return internal(err)
	}
	s.Publish(TopicReports)
	return nil
}

// OpenReportFile opens one attachment. The name must be one the report lists (no path is ever built from input).
func (s *Service) OpenReportFile(ctx context.Context, id int64, name string) (*os.File, int64, error) {
	var size int64
	err := s.db.QueryRowContext(ctx, `SELECT size FROM report_files WHERE report_id = ? AND name = ?`, id, name).Scan(&size)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, 0, ErrNotFound
	}
	if err != nil {
		return nil, 0, internal(err)
	}
	f, err := os.Open(filepath.Join(s.reportDir(id), name))
	if errors.Is(err, os.ErrNotExist) {
		return nil, 0, ErrNotFound
	}
	if err != nil {
		return nil, 0, internal(err)
	}
	return f, size, nil
}

// ReportText is the description file of the archive: who sent it, from where, and the text.
func ReportText(x Report) string {
	var b strings.Builder
	fmt.Fprintf(&b, "Report #%d\nSent: %s\nFrom: %s (%s)\n", x.ID, x.CreatedAt.Format(time.RFC3339), x.User, x.Device)
	if x.PlayerVersion != "" || x.OS != "" {
		fmt.Fprintf(&b, "App: %s\nSystem: %s\n", x.PlayerVersion, x.OS)
	}
	if x.EventTitle != "" {
		fmt.Fprintf(&b, "Linked to: %s", x.EventTitle)
		if x.EventTime != nil {
			fmt.Fprintf(&b, " (%s)", x.EventTime.Format(time.RFC3339))
		}
		b.WriteString("\n")
	}
	b.WriteString("\n" + x.Description + "\n")
	return b.String()
}

// WriteReportArchive streams a zip of the description (report.txt) and all attachments to w.
func (s *Service) WriteReportArchive(ctx context.Context, id int64, w io.Writer) error {
	x, err := s.GetReport(ctx, id)
	if err != nil {
		return err
	}
	zw := zip.NewWriter(w)
	hw, err := zw.CreateHeader(&zip.FileHeader{Name: reportDescriptionFile, Method: zip.Deflate, Modified: x.CreatedAt})
	if err != nil {
		return err
	}
	if _, err := io.WriteString(hw, ReportText(x)); err != nil {
		return err
	}
	for _, rf := range x.Files {
		f, _, err := s.OpenReportFile(ctx, id, rf.Name)
		if err != nil {
			return err
		}
		fw, err := zw.CreateHeader(&zip.FileHeader{Name: rf.Name, Method: zip.Deflate, Modified: x.CreatedAt})
		if err == nil {
			_, err = io.Copy(fw, f)
		}
		f.Close()
		if err != nil {
			return err
		}
	}
	return zw.Close()
}

// cleanReportStaging removes upload leftovers of a crashed server (<data>/reports/.tmp-*).
func (s *Service) cleanReportStaging() {
	entries, err := os.ReadDir(s.reportsDir())
	if err != nil {
		return
	}
	for _, e := range entries {
		if e.IsDir() && strings.HasPrefix(e.Name(), ".tmp-") {
			os.RemoveAll(filepath.Join(s.reportsDir(), e.Name()))
		}
	}
}
