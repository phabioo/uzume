package httpapi

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"net/url"
	"strings"
	"time"

	"github.com/phabioo/uzume/server/internal/adminhost"
	"github.com/phabioo/uzume/server/internal/api"
	"github.com/phabioo/uzume/server/internal/config"
	"github.com/phabioo/uzume/server/internal/diag"
	"github.com/phabioo/uzume/server/internal/hub"
)

// Admin API for the settings (0.18, `admin_v1`): server name, uploads, network, external TURN servers, updates,
// certificate and diagnostics bundle. The web theme, the admin password and the backups / metrics settings are
// not part of it. Validation and apply code are the app's (adminhost.Host); no secret is ever returned.

// hostError turns the refusals of the host into API errors.
func hostError(err error) error {
	var ve *adminhost.ValidationError
	switch {
	case errors.As(err, &ve):
		return &hub.Error{Code: hub.CodeBadRequest, Message: ve.Msg}
	case errors.Is(err, adminhost.ErrUnsupported):
		return &hub.Error{Code: hub.CodeBadRequest, Message: err.Error()}
	}
	return err
}

func (s *Server) adminNetwork(ctx context.Context, h adminhost.Host) (api.AdminNetwork, error) {
	st, err := h.NetState(ctx)
	if err != nil {
		return api.AdminNetwork{}, err
	}
	pending := map[string]bool{}
	for _, p := range st.Pending {
		pending[p] = true
	}
	out := api.AdminNetwork{CanRestart: st.CanRestart, Issues: append([]string{}, st.Issues...), PendingRestart: append([]string{}, st.Pending...),
		Settings: make([]api.AdminNetSetting, 0, len(config.NetKeys)), CloudflareConfigured: s.svc.CloudflareStatus().Configured}
	desired, running, base := st.Desired, st.Running, st.Base
	for _, k := range config.NetKeys {
		_, saved := st.Stored[k]
		src := api.AdminNetSettingSourceEnv
		if saved {
			src = api.AdminNetSettingSourceWeb
		}
		out.Settings = append(out.Settings, api.AdminNetSetting{Key: k, Label: config.NetLabel(k), Value: desired.NetValue(k),
			RunningValue: running.NetValue(k), DefaultValue: base.NetValue(k), Source: src, NeedsRestart: config.NetNeedsRestart(k),
			RestartPending: pending[config.NetLabel(k)]})
	}
	if st.TURN != nil {
		t := api.AdminTurn{PublicHost: st.TURN.PublicHost, Port: st.TURN.Port, RelayMin: st.TURN.RelayMin, RelayMax: st.TURN.RelayMax,
			Allocations: st.TURN.Allocations, Fixed: st.TURN.Fixed, Forwards: append([]string{}, st.TURNForwards...)}
		if st.TURN.RelayIP != "" {
			t.RelayIp = ptr(st.TURN.RelayIP)
		}
		out.Turn = &t
	}
	out.ExternalTurn = s.adminExternalTurn()
	return out, nil
}

// adminExternalTurn lists the external TURN servers without any secret.
func (s *Server) adminExternalTurn() api.AdminExternalTurn {
	src := api.AdminExternalTurnSourceFlags
	if s.svc.ExternalTURNSource() == hub.ExternalTURNFromWeb {
		src = api.AdminExternalTurnSourceWeb
	}
	out := api.AdminExternalTurn{Source: src, Active: s.svc.ExternalTURNActive(), Servers: []api.AdminExternalTurnServer{}}
	for _, e := range s.svc.ExternalTURNStatus() {
		o := api.AdminExternalTurnServer{Name: e.Name, Mode: api.AdminExternalTurnServerMode(e.Mode), Urls: append([]string{}, e.URLs...), SecretSet: e.SecretSet}
		if e.Username != "" {
			o.Username = ptr(e.Username)
		}
		out.Servers = append(out.Servers, o)
	}
	return out
}

