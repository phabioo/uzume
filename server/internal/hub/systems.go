package hub

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"
)

// Handshake features announced by the server (additive; protocol_version unchanged). Core features: cores.go.
const (
	FeatureUsersV1    = "users_v1"
	FeatureUploadsV1  = "uploads_v1"
	FeatureFirmwareV1 = "firmware_v1"
)

// Problem codes of the core check. Both are warnings: compatible stays true.
const (
	ProblemCoreMissing         Code = "core_missing"
	ProblemCoreVersionMismatch Code = "core_version_mismatch"
)

// FirmwareMode of a system.
type FirmwareMode string

const (
	FirmwareBuiltin FirmwareMode = "builtin"
	FirmwareNative  FirmwareMode = "native"
)

// FirmwareState is the status of a firmware file.
type FirmwareState string

const (
	FirmwareValid    FirmwareState = "valid"
	FirmwareMismatch FirmwareState = "mismatch" // pinned hash differs from the stored file
	FirmwareMissing  FirmwareState = "missing"  // required (mode native) and not provided
	FirmwareOptional FirmwareState = "optional" // not provided, not required
)

// FirmwareFile is a firmware/BIOS file of a system with its current state. SHA256/Size are empty/0 when absent.
type FirmwareFile struct {
	ID          string
	DisplayName string
	Sizes       []int64
	Required    bool
	Present     bool
	Size        int64
	SHA256      string
	// Pinned is the admin-pinned expected SHA-256 (empty = none).
	Pinned string
	State  FirmwareState
	// ProvidedAt is when the admin provided the file (zero when absent).
	ProvidedAt time.Time
}

// SystemEntry is a registry entry: system, installed cores and default core, provisioning and firmware.
type SystemEntry struct {
	ID         string
	Name       string
	Extensions []string
	// LibretroIDs are the libretro system ids (core info `systemid`) that belong to the system.
	LibretroIDs []string
	// CoreID, CoreName and ExpectedCoreVersion describe the default core (ADR 0020 D5); empty when none is installed.
	// ExpectedCoreVersion is the installed (served) version of the default core.
	CoreID              string
	CoreName            string
	ExpectedCoreVersion string
	// Cores are the installed cores, ordered by core id.
	Cores          []InstalledCore
	Platforms      []string
	Provisioning   string
	InputProfile   string
	DisplayProfile string
	FirmwareMode   FirmwareMode
	Firmware       []FirmwareFile
	// AutoUpdate is the per-system override of the server default for automatic core updates: SystemAutoDefault ("" ),
	// SystemAutoOn or SystemAutoOff.
	AutoUpdate string
}

func splitList(v string) []string {
	var out []string
	for _, p := range strings.Split(v, ",") {
		if p = strings.TrimSpace(p); p != "" {
			out = append(out, p)
		}
	}
	return out
}

func (s *Service) firmwarePath(systemID, fileID string) string {
	return filepath.Join(s.dataDir, "firmware", systemID, fileID)
}

