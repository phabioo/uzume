// Package httpapi implements the HTTP API of the Uzume server (OpenAPI spec in protocol/openapi)
// on top of the service layer internal/hub.
package httpapi

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net"
	"net/http"
	"runtime/debug"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/google/uuid"

	"github.com/phabioo/uzume/server/internal/adminhost"
	"github.com/phabioo/uzume/server/internal/api"
	"github.com/phabioo/uzume/server/internal/hub"
	"github.com/phabioo/uzume/server/internal/pluginpkg"
	"github.com/phabioo/uzume/server/internal/statusinfo"
)

const maxBodyBytes = 1 << 20

type ctxKey int

const (
	keyBearer ctxKey = iota
	keyPrincipal
	keyRemoteIP
	keyHost
)

var errNotImplemented = errors.New("httpapi: not implemented")

// Server implements api.StrictServerInterface.
type Server struct {
	svc     *hub.Service
	log     *slog.Logger
	restart func()
	status  *statusinfo.Source
	host    adminhost.Host // process-level facts for the admin settings API (nil in tests that do not need it)
}

// Register attaches the API routes (including the info endpoint and ROM download) to mux. The server has no
// other routes: everything else answers 404.
func Register(mux *http.ServeMux, svc *hub.Service, log *slog.Logger, opts ...Option) {
	if log == nil {
		log = slog.Default()
	}
	s := &Server{svc: svc, log: log}
	for _, o := range opts {
		o(s)
	}
	strict := api.NewStrictHandlerWithOptions(s, []api.StrictMiddlewareFunc{s.authMiddleware}, api.StrictHTTPServerOptions{
		RequestErrorHandlerFunc: func(w http.ResponseWriter, _ *http.Request, err error) {
			var mbe *http.MaxBytesError
			if errors.As(err, &mbe) {
				writeError(w, http.StatusRequestEntityTooLarge, hub.CodePayloadTooLarge, "Request too large")
				return
			}
			writeError(w, http.StatusBadRequest, hub.CodeBadRequest, "Invalid request")
		},
		ResponseErrorHandlerFunc: func(w http.ResponseWriter, r *http.Request, err error) { s.writeErr(w, r, err) },
	})
	api.HandlerWithOptions(strict, api.StdHTTPServerOptions{
		BaseRouter: skipRomMux{mux},
		Middlewares: []api.MiddlewareFunc{func(next http.Handler) http.Handler {
			return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				w.Header().Set("Cache-Control", "no-store")
				limit := int64(maxBodyBytes)
				if (r.Method == http.MethodPut && strings.Contains(r.URL.Path, "/saves/")) ||
					(r.Method == http.MethodPost && strings.Contains(r.URL.Path, "/saves/") && strings.HasSuffix(r.URL.Path, "/upload")) {
					limit = hub.MaxSaveBytes + 1 // the service answers 413 payload_too_large
				}
				if r.Method == http.MethodPost && strings.HasPrefix(r.URL.Path, "/api/v1/games/") && strings.HasSuffix(r.URL.Path, "/screenshots") {
					limit = hub.MaxScreenshotBytes + 1 // the service answers 413 payload_too_large
				}
				if r.Method == http.MethodPut && strings.HasPrefix(r.URL.Path, "/api/v1/games/") && strings.Contains(r.URL.Path, "/states/") {
					limit = hub.MaxStateBytes + hub.MaxStatePreviewBytes + 1<<20 // state, preview and the multipart framing; the service answers 413
				}
				if r.Method == http.MethodPost && r.URL.Path == "/api/v1/reports" {
					limit = hub.ReportMaxBytes + 1<<20 // attachments plus the multipart framing and meta
				}
				if r.Method == http.MethodPost && r.URL.Path == "/api/v1/admin/plugins/stage" {
					limit = pluginpkg.MaxCompressedBytes + 1 // the service answers 413 payload_too_large
				}
				if r.Method == http.MethodPost && r.URL.Path == "/api/v1/games" {
					limit = hub.MaxROMBytes // streamed to disk, never buffered
				}
				if r.Method == http.MethodPut && strings.HasPrefix(r.URL.Path, "/api/v1/uploads/") {
					limit = hub.UploadChunkBytes + 1 // one chunk; the service answers 413 for a larger Content-Range
				}
				r.Body = http.MaxBytesReader(w, r.Body, limit)
				defer s.recoverHandler(w, r)
				if r.Method == http.MethodPost && r.URL.Path == "/api/v1/handshake" {
					r2, err := s.checkHandshakeBody(r)
					if err != nil {
						s.writeErr(w, r, err)
						return
					}
					r = r2
				}
				if isStreamRoute(r) {
					rw, done := rollingWrite(w)
					defer done()
					w = rw
				}
				next.ServeHTTP(w, r)
			})
		}},
		ErrorHandlerFunc: func(w http.ResponseWriter, r *http.Request, err error) {
			var ip *api.InvalidParamFormatError
			if errors.As(err, &ip) && ip.ParamName != "confirm_revision" && ip.ParamName != "after" && ip.ParamName != "limit" { // e.g. no UUID in the path: does not exist
				writeError(w, http.StatusNotFound, hub.CodeNotFound, "Not found")
				return
			}
			writeError(w, http.StatusBadRequest, hub.CodeBadRequest, "Invalid request")
		},
	})
	mux.HandleFunc("GET /api/v1/roms/{sha256}", s.downloadROM)
	mux.HandleFunc("GET /api/v1/systems/{system_id}/firmware/{file_id}", s.downloadFirmware)
	mux.HandleFunc("GET /api/v1/admin/backups/{name}/download", s.downloadAdminBackup)
	mux.HandleFunc("GET /api/v1/ws", s.serveWS)
}

