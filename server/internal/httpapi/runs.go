package httpapi

import (
	"context"
	"encoding/json"

	"github.com/phabioo/uzume/server/internal/api"
	"github.com/phabioo/uzume/server/internal/hub"
)

// Runs REST API (feature runs_v1). The hub package already produces the wire format (JSON tags), so the generated
// types are filled by a JSON round trip; opaque objects (settings, state, payload, tree) stay byte-exact.

func toAPIRun(r hub.Run) (api.Run, error) {
	var out api.Run
	b, err := json.Marshal(r)
	if err == nil {
		err = json.Unmarshal(b, &out)
	}
	return out, err
}

func toAPIEvent(e hub.RunEvent) (api.RunEvent, error) {
	var out api.RunEvent
	b, err := json.Marshal(e)
	if err == nil {
		err = json.Unmarshal(b, &out)
	}
	return out, err
}

func (s *Server) ListRuns(ctx context.Context, _ api.ListRunsRequestObject) (api.ListRunsResponseObject, error) {
	runs, err := s.svc.ListRuns(ctx, principal(ctx).User.ID)
	if err != nil {
		return nil, err
	}
	out := api.ListRuns200JSONResponse{Runs: make([]api.Run, 0, len(runs))}
	for _, r := range runs {
		ar, err := toAPIRun(r)
		if err != nil {
			return nil, err
		}
		out.Runs = append(out.Runs, ar)
	}
	return out, nil
}

func (s *Server) CreateRun(ctx context.Context, req api.CreateRunRequestObject) (api.CreateRunResponseObject, error) {
	if req.Body == nil {
		return nil, hub.ErrBadRequest
	}
	b := req.Body
	in := hub.CreateRunInput{UserID: principal(ctx).User.ID, Name: b.Name, GameID: b.GameId.String()}
	for _, p := range b.Plugins {
		in.Plugins = append(in.Plugins, hub.RunPluginRef{PluginID: p.PluginId, Version: p.Version})
	}
	if b.InviteUserIds != nil {
		in.InviteUserIDs = *b.InviteUserIds
	}
	if b.Settings != nil {
		in.Settings = json.RawMessage(*b.Settings)
	}
	if rz := b.Randomizer; rz != nil {
		in.Randomizer = &hub.RunRandomizerInput{Mode: string(rz.Mode), ToolID: deref(rz.ToolId), ToolVersion: deref(rz.ToolVersion),
			PresetID: deref(rz.PresetId), Settings: deref(rz.Settings), AllowOwn: rz.AllowOwn != nil && *rz.AllowOwn}
	}
	if b.Rules != nil {
		in.Rules = &hub.RunRules{States: b.Rules.States, Rewind: b.Rules.Rewind, Cheats: string(b.Rules.Cheats), MemoryWrite: string(b.Rules.MemoryWrite)}
	}
	run, err := s.svc.CreateRun(ctx, in)
	if err != nil {
		return nil, err
	}
	ar, err := toAPIRun(run)
	if err != nil {
		return nil, err
	}
	return api.CreateRun201JSONResponse(ar), nil
}

func (s *Server) GetRun(ctx context.Context, req api.GetRunRequestObject) (api.GetRunResponseObject, error) {
	run, err := s.svc.GetRun(ctx, principal(ctx).User.ID, req.RunId.String())
	if err != nil {
		return nil, err
	}
	ar, err := toAPIRun(run)
	if err != nil {
		return nil, err
	}
	return api.GetRun200JSONResponse(ar), nil
}

func (s *Server) JoinRun(ctx context.Context, req api.JoinRunRequestObject) (api.JoinRunResponseObject, error) {
	if req.Body == nil {
		return nil, hub.ErrBadRequest
	}
	var jv *hub.JoinVariant
	if v := req.Body.Variant; v != nil {
		jv = &hub.JoinVariant{Kind: string(v.Kind)}
		if v.VariantId != nil {
			jv.VariantID = v.VariantId.String()
		}
	}
	run, err := s.svc.JoinRunWith(ctx, principal(ctx).User.ID, req.RunId.String(), req.Body.GameId.String(), jv)
	if err != nil {
		return nil, err
	}
	ar, err := toAPIRun(run)
	if err != nil {
		return nil, err
	}
	return api.JoinRun200JSONResponse(ar), nil
}