// ListRegistry returns all registry entries with their firmware files and states.
func (s *Service) ListRegistry(ctx context.Context) ([]SystemEntry, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT id, display_name, extensions, libretro_ids, COALESCE(default_core_id,''),
		platforms, provisioning, input_profile, display_profile, firmware_mode, core_auto_update FROM systems ORDER BY id`)
	if err != nil {
		return nil, internal(err)
	}
	var out []SystemEntry
	for rows.Next() {
		var e SystemEntry
		var ext, plat, libretro string
		if err := rows.Scan(&e.ID, &e.Name, &ext, &libretro, &e.CoreID, &plat, &e.Provisioning,
			&e.InputProfile, &e.DisplayProfile, &e.FirmwareMode, &e.AutoUpdate); err != nil {
			rows.Close()
			return nil, internal(err)
		}
		e.Extensions, e.Platforms, e.LibretroIDs = splitList(ext), splitList(plat), splitList(libretro)
		out = append(out, e)
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return nil, internal(err)
	}
	rows.Close()
	installed, err := s.loadInstalledCores(ctx, "")
	if err != nil {
		return nil, err
	}
	modes := make(map[string]FirmwareMode, len(out))
	for _, e := range out {
		modes[e.ID] = e.FirmwareMode
	}
	firmware, err := s.allFirmware(ctx, modes)
	if err != nil {
		return nil, err
	}
	for i := range out {
		out[i].Firmware = firmware[out[i].ID]
		out[i].Cores = installed[out[i].ID]
		for _, c := range out[i].Cores {
			if c.CoreID == out[i].CoreID {
				out[i].CoreName, out[i].ExpectedCoreVersion = c.DisplayName, c.Version
			}
		}
		if out[i].CoreName == "" { // default core id without an installed core (cannot happen with the install code)
			out[i].CoreID = ""
		}
	}
	return out, nil
}

// GetRegistryEntry returns one entry (ErrNotFound if unknown).
func (s *Service) GetRegistryEntry(ctx context.Context, systemID string) (SystemEntry, error) {
	all, err := s.ListRegistry(ctx)
	if err != nil {
		return SystemEntry{}, err
	}
	for _, e := range all {
		if e.ID == systemID {
			return e, nil
		}
	}
	return SystemEntry{}, ErrNotFound
}

// allFirmware loads the firmware files of all systems with one query, grouped by system id.
func (s *Service) allFirmware(ctx context.Context, modes map[string]FirmwareMode) (map[string][]FirmwareFile, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT d.system_id, d.file_id, d.display_name, d.sizes, COALESCE(f.sha256,''), COALESCE(f.size,0), f.sha256 IS NOT NULL,
		COALESCE(p.sha256,''), COALESCE(f.uploaded_at,0) FROM firmware_defs d
		LEFT JOIN firmware_files f ON f.system_id = d.system_id AND f.file_id = d.file_id
		LEFT JOIN firmware_pins p ON p.system_id = d.system_id AND p.file_id = d.file_id
		ORDER BY d.system_id, d.sort, d.file_id`)
	if err != nil {
		return nil, internal(err)
	}
	defer rows.Close()
	out := map[string][]FirmwareFile{}
	for rows.Next() {
		var f FirmwareFile
		var systemID, sizes string
		var at int64
		if err := rows.Scan(&systemID, &f.ID, &f.DisplayName, &sizes, &f.SHA256, &f.Size, &f.Present, &f.Pinned, &at); err != nil {
			return nil, internal(err)
		}
		if at > 0 {
			f.ProvidedAt = time.Unix(at, 0).UTC()
		}
		for _, p := range splitList(sizes) {
			if n, err := strconv.ParseInt(p, 10, 64); err == nil {
				f.Sizes = append(f.Sizes, n)
			}
		}
		f.Required = modes[systemID] == FirmwareNative
		switch {
		case !f.Present && f.Required:
			f.State = FirmwareMissing
		case !f.Present:
			f.State = FirmwareOptional
		case f.Pinned != "" && f.Pinned != f.SHA256:
			f.State = FirmwareMismatch
		default:
			f.State = FirmwareValid
		}
		out[systemID] = append(out[systemID], f)
	}
	return out, rows.Err()
}

// FirmwareProblems counts required firmware files (mode native) that are missing or mismatching (nav badge).
func (s *Service) FirmwareProblems(ctx context.Context) (int, error) {
	reg, err := s.ListRegistry(ctx)
	if err != nil {
		return 0, err
	}
	n := 0
	for _, e := range reg {
		for _, f := range e.Firmware {
			if f.Required && (f.State == FirmwareMissing || f.State == FirmwareMismatch) {
				n++
			}
		}
	}
	return n, nil
}

// SetFirmwareMode switches a system between builtin and native firmware.
func (s *Service) SetFirmwareMode(ctx context.Context, systemID string, mode FirmwareMode) (err error) {
	defer s.publishOK(&err, TopicSystems)
	if mode != FirmwareBuiltin && mode != FirmwareNative {
		return badRequest("Firmware mode must be builtin or native")
	}
	res, err := s.db.ExecContext(ctx, `UPDATE systems SET firmware_mode = ? WHERE id = ?`, string(mode), systemID)
	if err != nil {
		return internal(err)
	}
	if n, _ := res.RowsAffected(); n == 0 {
		return ErrNotFound
	}
	return nil
}

func (s *Service) firmwareDef(ctx context.Context, systemID, fileID string) (FirmwareFile, error) {
	e, err := s.GetRegistryEntry(ctx, systemID)
	if err != nil {
		return FirmwareFile{}, err
	}
	for _, f := range e.Firmware {
		if f.ID == fileID {
			return f, nil
		}
	}
	return FirmwareFile{}, ErrNotFound
}