// skipRomMux skips the generated routes for the ROM download and the WSS upgrade; both are registered
// directly on the mux.
type skipRomMux struct{ *http.ServeMux }

func (m skipRomMux) HandleFunc(pattern string, h func(http.ResponseWriter, *http.Request)) {
	if strings.Contains(pattern, "/api/v1/roms/") || pattern == "GET /api/v1/systems/{system_id}/firmware/{file_id}" || strings.HasSuffix(pattern, "/admin/backups/{name}/download") || strings.HasSuffix(pattern, " /api/v1/ws") {
		return
	}
	if pattern == "GET /api/v1/plugins/{plugin_id}/{version}/file" {
		// "GET /plugins/{plugin_id}/store/{key}" (plugin user store) would conflict with this pattern in the mux, because
		// neither is more specific. With a wildcard for the last segment the store route is the more specific one; the
		// file route answers 404 for anything but ".../file".
		m.ServeMux.HandleFunc("GET /api/v1/plugins/{plugin_id}/{version}/{last}", func(w http.ResponseWriter, r *http.Request) {
			if r.PathValue("last") != "file" {
				writeError(w, http.StatusNotFound, hub.CodeNotFound, "Not found")
				return
			}
			h(w, r)
		})
		return
	}
	if pattern == "GET /api/v1/admin/reports/{id}/archive" {
		// "GET /admin/reports/errors/{fingerprint}" (error groups) would conflict with this pattern: "errors/archive"
		// matches both and neither is more specific. With a wildcard for the last segment the error route is the more
		// specific one; the archive route answers 404 for anything but ".../archive".
		m.ServeMux.HandleFunc("GET /api/v1/admin/reports/{id}/{last}", func(w http.ResponseWriter, r *http.Request) {
			if r.PathValue("last") != "archive" {
				writeError(w, http.StatusNotFound, hub.CodeNotFound, "Not found")
				return
			}
			h(w, r)
		})
		return
	}
	m.ServeMux.HandleFunc(pattern, h)
}

// routeOf names the matched route ("GET /api/v1/games/{game_id}"), never the concrete path with ids.
func routeOf(r *http.Request) string {
	if r.Pattern != "" {
		return r.Pattern
	}
	return r.Method + " (unmatched)"
}

// recordServerError hands a server-side error to the error groups (ADR 0035 D1); the service limits the rate.
func (s *Server) recordServerError(r *http.Request, title, message string) {
	s.svc.RecordServerError(r.Context(), hub.ErrKindServerError, title, message)
}

// recoverHandler turns a panic of a request handler into a 500 and an error event. http.ErrAbortHandler is passed on.
func (s *Server) recoverHandler(w http.ResponseWriter, r *http.Request) {
	v := recover()
	if v == nil {
		return
	}
	if v == http.ErrAbortHandler {
		panic(v)
	}
	stack := string(debug.Stack())
	s.log.Error("handler panic", "method", r.Method, "path", r.URL.Path, "panic", fmt.Sprint(v), "stack", stack)
	s.recordServerError(r, "Panic in "+routeOf(r), fmt.Sprintf("panic in %s: %v\n%s", routeOf(r), v, stack))
	writeError(w, http.StatusInternalServerError, hub.CodeInternal, "Internal error")
}

func bearer(r *http.Request) string {
	h := r.Header.Get("Authorization")
	if len(h) > 7 && strings.EqualFold(h[:7], "bearer ") {
		return strings.TrimSpace(h[7:])
	}
	return ""
}