func apiUpdateSettings(st hub.UpdateStatus) api.AdminUpdateSettings {
	out := api.AdminUpdateSettings{Channel: api.AdminUpdateSettingsChannel(st.Settings.Channel), ChannelIsSet: st.Settings.ChannelIsSet,
		Auto: st.Settings.Auto, CompiledChannel: st.Settings.CompiledChannel, DefaultChannel: st.Settings.DefaultChannel,
		CurrentVersion: st.CurrentVersion, Packaged: st.Packaged, InContainer: st.InContainer, RequestPending: st.RequestPending,
		ActiveSessions: st.ActiveSessions, LastCheck: st.LastCheck, Paused: apiPause(st.Paused)}
	if st.Disabled != "" {
		out.DisabledReason = ptr(st.Disabled)
	}
	if st.LastError != "" {
		out.LastError = ptr(st.LastError)
	}
	if st.ManualCommand != "" {
		out.ManualCommand = ptr(st.ManualCommand)
	}
	if st.Skipped != "" {
		out.SkippedVersion = ptr(st.Skipped)
	}
	if st.Staged != nil {
		out.StagedVersion = ptr(st.Staged.Version)
	}
	if r := st.LastResult; r != nil {
		out.LastResult = &api.AdminUpdateResult{Version: r.Version, Time: r.Time, Ok: r.OK, Message: r.Message}
	}
	if a := st.Available; a != nil {
		av := &api.AdminUpdateAvailable{Version: a.Version, Channel: a.Channel, PublishedAt: a.PublishedAt, Size: a.Size,
			Breaking: a.Breaking, BreakingCount: a.BreakingCount, Skipped: a.Skipped}
		if a.NotesURL != "" {
			av.NotesUrl = ptr(a.NotesURL)
		}
		out.Available = av
	}
	return out
}

func apiSecurity(sec adminhost.Security) api.AdminSecurity {
	out := api.AdminSecurity{Tls: sec.TLS, CanRenewCert: sec.CanRenew, TrustedProxy: append([]string{}, sec.TrustedProxy...)}
	if sec.Fingerprint != "" {
		out.Fingerprint = ptr(sec.Fingerprint)
	}
	if sec.CertSource != "" {
		out.CertSource = ptr(sec.CertSource)
	}
	if !sec.NotAfter.IsZero() {
		out.CertNotAfter = ptr(sec.NotAfter)
	}
	return out
}

func (s *Server) adminSettings(ctx context.Context, h adminhost.Host) (api.AdminSettings, error) {
	nw, err := s.adminNetwork(ctx, h)
	if err != nil {
		return api.AdminSettings{}, err
	}
	allow, err := s.svc.AllowUserUploads(ctx)
	if err != nil {
		return api.AdminSettings{}, err
	}
	up, err := s.svc.UpdateStatus(ctx)
	if err != nil {
		return api.AdminSettings{}, err
	}
	quota, err := s.svc.StateQuota(ctx)
	if err != nil {
		return api.AdminSettings{}, err
	}
	return api.AdminSettings{HubName: s.svc.Info().Name, AllowUserUploads: allow, StateQuotaBytes: quota, Network: nw, Security: apiSecurity(h.Security()),
		Updates: apiUpdateSettings(up)}, nil
}

func (s *Server) GetAdminSettings(ctx context.Context, _ api.GetAdminSettingsRequestObject) (api.GetAdminSettingsResponseObject, error) {
	h, err := s.adminHost(ctx)
	if err != nil {
		return nil, err
	}
	out, err := s.adminSettings(ctx, h)
	if err != nil {
		return nil, err
	}
	return api.GetAdminSettings200JSONResponse(out), nil
}