// ProvideFirmware stores (or replaces) a firmware file from r. The size must match the file definition and, if
// the admin pinned an expected SHA-256, the content must match it. Nothing is stored on failure. The file is
// kept at <data dir>/firmware/<system>/<file_id> (0600); the bytes are never logged.
func (s *Service) ProvideFirmware(ctx context.Context, systemID, fileID string, r io.Reader) (_ FirmwareFile, err error) {
	defer s.publishOK(&err, TopicSystems)
	def, err := s.firmwareDef(ctx, systemID, fileID)
	if err != nil {
		return FirmwareFile{}, err
	}
	var max int64
	for _, n := range def.Sizes {
		if n > max {
			max = n
		}
	}
	dir := filepath.Join(s.dataDir, "firmware", systemID)
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return FirmwareFile{}, internal(err)
	}
	tmp, err := os.CreateTemp(filepath.Join(s.dataDir, "tmp"), "fw-*")
	if err != nil {
		return FirmwareFile{}, internal(err)
	}
	tmpName := tmp.Name()
	defer os.Remove(tmpName)
	h := sha256.New()
	// Read at most one byte more than the largest allowed size: anything bigger is rejected without buffering it.
	size, err := io.Copy(io.MultiWriter(tmp, h), io.LimitReader(r, max+1))
	if err == nil {
		err = tmp.Sync()
	}
	if cerr := tmp.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		return FirmwareFile{}, internal(err)
	}
	ok := false
	for _, n := range def.Sizes {
		ok = ok || n == size
	}
	if !ok {
		got := strconv.FormatInt(size, 10)
		if size > max {
			got = "more than " + strconv.FormatInt(max, 10)
		}
		return FirmwareFile{}, badRequest("%s must be %s bytes (got %s)", def.DisplayName, sizeList(def.Sizes), got)
	}
	sha := hex.EncodeToString(h.Sum(nil))
	if def.Pinned != "" && def.Pinned != sha {
		return FirmwareFile{}, badRequest("%s does not match the pinned SHA-256", def.DisplayName)
	}
	if err := os.Chmod(tmpName, 0o600); err != nil {
		return FirmwareFile{}, internal(err)
	}
	// Keep the old file as a backup until the metadata is updated, so file and DB never diverge.
	s.fwMu.Lock()
	defer s.fwMu.Unlock()
	dst := s.firmwarePath(systemID, fileID)
	bak := dst + ".bak"
	hadOld := os.Rename(dst, bak) == nil
	if err := os.Rename(tmpName, dst); err != nil {
		if hadOld {
			os.Rename(bak, dst)
		}
		return FirmwareFile{}, internal(err)
	}
	if _, err := s.db.ExecContext(ctx, `INSERT INTO firmware_files(system_id, file_id, sha256, size, uploaded_at) VALUES (?,?,?,?,?)
		ON CONFLICT(system_id, file_id) DO UPDATE SET sha256 = excluded.sha256, size = excluded.size, uploaded_at = excluded.uploaded_at`,
		systemID, fileID, sha, size, s.now().Unix()); err != nil {
		if hadOld {
			os.Rename(bak, dst)
		} else {
			os.Remove(dst)
		}
		return FirmwareFile{}, internal(err)
	}
	syncDir(filepath.Dir(dst))
	if hadOld {
		os.Remove(bak)
	}
	return s.firmwareDef(ctx, systemID, fileID)
}

func sizeList(sizes []int64) string {
	parts := make([]string, len(sizes))
	for i, n := range sizes {
		parts[i] = strconv.FormatInt(n, 10)
	}
	return strings.Join(parts, " or ")
}

// RemoveFirmware deletes a provided file (a pinned hash stays). ErrNotFound if it was not provided.
func (s *Service) RemoveFirmware(ctx context.Context, systemID, fileID string) (err error) {
	defer s.publishOK(&err, TopicSystems)
	res, err := s.db.ExecContext(ctx, `DELETE FROM firmware_files WHERE system_id = ? AND file_id = ?`, systemID, fileID)
	if err != nil {
		return internal(err)
	}
	if n, _ := res.RowsAffected(); n == 0 {
		return ErrNotFound
	}
	if err := os.Remove(s.firmwarePath(systemID, fileID)); err != nil && !errors.Is(err, os.ErrNotExist) {
		slog.Warn("remove firmware: delete file failed", "system", systemID, "file", fileID, "err", err)
	}
	return nil
}