func remoteIP(r *http.Request) string {
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		return r.RemoteAddr
	}
	return host
}

func (s *Server) authMiddleware(next api.StrictHandlerFunc, op string) api.StrictHandlerFunc {
	return func(ctx context.Context, w http.ResponseWriter, r *http.Request, req any) (any, error) {
		tok := bearer(r)
		ctx = context.WithValue(ctx, keyBearer, tok)
		ctx = context.WithValue(ctx, keyRemoteIP, remoteIP(r))
		ctx = hub.WithRequestHost(ctx, r.Host)
		ctx = context.WithValue(ctx, keyHost, r.Host)
		if isLocalOp(op) && !isLoopbackRemote(r) {
			return nil, errNotLoopback
		}
		switch op {
		case "UploadGame", "ListSystems", "RevokeSelf", "PostHandshake", "ListGames", "GetGame", "ConnectWebSocket",
			"ListSaves", "GetSaveSlot", "PutSave", "DownloadSaveContent", "ListSaveHistory", "DownloadSaveHistoryContent", "ResolveSaveConflict",
			"UpdateLocalSettings", "RestoreSaveHistoryVersion", "CreateSaveSnapshot", "DeleteSaveVersion", "UploadSaveFile", "RenameSaveSlot", "DeleteSaveSlot",
			"ListUsers", "GetCorePackage", "GetCorePackageFile", "ListSessions", "PublishSession", "GetSession", "UpdateSession", "EndSession", "InviteSessionUser",
			"WithdrawSessionInvite", "DeclineSession", "JoinSession", "RemoveSessionViewer", "ListPlugins", "GetPluginFile",
			"RegisterGame", "DeleteGameRegistration", "CreateUpload", "GetUpload", "PutUploadChunk", "DeleteUpload",
			"GetAdminStatus", "GetAdminStatusLog", "GetAdminStorage", "CleanupAdminStorage", "GetAdminMetrics", "SetAdminMetrics", "CreateAdminMetricsToken",
			"ListAdminBackups", "StartAdminBackup", "GetAdminBackupStatus", "CancelAdminBackup", "GetAdminBackupSettings", "UpdateAdminBackupSettings",
			"DismissAdminRestoreResult", "DeleteAdminBackup", "RestoreAdminBackup",
			"CreateReport", "CreateErrorEvents", "ListAdminErrorGroups", "GetAdminErrorGroup", "PatchAdminErrorGroup", "DeleteAdminErrorGroup", "ListAdminReports", "GetAdminReport", "MarkAdminReportSeen", "DeleteAdminReport", "GetAdminReportFile", "GetAdminReportArchive", "ExportAdminUserSaves",
			"GetAdminUpdateVersions", "RollbackAdminUpdate", "ResumeAdminUpdates",
			"GetAccount", "UpdateAccount", "SetAccountPassword", "RemoveAccountPassword", "ConfirmAccount", "EndAccountConfirmation",
			"ListAccountDevices", "SignOutAccountDevice", "SignOutAllAccountDevices", "ResetAdminUserPassword",
			"GetAdminAccountSettings", "UpdateAdminAccountSettings",
			"ListAdminUsers", "CreateAdminUser", "UpdateAdminUser", "DeleteAdminUser", "CreateAdminUserCode", "SetAdminUserRole", "DisableAdminUser", "EnableAdminUser",
			"ListAdminInvites", "CreateAdminInvite", "RevokeAdminInvite", "ListAdminPairingRequests", "AllowAdminPairingRequest", "DenyAdminPairingRequest",
			"ListAdminDevices", "RenameAdminDevice", "RevokeAdminDevice", "DeleteAdminDevice",
			"ListAdminSystems", "UpdateAdminSystem", "UpdateAdminSystemCore", "InstallAdminCore", "UpgradeAdminCore", "RemoveAdminCore", "SyncAdminCores", "UpdateAdminCoreSettings",
			"GetAdminFirmwareFile", "PutAdminFirmwareFile", "PinAdminFirmwareFile", "RemoveAdminFirmwareFile",
			"ListAdminPlugins", "StageAdminPlugin", "OfferAdminPlugin", "DiscardAdminPlugin", "RemoveAdminPlugin", "RefreshAdminPluginIndex", "InstallAdminIndexPlugin",
			"CreateAdminDiagnosticsBundle", "DeleteAdminGame", "RenameAdminGame", "GetAdminLibraryRescan", "StartAdminLibraryRescan", "ListAdminRuns", "DeleteAdminRun", "GetAdminRun", "ArchiveAdminRun", "ExportAdminRun", "FinishAdminRun", "RemoveAdminRunInvite", "ResendAdminRunInvite", "PauseAdminRun", "ResendAdminRunInvites", "ResumeAdminRun", "StartAdminRun", "UnarchiveAdminRun", "ListAdminSaves", "RenewAdminCertificate", "GetAdminSettings", "UpdateAdminSettings", "DeleteAdminExternalTurn", "SaveAdminExternalTurn", "ResetAdminExternalTurn", "RestartAdminServer", "ResetAdminNetSetting", "SetAdminNetSetting", "CheckAdminUpdates", "InstallAdminUpdate", "DeleteAdminSaveSlot", "GetAdminSaveSlot", "ResolveAdminSaveConflict", "DownloadAdminSaveContent", "ListAdminSaveHistory", "DeleteAdminSaveVersion", "DownloadAdminSaveHistoryContent", "RestoreAdminSaveVersion", "RenameAdminSaveSlot", "CreateAdminSaveSlot", "CreateAdminSaveSnapshot", "UploadAdminSaveFile",
			"ListSaveStates", "DownloadSaveState", "PutSaveState", "DeleteSaveState", "DownloadSaveStatePreview", "SetRunRules", "SetRunPlugins",
			"ListPluginStore", "GetPluginStoreValue", "PutPluginStoreValue", "DeletePluginStoreValue", "GetGameCheats", "GetAdminCheatDb", "UpdateAdminCheatDb", "RefreshAdminCheatDb",
			"UploadScreenshot", "ListScreenshots", "ListRunScreenshots", "DownloadScreenshotContent", "DeleteScreenshot",
			"AddFavorite", "RemoveFavorite", "ListCollections", "CreateCollection", "RenameCollection", "DeleteCollection", "AddCollectionGame", "RemoveCollectionGame",
			"ListTools", "GetToolPackage", "ListVariants", "ListGameVariants", "CreateVariant", "RenameVariant", "DeleteVariant", "GetRunVariant", "SetRunVariantState",
			"ListAdminTools", "UpdateAdminTool", "SyncAdminTool", "InstallAdminToolVersion", "RemoveAdminToolVersion",
			"ListRuns", "CreateRun", "GetRun", "JoinRun", "DeclineRun", "LeaveRun", "InviteRun", "SetRunStatus", "PutRunState", "AppendRunEvent", "ListRunEvents", "PutRunBoard", "ListRunChat", "ListSessionChat":
			p, err := s.svc.Authenticate(ctx, tok)
			if err != nil {
				return nil, err
			}
			ctx = context.WithValue(ctx, keyPrincipal, p)
		}
		return next(ctx, w, r, req)
	}
}

