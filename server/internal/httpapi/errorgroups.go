package httpapi

import (
	"context"
	"encoding/json"

	"github.com/phabioo/uzume/server/internal/api"
	"github.com/phabioo/uzume/server/internal/hub"
)

// Error events and error groups (feature reports_v2, ADR 0035): any paired device sends events, admins read the
// groups. Admin tokens only for the admin endpoints: 401 without a token, 403 for users.

func (s *Server) CreateErrorEvents(ctx context.Context, req api.CreateErrorEventsRequestObject) (api.CreateErrorEventsResponseObject, error) {
	if req.Body == nil {
		return nil, hub.ErrBadRequest
	}
	evs := make([]hub.ErrorEventInput, 0, len(req.Body.Events))
	for _, e := range req.Body.Events {
		evs = append(evs, hub.ErrorEventInput{Kind: string(e.Kind), Component: e.Component, Fingerprint: e.Fingerprint, Title: e.Title,
			Message: e.Message, AppVersion: e.AppVersion, OS: e.Os, OccurredAt: e.OccurredAt})
	}
	accepted, dropped, err := s.svc.RecordErrorEvents(ctx, principal(ctx), evs)
	if err != nil {
		return nil, err
	}
	if dropped > 0 {
		s.log.Info("error events dropped by the hourly limit", "device", principal(ctx).Device.Name, "dropped", dropped) // no text
	}
	return api.CreateErrorEvents202JSONResponse{Accepted: accepted, Dropped: dropped}, nil
}

func errorGroupBody(g hub.ErrorGroup) api.ErrorGroup {
	return api.ErrorGroup{Fingerprint: g.Fingerprint, Kind: api.ErrorGroupKind(g.Kind), Component: g.Component, Title: g.Title,
		Count: g.Count, Devices: g.Devices, FirstSeen: g.FirstSeen, LastSeen: g.LastSeen, Versions: g.Versions,
		IssueUrl: g.IssueURL, Seen: g.Seen, IssuesBase: g.IssuesBase}
}

func (s *Server) ListAdminErrorGroups(ctx context.Context, _ api.ListAdminErrorGroupsRequestObject) (api.ListAdminErrorGroupsResponseObject, error) {
	if err := requireAdmin(ctx); err != nil {
		return nil, err
	}
	gs, unseen, err := s.svc.ListErrorGroups(ctx)
	if err != nil {
		return nil, err
	}
	out := api.ErrorGroupList{Groups: make([]api.ErrorGroup, 0, len(gs)), New: unseen}
	for _, g := range gs {
		out.Groups = append(out.Groups, errorGroupBody(g))
	}
	return api.ListAdminErrorGroups200JSONResponse(out), nil
}

func (s *Server) GetAdminErrorGroup(ctx context.Context, req api.GetAdminErrorGroupRequestObject) (api.GetAdminErrorGroupResponseObject, error) {
	if err := requireAdmin(ctx); err != nil {
		return nil, err
	}
	if !hub.ValidFingerprint(req.Fingerprint) {
		return nil, hub.ErrNotFound
	}
	d, err := s.svc.GetErrorGroup(ctx, req.Fingerprint)
	if err != nil {
		return nil, err
	}
	g := errorGroupBody(d.ErrorGroup)
	out := api.ErrorGroupDetail{Fingerprint: g.Fingerprint, Kind: api.ErrorGroupDetailKind(g.Kind), Component: g.Component, Title: g.Title,
		Count: g.Count, Devices: g.Devices, FirstSeen: g.FirstSeen, LastSeen: g.LastSeen, Versions: g.Versions, IssueUrl: g.IssueUrl,
		Seen: g.Seen, IssuesBase: g.IssuesBase, PublicTitle: d.PublicTitle, PublicSummary: d.PublicSummary,
		Events: make([]api.ErrorGroupEvent, 0, len(d.Events))}
	for _, e := range d.Events {
		out.Events = append(out.Events, api.ErrorGroupEvent{OccurredAt: e.OccurredAt, AppVersion: e.AppVersion, Os: e.OS,
			DeviceName: e.DeviceName, UserName: e.UserName, Message: e.Message})
	}
	return api.GetAdminErrorGroup200JSONResponse(out), nil
}

func (s *Server) PatchAdminErrorGroup(ctx context.Context, req api.PatchAdminErrorGroupRequestObject) (api.PatchAdminErrorGroupResponseObject, error) {
	if err := requireAdmin(ctx); err != nil {
		return nil, err
	}
	if req.Body == nil {
		return nil, hub.ErrBadRequest
	}
	if !hub.ValidFingerprint(req.Fingerprint) {
		return nil, hub.ErrNotFound
	}
	var u hub.ErrorGroupUpdate
	if raw := req.Body.IssueUrl; len(raw) > 0 {
		u.IssueURLSet = true
		if string(raw) != "null" {
			var v string
			if json.Unmarshal(raw, &v) != nil {
				return nil, &hub.Error{Code: hub.CodeBadRequest, Message: "issue_url must be a string or null"}
			}
			u.IssueURL = &v
		}
	}
	if req.Body.Seen != nil {
		if !bool(*req.Body.Seen) {
			return nil, &hub.Error{Code: hub.CodeBadRequest, Message: "seen can only be true"}
		}
		u.Seen = true
	}
	g, err := s.svc.UpdateErrorGroup(ctx, req.Fingerprint, u)
	if err != nil {
		return nil, err
	}
	return api.PatchAdminErrorGroup200JSONResponse(errorGroupBody(g)), nil
}

func (s *Server) DeleteAdminErrorGroup(ctx context.Context, req api.DeleteAdminErrorGroupRequestObject) (api.DeleteAdminErrorGroupResponseObject, error) {
	if err := requireAdmin(ctx); err != nil {
		return nil, err
	}
	if !hub.ValidFingerprint(req.Fingerprint) {
		return nil, hub.ErrNotFound
	}
	if err := s.svc.DeleteErrorGroup(ctx, req.Fingerprint); err != nil {
		return nil, err
	}
	s.log.Info("error group deleted", "fingerprint", req.Fingerprint, "admin", principal(ctx).User.Username)
	return api.DeleteAdminErrorGroup204Response{}, nil
}