// SetFirmwarePin sets (64 lowercase hex characters) or clears (empty) the expected SHA-256 of a file.
func (s *Service) SetFirmwarePin(ctx context.Context, systemID, fileID, sha string) (err error) {
	defer s.publishOK(&err, TopicSystems)
	sha = strings.ToLower(strings.TrimSpace(sha))
	if _, err := s.firmwareDef(ctx, systemID, fileID); err != nil {
		return err
	}
	if sha == "" {
		_, err := s.db.ExecContext(ctx, `DELETE FROM firmware_pins WHERE system_id = ? AND file_id = ?`, systemID, fileID)
		return internal2(err)
	}
	if !ValidSHA256(sha) {
		return badRequest("SHA-256 must be 64 hexadecimal characters")
	}
	_, err = s.db.ExecContext(ctx, `INSERT INTO firmware_pins(system_id, file_id, sha256) VALUES (?,?,?)
		ON CONFLICT(system_id, file_id) DO UPDATE SET sha256 = excluded.sha256`, systemID, fileID, sha)
	return internal2(err)
}

var idRe = regexp.MustCompile(`^[a-z0-9_-]{1,32}$`)

// OpenFirmware opens a provided firmware file for download; the caller closes it. ErrNotFound if unknown or absent.
func (s *Service) OpenFirmware(ctx context.Context, systemID, fileID string) (*os.File, FirmwareFile, error) {
	if !idRe.MatchString(systemID) || !idRe.MatchString(fileID) {
		return nil, FirmwareFile{}, ErrNotFound
	}
	def, err := s.firmwareDef(ctx, systemID, fileID)
	if err != nil {
		return nil, FirmwareFile{}, err
	}
	// A file that does not match the admin-pinned hash is not offered to apps.
	if !def.Present || def.State == FirmwareMismatch {
		return nil, FirmwareFile{}, ErrNotFound
	}
	f, err := os.Open(s.firmwarePath(systemID, fileID))
	if errors.Is(err, os.ErrNotExist) {
		return nil, FirmwareFile{}, ErrNotFound
	}
	if err != nil {
		return nil, FirmwareFile{}, internal(err)
	}
	return f, def, nil
}

// ---- Core reports and the handshake core check ----

// CoreReport is a core reported by an app (id and version string).
type CoreReport struct {
	ID      string `json:"id"`
	Version string `json:"version"`
}

// canonCoreID maps a core id to its canonical libretro id (legacy aliases resolved, ADR 0020 D3).
func canonCoreID(id string) string {
	if a, ok := legacyAliases[id]; ok {
		return a
	}
	return id
}

// matchReportedCore finds the reported core that serves the system: the default core first, else any other installed
// core of the system (the app picks among the served cores). Ids are compared canonically on both sides. It returns
// the report and the version the server serves for that core.
func matchReportedCore(e SystemEntry, cores []CoreReport) (got *CoreReport, served string) {
	find := func(id string) *CoreReport {
		for i := range cores {
			if canonCoreID(cores[i].ID) == canonCoreID(id) {
				return &cores[i]
			}
		}
		return nil
	}
	if got = find(e.CoreID); got != nil {
		return got, e.ExpectedCoreVersion
	}
	for _, c := range e.Cores {
		if got = find(c.CoreID); got != nil {
			return got, c.Version
		}
	}
	return nil, ""
}

// checkCores compares the reported cores with the registry; both problems are warnings. A system is fine when the
// App reports any core the server serves for it (default or not).
func checkCores(reg []SystemEntry, cores []CoreReport) []Problem {
	var out []Problem
	for _, e := range reg {
		if e.CoreID == "" {
			continue // no core installed for the system: nothing to compare
		}
		got, served := matchReportedCore(e, cores)
		switch {
		case got == nil:
			out = append(out, Problem{Code: ProblemCoreMissing, CoreID: e.CoreID,
				Detail: "Core " + e.CoreID + " (" + e.Name + ") is not installed"})
		case served != "" && got.Version != served:
			out = append(out, Problem{Code: ProblemCoreVersionMismatch, CoreID: e.CoreID,
				Detail: "Core " + got.ID + " is version " + got.Version + ", " + served + " expected"})
		}
	}
	return out
}