// handshakeIdentity are the fields every app version has always sent; a handshake without them is a 400.
// The newer members (cores, video, audio, input) are optional on the wire so that older installed apps can
// still reach the server (and its update); when they are missing the server treats them as "not reported".
var handshakeIdentity = []string{"platform", "arch", "player_version", "protocol_version", "min_protocol_version"}

// handshakeSeen records which optional handshake parts the body carried (oapi-codegen decodes a missing object
// to zero values that would look like "no H.264").
type handshakeSeen struct{ cores, video bool }

const keyHandshakeSeen ctxKey = 100

var handshakeLenientOnce sync.Once

// checkHandshakeBody reads the (already size-capped) body, verifies the identity fields, notes which optional
// parts are present and restores the body for the strict handler. Malformed JSON is left to the handler (400).
func (s *Server) checkHandshakeBody(r *http.Request) (*http.Request, error) {
	raw, err := io.ReadAll(r.Body)
	if err != nil {
		return r, err
	}
	r.Body = io.NopCloser(bytes.NewReader(raw))
	var top map[string]json.RawMessage
	if json.Unmarshal(raw, &top) != nil {
		return r, nil
	}
	for _, f := range handshakeIdentity {
		if v, ok := top[f]; !ok || string(v) == "null" {
			return r, &hub.Error{Code: hub.CodeBadRequest, Message: "Handshake field " + f + " is required"}
		}
	}
	var video map[string]json.RawMessage
	_ = json.Unmarshal(top["video"], &video)
	_, enc := video["h264_encode"]
	_, dec := video["h264_decode"]
	seen := handshakeSeen{cores: string(top["cores"]) != "" && string(top["cores"]) != "null", video: enc && dec}
	if !seen.cores || !seen.video {
		handshakeLenientOnce.Do(func() {
			s.log.Debug("handshake without cores or video capabilities (older app); treated as not reported")
		})
	}
	return r.WithContext(context.WithValue(r.Context(), keyHandshakeSeen, seen)), nil
}