// UpdateAdminSettings changes the given fields. The values are checked first, so a refused value changes nothing.
func (s *Server) UpdateAdminSettings(ctx context.Context, req api.UpdateAdminSettingsRequestObject) (api.UpdateAdminSettingsResponseObject, error) {
	h, err := s.adminHost(ctx)
	if err != nil {
		return nil, err
	}
	if req.Body == nil {
		return nil, hub.ErrBadRequest
	}
	b := req.Body
	if b.Name != nil {
		if n := strings.TrimSpace(*b.Name); n == "" || len(n) > 100 {
			return nil, &hub.Error{Code: hub.CodeBadRequest, Message: "Name must be 1 to 100 characters long"}
		}
	}
	if b.StateQuotaBytes != nil && *b.StateQuotaBytes < hub.MinStateQuotaBytes {
		return nil, &hub.Error{Code: hub.CodeBadRequest, Message: "The save state quota must be at least 65 MiB"}
	}
	if b.UpdateChannel != nil && *b.UpdateChannel != "" && *b.UpdateChannel != api.AdminSettingsUpdateUpdateChannelStable && *b.UpdateChannel != api.AdminSettingsUpdateUpdateChannelBeta {
		return nil, &hub.Error{Code: hub.CodeBadRequest, Message: "Update channel must be stable or beta"}
	}
	if b.Name != nil {
		if err := s.svc.SetHubName(ctx, *b.Name); err != nil {
			return nil, err
		}
	}
	if b.AllowUserUploads != nil {
		if err := s.svc.SetAllowUserUploads(ctx, *b.AllowUserUploads); err != nil {
			return nil, err
		}
	}
	if b.StateQuotaBytes != nil {
		if err := s.svc.SetStateQuota(ctx, *b.StateQuotaBytes); err != nil {
			return nil, err
		}
	}
	if b.UpdateChannel != nil || b.UpdateAuto != nil {
		// The hub stores both together: a field that is not sent keeps its effective value.
		cur, err := s.svc.UpdateSettings(ctx)
		if err != nil {
			return nil, err
		}
		channel, auto := "", cur.Auto
		if cur.ChannelIsSet {
			channel = cur.Channel
		}
		if b.UpdateChannel != nil {
			channel = string(*b.UpdateChannel)
		}
		if b.UpdateAuto != nil {
			auto = *b.UpdateAuto
		}
		if err := s.svc.SetUpdateSettings(ctx, channel, auto); err != nil {
			return nil, err
		}
	}
	out, err := s.adminSettings(ctx, h)
	if err != nil {
		return nil, err
	}
	return api.UpdateAdminSettings200JSONResponse(out), nil
}

// netForm turns the body into the fields of the form fields.
func netForm(b api.AdminNetSettingInput) url.Values {
	f := url.Values{}
	set := func(k string, v *string) {
		if v != nil {
			f.Set(k, *v)
		}
	}
	set("value", b.Value)
	set("min", b.Min)
	set("max", b.Max)
	set("url", b.Url)
	if b.Op != nil {
		f.Set("op", string(*b.Op))
	}
	return f
}

func (s *Server) SetAdminNetSetting(ctx context.Context, req api.SetAdminNetSettingRequestObject) (api.SetAdminNetSettingResponseObject, error) {
	h, err := s.adminHost(ctx)
	if err != nil {
		return nil, err
	}
	if err := s.requireConfirmed(ctx); err != nil {
		return nil, err
	}
	if req.Body == nil {
		return nil, hub.ErrBadRequest
	}
	if err := h.SaveNet(ctx, req.Key, netForm(*req.Body)); err != nil {
		return nil, hostError(err)
	}
	s.log.Info("network setting saved from the admin API", "key", req.Key, "admin", principal(ctx).User.Username)
	out, err := s.adminNetwork(ctx, h)
	if err != nil {
		return nil, err
	}
	return api.SetAdminNetSetting200JSONResponse(out), nil
}