func (s *Service) storeReport(ctx context.Context, deviceID string, in HandshakeInput, cores []CoreReport) error {
	if cores == nil {
		cores = []CoreReport{}
	}
	js, err := json.Marshal(cores)
	if err != nil {
		return internal(err)
	}
	_, err = s.db.ExecContext(ctx, `INSERT INTO device_reports(device_id, platform, arch, player_version, protocol_version, cores, reported_at)
		VALUES (?,?,?,?,?,?,?) ON CONFLICT(device_id) DO UPDATE SET platform = excluded.platform, arch = excluded.arch,
		player_version = excluded.player_version, protocol_version = excluded.protocol_version, cores = excluded.cores,
		reported_at = excluded.reported_at`,
		deviceID, in.Platform, in.Arch, in.PlayerVersion, in.ProtocolVersion, string(js), s.now().Unix())
	return internal2(err)
}

// ClientStatus is the compatibility status of a reporting device for one system.
type ClientStatus string

const (
	ClientCompatible   ClientStatus = "compatible"
	ClientCoreMismatch ClientStatus = "core_version_mismatch"
	ClientCoreMissing  ClientStatus = "core_missing"
	ClientPlayerTooOld ClientStatus = "player_too_old"
)

// ClientReport is the last handshake report of a trusted device, evaluated for one system.
type ClientReport struct {
	DeviceID        string
	DeviceName      string
	Platform        string
	Arch            string
	PlayerVersion   string
	ProtocolVersion int
	CoreVersion     string // version of the system's preferred core; empty if not installed
	Status          ClientStatus
	// Expected is the expected core version (mismatch) or the minimum protocol version (player too old).
	Expected   string
	ReportedAt time.Time
}

// DeviceReports are the stored handshake reports of all trusted devices, loaded once and evaluated per system
// with ClientReports.
type DeviceReports struct {
	rows     []deviceReportRow
	minProto int
}

type deviceReportRow struct {
	ClientReport
	cores []CoreReport
}

// LoadDeviceReports reads the last report of each trusted device (one query, one decode per device).
func (s *Service) LoadDeviceReports(ctx context.Context) (DeviceReports, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT d.id, d.name, r.platform, r.arch, r.player_version, r.protocol_version, r.cores, r.reported_at
		FROM device_reports r JOIN devices d ON d.id = r.device_id WHERE d.status = 'trusted' ORDER BY d.name COLLATE NOCASE, d.id`)
	if err != nil {
		return DeviceReports{}, internal(err)
	}
	defer rows.Close()
	out := DeviceReports{minProto: s.Info().MinProtocolVersion}
	for rows.Next() {
		var c deviceReportRow
		var cores string
		var at int64
		if err := rows.Scan(&c.DeviceID, &c.DeviceName, &c.Platform, &c.Arch, &c.PlayerVersion, &c.ProtocolVersion, &cores, &at); err != nil {
			return DeviceReports{}, internal(err)
		}
		c.ReportedAt = time.Unix(at, 0).UTC()
		_ = json.Unmarshal([]byte(cores), &c.cores)
		out.rows = append(out.rows, c)
	}
	return out, rows.Err()
}

// ClientReports evaluates the loaded reports against one system.
func (d DeviceReports) ClientReports(e SystemEntry) []ClientReport {
	var out []ClientReport
	for _, r := range d.rows {
		c := r.ClientReport
		served := e.ExpectedCoreVersion
		if got, sv := matchReportedCore(e, r.cores); got != nil && e.CoreID != "" {
			c.CoreVersion, served = got.Version, sv
		}
		switch {
		case c.ProtocolVersion < d.minProto:
			c.Status, c.Expected = ClientPlayerTooOld, strconv.Itoa(d.minProto)
		case c.CoreVersion == "":
			c.Status = ClientCoreMissing
		case served != "" && c.CoreVersion != served:
			c.Status, c.Expected = ClientCoreMismatch, served
		default:
			c.Status = ClientCompatible
		}
		out = append(out, c)
	}
	return out
}

// ListClientReports returns the last report of each trusted device, evaluated against the system.
func (s *Service) ListClientReports(ctx context.Context, e SystemEntry) ([]ClientReport, error) {
	d, err := s.LoadDeviceReports(ctx)
	if err != nil {
		return nil, err
	}
	return d.ClientReports(e), nil
}