func principal(ctx context.Context) hub.Principal {
	p, _ := ctx.Value(keyPrincipal).(hub.Principal)
	return p
}

func httpStatus(c hub.Code) int {
	switch c {
	case hub.CodeBadRequest, hub.CodeImportDirUnreadable, hub.CodeInvalidImage:
		return http.StatusBadRequest
	case hub.CodeUnauthorized, hub.CodeDeviceRevoked, hub.CodeInvalidCredentials, hub.CodeUserDisabled, hub.CodeInvalidSetupCode:
		return http.StatusUnauthorized
	case hub.CodeForbidden, hub.CodeUploadsDisabled, hub.CodeConfirmationRequired, hub.CodePasswordRequired:
		return http.StatusForbidden
	case hub.CodeNotFound, hub.CodeInviteInvalid, hub.CodeCorePackageNotFound, hub.CodeCoreFileNotAvailable:
		return http.StatusNotFound
	case hub.CodeSessionForbidden:
		return http.StatusForbidden
	case hub.CodeSessionNotFound:
		return http.StatusNotFound
	case hub.CodeSessionEnded, hub.CodeSetupCodeExpired:
		return http.StatusGone
	case hub.CodeRunNotActive, hub.CodeStateConflict, hub.CodeStoreConflict:
		return http.StatusConflict
	case hub.CodeTooLarge:
		return http.StatusRequestEntityTooLarge
	case hub.CodeToolInUse, hub.CodeToolHashMismatch, hub.CodeNameTaken, hub.CodeOwnVariantMismatch, hub.CodeOwnVariantsOff,
		hub.CodeConflict, hub.CodeChildMode, hub.CodePairingExpired, hub.CodeSaveConflict, hub.CodeSaveConflictStale,
		hub.CodeSessionFull, hub.CodeCapabilityMissing, hub.CodeDisplayNameTaken,
		hub.CodePluginUpdateIncompatible, hub.CodePluginRequiresMissing, hub.CodeSaveSlotExists, hub.CodeSessionRunning, hub.CodeSaveSlotProtected, hub.CodePasswordNotSet, hub.CodeAlreadySetUp:
		return http.StatusConflict
	case hub.CodePayloadTooLarge, hub.CodeStateQuotaExceeded, hub.CodeStoreQuotaExceeded:
		return http.StatusRequestEntityTooLarge
	case hub.CodeRateLimited:
		return http.StatusTooManyRequests
	}
	return http.StatusInternalServerError
}

func writeError(w http.ResponseWriter, status int, code hub.Code, msg string) {
	writeErrorRetry(w, status, code, msg, 0)
}

// writeErrorRetry also names the wait of a rate_limited answer (retry_after in the body, Retry-After header).
func writeErrorRetry(w http.ResponseWriter, status int, code hub.Code, msg string, retryAfter int) {
	if status == http.StatusUnauthorized {
		w.Header().Set("WWW-Authenticate", "Bearer")
	}
	w.Header().Set("Content-Type", "application/json")
	var e api.Error
	e.Error.Code, e.Error.Message = api.ErrorCode(code), msg
	if retryAfter > 0 {
		w.Header().Set("Retry-After", strconv.Itoa(retryAfter))
		e.Error.RetryAfter = &retryAfter
	}
	w.WriteHeader(status)
	json.NewEncoder(w).Encode(e)
}

func (s *Server) writeErr(w http.ResponseWriter, r *http.Request, err error) {
	var he *hub.Error
	switch {
	case errors.As(err, &he) && he.ExistingGameID != "" && he.Code == hub.CodeConflict:
		// a ROM that is already stored: name the existing game (DuplicateGameError)
		if id, perr := uuid.Parse(he.ExistingGameID); perr == nil {
			var out api.DuplicateGameError
			out.Error.Code, out.Error.Message, out.ExistingGameId = api.ErrorCode(he.Code), he.Message, id
			w.Header().Set("Content-Type", "application/json")
			w.WriteHeader(http.StatusConflict)
			json.NewEncoder(w).Encode(out)
			return
		}
		writeError(w, httpStatus(he.Code), he.Code, he.Message)
	case errors.As(err, &he):
		writeErrorRetry(w, httpStatus(he.Code), he.Code, he.Message, he.RetryAfter)
	case errors.Is(err, errNotImplemented):
		writeError(w, http.StatusNotImplemented, hub.CodeInternal, "Not implemented yet")
	default:
		var mbe *http.MaxBytesError
		if errors.As(err, &mbe) {
			writeError(w, http.StatusRequestEntityTooLarge, hub.CodePayloadTooLarge, "Request too large")
			return
		}
		s.log.Error("internal error", "method", r.Method, "path", r.URL.Path, "err", err)
		if !errors.Is(err, context.Canceled) { // a client that went away is not a server error
			s.recordServerError(r, "Internal error in "+routeOf(r), routeOf(r)+": "+err.Error())
		}
		writeError(w, http.StatusInternalServerError, hub.CodeInternal, "Internal error")
	}
}