func (s *Server) DeclineRun(ctx context.Context, req api.DeclineRunRequestObject) (api.DeclineRunResponseObject, error) {
	run, err := s.svc.DeclineRun(ctx, principal(ctx).User.ID, req.RunId.String())
	if err != nil {
		return nil, err
	}
	ar, err := toAPIRun(run)
	if err != nil {
		return nil, err
	}
	return api.DeclineRun200JSONResponse(ar), nil
}

func (s *Server) InviteRun(ctx context.Context, req api.InviteRunRequestObject) (api.InviteRunResponseObject, error) {
	if req.Body == nil {
		return nil, hub.ErrBadRequest
	}
	run, err := s.svc.InviteRun(ctx, principal(ctx).User.ID, req.RunId.String(), req.Body.UserIds)
	if err != nil {
		return nil, err
	}
	ar, err := toAPIRun(run)
	if err != nil {
		return nil, err
	}
	return api.InviteRun200JSONResponse(ar), nil
}

func (s *Server) LeaveRun(ctx context.Context, req api.LeaveRunRequestObject) (api.LeaveRunResponseObject, error) {
	run, err := s.svc.LeaveRun(ctx, principal(ctx).User.ID, req.RunId.String())
	if err != nil {
		return nil, err
	}
	ar, err := toAPIRun(run)
	if err != nil {
		return nil, err
	}
	return api.LeaveRun200JSONResponse(ar), nil
}

func (s *Server) SetRunStatus(ctx context.Context, req api.SetRunStatusRequestObject) (api.SetRunStatusResponseObject, error) {
	if req.Body == nil {
		return nil, hub.ErrBadRequest
	}
	run, err := s.svc.SetRunStatus(ctx, principal(ctx).User.ID, req.RunId.String(), string(req.Body.Status))
	if err != nil {
		return nil, err
	}
	ar, err := toAPIRun(run)
	if err != nil {
		return nil, err
	}
	return api.SetRunStatus200JSONResponse(ar), nil
}

func (s *Server) PutRunState(ctx context.Context, req api.PutRunStateRequestObject) (api.PutRunStateResponseObject, error) {
	if req.Body == nil || len(req.Body.State) == 0 {
		return nil, hub.ErrBadRequest
	}
	p := principal(ctx)
	rev, conflict, err := s.svc.PutRunState(ctx, p.User.ID, p.Device.ID, req.RunId.String(), req.PluginId, req.Body.BaseRev, json.RawMessage(req.Body.State))
	if err != nil {
		return nil, err
	}
	if conflict != nil {
		var e api.RunStateConflictError
		e.Error.Code, e.Error.Message = api.ErrorCodeStateConflict, "base_rev is not the current state_rev; merge with the returned state and retry"
		state := api.RunJsonObject(conflict.State)
		e.State, e.StateRev = &state, &conflict.StateRev
		return api.PutRunState409JSONResponse(e), nil
	}
	return api.PutRunState200JSONResponse{StateRev: rev}, nil
}

func (s *Server) AppendRunEvent(ctx context.Context, req api.AppendRunEventRequestObject) (api.AppendRunEventResponseObject, error) {
	if req.Body == nil {
		return nil, hub.ErrBadRequest
	}
	b, p := req.Body, principal(ctx)
	in := hub.AppendRunEventInput{UserID: p.User.ID, DeviceID: p.Device.ID, RunID: req.RunId.String(), ClientID: b.ClientId.String(),
		Source: string(b.Source), Type: b.Type}
	if b.PluginId != nil {
		in.PluginID = *b.PluginId
	}
	if b.Text != nil {
		in.Text = *b.Text
	}
	if b.Payload != nil {
		in.Payload = json.RawMessage(*b.Payload)
	}
	seq, created, err := s.svc.AppendRunEvent(ctx, in)
	if err != nil {
		return nil, err
	}
	if !created {
		return api.AppendRunEvent200JSONResponse{Seq: seq}, nil
	}
	return api.AppendRunEvent201JSONResponse{Seq: seq}, nil
}

