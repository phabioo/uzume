package httpapi

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"net/http"

	"github.com/phabioo/uzume/server/internal/api"
	"github.com/phabioo/uzume/server/internal/hub"
)

// Save states (0.20 package 2, feature states_v1) and the core run rules (feature run_rules_v1). Own user only.

func toAPISaveState(st hub.SaveState) api.SaveState {
	out := api.SaveState{GameId: mustUUID(st.GameID), Slot: st.Slot, N: st.N, CoreId: st.CoreID, CoreVersion: st.CoreVersion,
		SizeBytes: st.SizeBytes, HasPreview: st.PreviewBytes > 0, Sha256: st.SHA256, DeviceId: st.DeviceID,
		DeviceName: st.DeviceName, CreatedAt: st.CreatedAt}
	if st.PreviewBytes > 0 {
		out.PreviewBytes = ptr(st.PreviewBytes)
	}
	return out
}

func (s *Server) ListSaveStates(ctx context.Context, req api.ListSaveStatesRequestObject) (api.ListSaveStatesResponseObject, error) {
	slot := ""
	if req.Params.Slot != nil {
		slot = *req.Params.Slot
	}
	l, err := s.svc.ListStates(ctx, principal(ctx).User.ID, req.GameId.String(), slot)
	if err != nil {
		return nil, err
	}
	out := api.ListSaveStates200JSONResponse{States: make([]api.SaveState, 0, len(l.States)), UsedBytes: l.UsedBytes, QuotaBytes: l.QuotaBytes}
	for _, st := range l.States {
		out.States = append(out.States, toAPISaveState(st))
	}
	return out, nil
}

func (s *Server) DownloadSaveState(ctx context.Context, req api.DownloadSaveStateRequestObject) (api.DownloadSaveStateResponseObject, error) {
	f, st, err := s.svc.OpenState(ctx, principal(ctx).User.ID, req.GameId.String(), req.Slot, req.N)
	if err != nil {
		return nil, err
	}
	return api.DownloadSaveState200ApplicationoctetStreamResponse{Body: f, Headers: api.DownloadSaveState200ResponseHeaders{
		ContentLength: ptr(st.SizeBytes), ETag: ptr(`"` + st.SHA256 + `"`), XUzumeCoreId: ptr(st.CoreID), XUzumeCoreVersion: ptr(st.CoreVersion)}}, nil
}

func (s *Server) DownloadSaveStatePreview(ctx context.Context, req api.DownloadSaveStatePreviewRequestObject) (api.DownloadSaveStatePreviewResponseObject, error) {
	f, st, err := s.svc.OpenStatePreview(ctx, principal(ctx).User.ID, req.GameId.String(), req.Slot, req.N)
	if err != nil {
		return nil, err
	}
	return api.DownloadSaveStatePreview200ImagepngResponse{Body: f, Headers: api.DownloadSaveStatePreview200ResponseHeaders{
		ContentLength: ptr(st.PreviewBytes)}}, nil
}

func (s *Server) DeleteSaveState(ctx context.Context, req api.DeleteSaveStateRequestObject) (api.DeleteSaveStateResponseObject, error) {
	if err := s.svc.DeleteState(ctx, principal(ctx).User.ID, req.GameId.String(), req.Slot, req.N); err != nil {
		return nil, err
	}
	return api.DeleteSaveState204Response{}, nil
}

// PutSaveState reads the parts meta, state and (optionally) preview in this order; the state streams to disk.
func (s *Server) PutSaveState(ctx context.Context, req api.PutSaveStateRequestObject) (api.PutSaveStateResponseObject, error) {
	if req.Body == nil {
		return nil, hub.ErrBadRequest
	}
	mr := req.Body
	var mbe *http.MaxBytesError
	part, err := mr.NextPart()
	if errors.As(err, &mbe) {
		return nil, err
	}
	if err != nil || part.FormName() != "meta" {
		return nil, &hub.Error{Code: hub.CodeBadRequest, Message: "The first part must be meta"}
	}
	raw, err := io.ReadAll(io.LimitReader(part, hub.StateMetaMaxBytes+1))
	if err != nil {
		return nil, err
	}
	var meta api.SaveStateMeta
	if len(raw) > hub.StateMetaMaxBytes || json.Unmarshal(raw, &meta) != nil {
		return nil, &hub.Error{Code: hub.CodeBadRequest, Message: "meta is not valid SaveStateMeta JSON"}
	}
	part, err = mr.NextPart()
	if errors.As(err, &mbe) {
		return nil, err
	}
	if err != nil || part.FormName() != "state" {
		return nil, &hub.Error{Code: hub.CodeBadRequest, Message: "The second part must be state"}
	}
	p := principal(ctx)
	in := hub.PutStateInput{UserID: p.User.ID, DeviceID: p.Device.ID, GameID: req.GameId.String(), Slot: req.Slot, N: req.N,
		CoreID: meta.CoreId, CoreVersion: meta.CoreVersion}
	// The optional preview part follows the state in the body, so the service asks for it after it read the state.
	in.State = part
	in.NextPreview = func() (io.Reader, error) {
		pp, err := mr.NextPart()
		if err == io.EOF {
			return nil, nil
		}
		if err != nil {
			return nil, err // a body over the cap surfaces as *http.MaxBytesError (413)
		}
		if pp.FormName() != "preview" {
			return nil, &hub.Error{Code: hub.CodeBadRequest, Message: "The third part must be preview"}
		}
		return pp, nil
	}
	st, err := s.svc.PutState(ctx, in)
	if err != nil {
		return nil, err
	}
	return api.PutSaveState200JSONResponse(toAPISaveState(st)), nil
}