// ---- StrictServerInterface ----

func (s *Server) GetHubInfo(ctx context.Context, _ api.GetHubInfoRequestObject) (api.GetHubInfoResponseObject, error) {
	i := s.svc.Info()
	setup, err := s.svc.SetupRequired(ctx)
	if err != nil {
		return nil, err
	}
	id, err := uuid.Parse(i.HubID)
	if err != nil {
		return nil, err
	}
	return api.GetHubInfo200JSONResponse{HubId: id, Name: i.Name, HubVersion: i.HubVersion,
		ProtocolVersion: i.ProtocolVersion, MinProtocolVersion: i.MinProtocolVersion, ApiBase: "/api/v1", SetupRequired: setup}, nil
}

func (s *Server) CreatePairingRequest(ctx context.Context, req api.CreatePairingRequestRequestObject) (api.CreatePairingRequestResponseObject, error) {
	if req.Body == nil {
		return nil, hub.ErrBadRequest
	}
	b := req.Body
	ip, _ := ctx.Value(keyRemoteIP).(string)
	in := hub.PairingInput{DeviceID: b.DeviceId.String(), DeviceName: b.DeviceName,
		Platform: b.Platform, Arch: b.Arch, PlayerVersion: b.PlayerVersion, ProtocolVersion: b.ProtocolVersion, RemoteAddr: ip}
	if b.InstallId != nil {
		in.InstallID = *b.InstallId
	}
	c, err := s.svc.CreatePairingRequest(ctx, in)
	if err != nil {
		return nil, err
	}
	id, err := uuid.Parse(c.RequestID)
	if err != nil {
		return nil, err
	}
	return api.CreatePairingRequest202JSONResponse{RequestId: id, PollToken: c.PollToken,
		Status: api.PairingRequestAcceptedStatus("pending"), ExpiresIn: int(c.ExpiresIn.Seconds())}, nil
}

func (s *Server) GetPairingRequest(ctx context.Context, req api.GetPairingRequestRequestObject) (api.GetPairingRequestResponseObject, error) {
	tok, _ := ctx.Value(keyBearer).(string)
	res, err := s.svc.PollPairing(ctx, req.RequestId.String(), tok)
	if err != nil {
		return nil, err
	}
	out := api.PairingStatus{Status: api.PairingStatusStatus(res.Status)}
	if res.Status == hub.PairingApproved {
		hid, err := uuid.Parse(res.HubID)
		if err != nil {
			return nil, err
		}
		out.HubId, out.UserId, out.DeviceCredential = &hid, &res.UserID, &res.DeviceCredential
	}
	return api.GetPairingRequest200JSONResponse(out), nil
}

func (s *Server) CreateAccessToken(ctx context.Context, req api.CreateAccessTokenRequestObject) (api.CreateAccessTokenResponseObject, error) {
	if req.Body == nil {
		return nil, hub.ErrBadRequest
	}
	t, err := s.svc.IssueAccessToken(ctx, req.Body.DeviceId.String(), req.Body.DeviceCredential)
	if err != nil {
		return nil, err
	}
	return api.CreateAccessToken200JSONResponse{AccessToken: t.Token, TokenType: api.Bearer, ExpiresIn: int(t.ExpiresIn.Seconds())}, nil
}

func (s *Server) RevokeSelf(ctx context.Context, _ api.RevokeSelfRequestObject) (api.RevokeSelfResponseObject, error) {
	if err := s.svc.RevokeDevice(ctx, principal(ctx).Device.ID); err != nil {
		return nil, err
	}
	return api.RevokeSelf204Response{}, nil
}