func (s *Server) ResetAdminNetSetting(ctx context.Context, req api.ResetAdminNetSettingRequestObject) (api.ResetAdminNetSettingResponseObject, error) {
	h, err := s.adminHost(ctx)
	if err != nil {
		return nil, err
	}
	if err := s.requireConfirmed(ctx); err != nil {
		return nil, err
	}
	if err := h.ResetNet(ctx, req.Key); err != nil {
		return nil, hostError(err)
	}
	s.log.Info("network setting reset from the admin API", "key", req.Key, "admin", principal(ctx).User.Username)
	out, err := s.adminNetwork(ctx, h)
	if err != nil {
		return nil, err
	}
	return api.ResetAdminNetSetting200JSONResponse(out), nil
}

// restartResponse writes the answer first and then asks the process to restart.
type restartResponse struct {
	body    api.AdminRestart
	restart func()
}

func (r restartResponse) VisitRestartAdminServerResponse(w http.ResponseWriter) error {
	b, err := json.Marshal(r.body)
	if err != nil {
		return err
	}
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusAccepted)
	_, err = w.Write(append(b, '\n'))
	if f, ok := w.(http.Flusher); ok {
		f.Flush()
	}
	r.restart()
	return err
}

func (s *Server) RestartAdminServer(ctx context.Context, _ api.RestartAdminServerRequestObject) (api.RestartAdminServerResponseObject, error) {
	h, err := s.adminHost(ctx)
	if err != nil {
		return nil, err
	}
	rb, restart, err := h.PrepareRestart(ctx, principal(ctx).User.Username)
	if err != nil {
		return nil, hostError(err)
	}
	body := api.AdminRestart{PortChanged: rb.PortChanged}
	if rb.URL != "" {
		body.Url = ptr(strings.TrimSuffix(rb.URL, "/settings/network"))
	}
	return restartResponse{body: body, restart: restart}, nil
}

func (s *Server) SaveAdminExternalTurn(ctx context.Context, req api.SaveAdminExternalTurnRequestObject) (api.SaveAdminExternalTurnResponseObject, error) {
	if err := requireAdmin(ctx); err != nil {
		return nil, err
	}
	if err := s.requireConfirmed(ctx); err != nil {
		return nil, err
	}
	if req.Body == nil {
		return nil, hub.ErrBadRequest
	}
	b := req.Body
	in := hub.ExternalTURNInput{Name: b.Name, Mode: string(b.Mode), URLs: b.Urls}
	if b.Original != nil {
		in.Original = strings.TrimSpace(*b.Original)
	}
	if b.Username != nil {
		in.Username = *b.Username
	}
	if b.Secret != nil {
		in.Secret = *b.Secret
	}
	in.ClearSecret = b.ClearSecret != nil && *b.ClearSecret
	if err := s.svc.SaveExternalTURN(ctx, in); err != nil {
		return nil, err
	}
	s.log.Info("external TURN server saved from the admin API", "name", strings.TrimSpace(in.Name), "admin", principal(ctx).User.Username)
	return api.SaveAdminExternalTurn200JSONResponse(s.adminExternalTurn()), nil
}

func (s *Server) DeleteAdminExternalTurn(ctx context.Context, req api.DeleteAdminExternalTurnRequestObject) (api.DeleteAdminExternalTurnResponseObject, error) {
	if err := requireAdmin(ctx); err != nil {
		return nil, err
	}
	if err := s.requireConfirmed(ctx); err != nil {
		return nil, err
	}
	if err := s.svc.DeleteExternalTURN(ctx, req.Params.Name); err != nil {
		return nil, err
	}
	s.log.Info("external TURN server removed from the admin API", "name", req.Params.Name, "admin", principal(ctx).User.Username)
	return api.DeleteAdminExternalTurn200JSONResponse(s.adminExternalTurn()), nil
}