func (s *Server) ListRunEvents(ctx context.Context, req api.ListRunEventsRequestObject) (api.ListRunEventsResponseObject, error) {
	var after int64
	limit := hub.DefaultRunEventsPage
	if req.Params.After != nil {
		after = *req.Params.After
	}
	if req.Params.Limit != nil {
		limit = *req.Params.Limit
	}
	evs, last, err := s.svc.ListRunEvents(ctx, principal(ctx).User.ID, req.RunId.String(), after, limit)
	if err != nil {
		return nil, err
	}
	out := api.ListRunEvents200JSONResponse{Events: make([]api.RunEvent, 0, len(evs)), LastSeq: last}
	for _, e := range evs {
		ae, err := toAPIEvent(e)
		if err != nil {
			return nil, err
		}
		out.Events = append(out.Events, ae)
	}
	return out, nil
}

func (s *Server) PutRunBoard(ctx context.Context, req api.PutRunBoardRequestObject) (api.PutRunBoardResponseObject, error) {
	if req.Body == nil || len(req.Body.Tree) == 0 {
		return nil, hub.ErrBadRequest
	}
	rev, err := s.svc.PutRunBoard(ctx, principal(ctx).User.ID, req.RunId.String(), req.PluginId, json.RawMessage(req.Body.Tree))
	if err != nil {
		return nil, err
	}
	return api.PutRunBoard200JSONResponse{Rev: rev}, nil
}

func deref[T any](p *T) (v T) {
	if p != nil {
		v = *p
	}
	return v
}

// GetRunVariant gives a joined participant the recipe to build their variant; the run seed itself stays on the server.
func (s *Server) GetRunVariant(ctx context.Context, req api.GetRunVariantRequestObject) (api.GetRunVariantResponseObject, error) {
	v, err := s.svc.GetRunVariant(ctx, principal(ctx).User.ID, req.RunId.String())
	if err != nil {
		return nil, err
	}
	kind := api.RunVariantVariantKind(v.Kind)
	return api.GetRunVariant200JSONResponse(api.RunVariant{Settings: v.Settings, Seed: v.Seed, ToolId: v.ToolID, ToolVersion: v.ToolVersion,
		BaseSha256: strPtr(v.BaseSHA256), TitleId: strPtr(v.TitleID), Slot: v.Slot, VariantKind: &kind}), nil
}

func (s *Server) SetRunVariantState(ctx context.Context, req api.SetRunVariantStateRequestObject) (api.SetRunVariantStateResponseObject, error) {
	if req.Body == nil {
		return nil, hub.ErrBadRequest
	}
	run, err := s.svc.SetRunVariantState(ctx, principal(ctx).User.ID, req.RunId.String(), string(req.Body.State))
	if err != nil {
		return nil, err
	}
	ar, err := toAPIRun(run)
	if err != nil {
		return nil, err
	}
	return api.SetRunVariantState200JSONResponse(ar), nil
}

func (s *Server) SetRunRules(ctx context.Context, req api.SetRunRulesRequestObject) (api.SetRunRulesResponseObject, error) {
	if req.Body == nil {
		return nil, hub.ErrBadRequest
	}
	p := principal(ctx)
	b := req.Body
	run, err := s.svc.SetRunRules(ctx, p.User.ID, req.RunId.String(), hub.RunRules{States: b.States, Rewind: b.Rewind,
		Cheats: string(b.Cheats), MemoryWrite: string(b.MemoryWrite)}, p.User.Role == hub.RoleAdmin)
	if err != nil {
		return nil, err
	}
	ar, err := toAPIRun(run)
	if err != nil {
		return nil, err
	}
	return api.SetRunRules200JSONResponse(ar), nil
}