func (s *Server) PostHandshake(ctx context.Context, req api.PostHandshakeRequestObject) (api.PostHandshakeResponseObject, error) {
	if req.Body == nil {
		return nil, hub.ErrBadRequest
	}
	b := req.Body
	seen, _ := ctx.Value(keyHandshakeSeen).(handshakeSeen)
	cores := make([]hub.CoreReport, 0, len(b.Cores))
	for _, c := range b.Cores {
		cores = append(cores, hub.CoreReport{ID: c.Id, Version: c.Version})
	}
	in := hub.HandshakeInput{Platform: b.Platform, Arch: b.Arch,
		PlayerVersion: b.PlayerVersion, ProtocolVersion: b.ProtocolVersion, MinProtocolVersion: b.MinProtocolVersion}
	if seen.video {
		in.H264Encode, in.H264Decode = &b.Video.H264Encode, &b.Video.H264Decode
	}
	if seen.cores {
		in.Cores = &cores
	}
	res, err := s.svc.Handshake(ctx, principal(ctx).Device.ID, in)
	if err != nil {
		return nil, err
	}
	features := hub.BaseFeatures()
	if s.svc.RelayEnabled() {
		features = append(features, hub.FeatureTURNV1)
	}
	if can, err := s.svc.CanUpload(ctx, principal(ctx).User); err != nil {
		return nil, err
	} else if can {
		features = append(features, hub.FeatureUploadsV1)
	}
	if principal(ctx).User.Role == hub.RoleAdmin {
		features = append(features, hub.FeatureAdminV1)
	}
	out := api.HandshakeResponse{HubVersion: res.Info.HubVersion, ProtocolVersion: res.Info.ProtocolVersion,
		MinProtocolVersion: res.Info.MinProtocolVersion, Compatible: res.Compatible, Problems: []api.HandshakeProblem{},
		Features: &features}
	if u := principal(ctx).User; u.ID != "" {
		out.User = &api.HandshakeUser{Id: u.ID, DisplayName: u.DisplayName, Role: api.HandshakeUserRole(u.Role)}
	}
	for _, p := range res.Problems {
		hp := api.HandshakeProblem{Code: api.HandshakeProblemCode(p.Code), Detail: p.Detail}
		if p.CoreID != "" {
			id := p.CoreID
			hp.CoreId = &id
		}
		out.Problems = append(out.Problems, hp)
	}
	return api.PostHandshake200JSONResponse(out), nil
}

func toAPIGame(g hub.Game) (api.Game, error) {
	id, err := uuid.Parse(g.ID)
	if err != nil {
		return api.Game{}, err
	}
	onHub := g.OnHub
	roms := make([]api.RomInfo, 0, len(g.ROMs))
	for _, f := range g.ROMs {
		stored := f.Stored
		roms = append(roms, api.RomInfo{Sha256: f.SHA256, Size: f.Size, Filename: f.Filename, Stored: &stored})
	}
	rom := api.RomInfo{Sha256: g.ROMSHA256, Size: g.ROMSize, Filename: g.Filename, Stored: &onHub}
	return api.Game{Id: id, Title: g.Title, System: g.System, UploadedBy: g.UploadedBy, AddedAt: g.AddedAt,
		Rom: rom, OnHub: &onHub, Roms: &roms, GameVersion: g.GameVersion,
		GameCode: &g.GameCode, GameCodeKind: (*api.GameGameCodeKind)(&g.GameCodeKind)}, nil
}

// ListGames lists the games visible to the caller: games with a file on the server and local-only games the caller
// registered a file of.
func (s *Server) ListGames(ctx context.Context, _ api.ListGamesRequestObject) (api.ListGamesResponseObject, error) {
	gs, err := s.svc.ListGamesFor(ctx, principal(ctx).User.ID)
	if err != nil {
		return nil, err
	}
	personal, err := s.svc.PersonalGames(ctx, principal(ctx).User.ID, "")
	if err != nil {
		return nil, err
	}
	out := api.ListGames200JSONResponse{Games: make([]api.Game, 0, len(gs))}
	for _, g := range gs {
		ag, err := toAPIGame(g)
		if err != nil {
			return nil, err
		}
		ag.Personal = toAPIPersonal(personal[g.ID])
		out.Games = append(out.Games, ag)
	}
	return out, nil
}

func (s *Server) GetGame(ctx context.Context, req api.GetGameRequestObject) (api.GetGameResponseObject, error) {
	g, err := s.svc.GetGameFor(ctx, principal(ctx).User.ID, req.GameId.String())
	if err != nil {
		return nil, err
	}
	ag, err := toAPIGame(g)
	if err != nil {
		return nil, err
	}
	personal, err := s.svc.PersonalGames(ctx, principal(ctx).User.ID, g.ID)
	if err != nil {
		return nil, err
	}
	ag.Personal = toAPIPersonal(personal[g.ID])
	return api.GetGame200JSONResponse(ag), nil
}