func (s *Server) ResetAdminExternalTurn(ctx context.Context, _ api.ResetAdminExternalTurnRequestObject) (api.ResetAdminExternalTurnResponseObject, error) {
	if err := requireAdmin(ctx); err != nil {
		return nil, err
	}
	if err := s.requireConfirmed(ctx); err != nil {
		return nil, err
	}
	if err := s.svc.ResetExternalTURN(ctx); err != nil {
		return nil, err
	}
	return api.ResetAdminExternalTurn200JSONResponse(s.adminExternalTurn()), nil
}

func (s *Server) CheckAdminUpdates(ctx context.Context, _ api.CheckAdminUpdatesRequestObject) (api.CheckAdminUpdatesResponseObject, error) {
	if err := requireAdmin(ctx); err != nil {
		return nil, err
	}
	s.svc.TriggerUpdateCheck()
	return api.CheckAdminUpdates202Response{}, nil
}

func (s *Server) InstallAdminUpdate(ctx context.Context, req api.InstallAdminUpdateRequestObject) (api.InstallAdminUpdateResponseObject, error) {
	if err := requireAdmin(ctx); err != nil {
		return nil, err
	}
	if err := s.requireConfirmed(ctx); err != nil {
		return nil, err
	}
	confirm := req.Body != nil && req.Body.ConfirmBreaking != nil && *req.Body.ConfirmBreaking
	st, err := s.svc.InstallUpdate(ctx, confirm)
	switch {
	case err == nil:
	case errors.Is(err, hub.ErrUpdateBreaking):
		return nil, &hub.Error{Code: hub.CodeConflict, Message: err.Error()}
	case errors.Is(err, hub.ErrNotPackaged), errors.Is(err, hub.ErrNoUpdate), errors.Is(err, hub.ErrUpdatesOff):
		return nil, &hub.Error{Code: hub.CodeBadRequest, Message: err.Error()}
	default:
		return nil, err
	}
	s.log.Warn("update install requested from the admin API", "version", st.Version, "admin", principal(ctx).User.Username)
	return api.InstallAdminUpdate202JSONResponse{Version: st.Version}, nil
}

func (s *Server) RenewAdminCertificate(ctx context.Context, _ api.RenewAdminCertificateRequestObject) (api.RenewAdminCertificateResponseObject, error) {
	h, err := s.adminHost(ctx)
	if err != nil {
		return nil, err
	}
	fp, notAfter, err := h.RenewCert(principal(ctx).User.Username)
	if err != nil {
		return nil, hostError(err)
	}
	return api.RenewAdminCertificate200JSONResponse{Fingerprint: fp, NotAfter: notAfter}, nil
}

// diagBundleResponse streams the zip like the app; once it has started a failure can only truncate it.
type diagBundleResponse struct {
	ctx  context.Context
	info diag.Info
	opt  diag.Options
	name string
	log  func(error)
}

func (d diagBundleResponse) VisitCreateAdminDiagnosticsBundleResponse(w http.ResponseWriter) error {
	h := w.Header()
	h.Set("Content-Type", "application/zip")
	h.Set("Content-Disposition", `attachment; filename="`+d.name+`"`)
	w.WriteHeader(http.StatusOK)
	if err := diag.Write(d.ctx, w, d.info, d.opt); err != nil {
		d.log(err)
	}
	return nil
}

func (s *Server) CreateAdminDiagnosticsBundle(ctx context.Context, _ api.CreateAdminDiagnosticsBundleRequestObject) (api.CreateAdminDiagnosticsBundleResponseObject, error) {
	h, err := s.adminHost(ctx)
	if err != nil {
		return nil, err
	}
	o := diag.Options{Config: h.RunningConfig()}
	info, err := diag.Collect(ctx, s.svc, o)
	if err != nil {
		return nil, err
	}
	name := diag.FileName(time.Now())
	s.log.Info("diagnostics bundle downloaded", "file", name, "admin", principal(ctx).User.Username)
	return diagBundleResponse{ctx: ctx, info: info, opt: o, name: name,
		log: func(err error) { s.log.Warn("diagnostics bundle aborted", "err", err) }}, nil
}