// DownloadRom is not used by the strict handler (the route lives on the mux, see downloadROM).
func (s *Server) DownloadRom(context.Context, api.DownloadRomRequestObject) (api.DownloadRomResponseObject, error) {
	return nil, errNotImplemented
}

// ConnectWebSocket: documented only; the upgrade is served by serveWS (ws.go) directly on the mux.
func (s *Server) ConnectWebSocket(context.Context, api.ConnectWebSocketRequestObject) (api.ConnectWebSocketResponseObject, error) {
	return nil, errNotImplemented
}

// ---- Firmware download (directly on the mux: ETag = sha256, If-None-Match; never logged) ----

func (s *Server) downloadFirmware(w http.ResponseWriter, r *http.Request) {
	if _, err := s.svc.Authenticate(r.Context(), bearer(r)); err != nil {
		s.writeErr(w, r, err)
		return
	}
	f, def, err := s.svc.OpenFirmware(r.Context(), r.PathValue("system_id"), r.PathValue("file_id"))
	if err != nil {
		s.writeErr(w, r, err)
		return
	}
	defer f.Close()
	w, done := rollingWrite(w)
	defer done()
	h := w.Header()
	h.Set("Cache-Control", "private, no-cache")
	h.Set("Content-Type", "application/octet-stream")
	h.Set("ETag", `"`+def.SHA256+`"`)
	http.ServeContent(&jsonRangeErrWriter{ResponseWriter: w}, r, "", time.Time{}, f)
}

// GetFirmwareFile is not used by the strict handler (the route lives on the mux, see downloadFirmware).
func (s *Server) GetFirmwareFile(context.Context, api.GetFirmwareFileRequestObject) (api.GetFirmwareFileResponseObject, error) {
	return nil, errNotImplemented
}

// ---- ROM download (directly on the mux: Range, ETag, If-None-Match via http.ServeContent) ----

func (s *Server) downloadROM(w http.ResponseWriter, r *http.Request) {
	if _, err := s.svc.Authenticate(r.Context(), bearer(r)); err != nil {
		s.writeErr(w, r, err)
		return
	}
	f, g, err := s.svc.OpenROM(r.Context(), r.PathValue("sha256"))
	if err != nil {
		s.writeErr(w, r, err)
		return
	}
	defer f.Close()
	w, done := rollingWrite(w)
	defer done()
	h := w.Header()
	h.Set("Cache-Control", "private, max-age=0, must-revalidate")
	h.Set("Content-Type", "application/octet-stream")
	h.Set("ETag", `"`+g.ROMSHA256+`"`)
	http.ServeContent(&jsonRangeErrWriter{ResponseWriter: w}, r, g.Filename, g.AddedAt, f)
}

// jsonRangeErrWriter replaces the plain-text error body of http.ServeContent on 416 with the spec's error format.
type jsonRangeErrWriter struct {
	http.ResponseWriter
	errBody bool
}

func (w *jsonRangeErrWriter) WriteHeader(code int) {
	if code == http.StatusRequestedRangeNotSatisfiable {
		w.errBody = true
		w.Header().Set("Content-Type", "application/json")
		w.Header().Del("X-Content-Type-Options")
	}
	w.ResponseWriter.WriteHeader(code)
}

func (w *jsonRangeErrWriter) Write(p []byte) (int, error) {
	if w.errBody {
		w.errBody = false
		var e api.Error
		e.Error.Code, e.Error.Message = api.ErrorCode(hub.CodeBadRequest), "Range not satisfiable"
		if err := json.NewEncoder(w.ResponseWriter).Encode(e); err != nil {
			return 0, err
		}
		return len(p), nil
	}
	return w.ResponseWriter.Write(p)
}

// LogRequests logs method, path, status and duration (never headers, query or bodies).
func LogRequests(log *slog.Logger, next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		sw := &statusWriter{ResponseWriter: w, status: 200}
		next.ServeHTTP(sw, r)
		log.Info("request", "method", r.Method, "path", r.URL.Path, "status", sw.status, "dur_ms", time.Since(start).Milliseconds())
	})
}

type statusWriter struct {
	http.ResponseWriter
	status int
}

func (w *statusWriter) WriteHeader(c int) { w.status = c; w.ResponseWriter.WriteHeader(c) }

func (w *statusWriter) Unwrap() http.ResponseWriter { return w.ResponseWriter }

// ptr returns a pointer to v (optional response header fields in generated code are pointers).
func ptr[T any](v T) *T { return &v }
