package httpapi

import (
	"context"

	"github.com/phabioo/uzume/server/internal/api"
	"github.com/phabioo/uzume/server/internal/hub"
)

func toAPISession(s hub.Session) api.Session {
	out := api.Session{SessionId: mustUUID(s.SessionID), GameId: mustUUID(s.GameID), GameTitle: s.GameTitle,
		Owner:      api.SessionOwner{UserId: s.Owner.UserID, DisplayName: s.Owner.DisplayName, DeviceName: s.Owner.DeviceName},
		Visibility: api.SessionVisibility(s.Visibility), CreatedAt: s.CreatedAt, ViewerCount: s.ViewerCount,
		IsOwner: s.IsOwner, Invited: s.Invited}
	if s.Viewers != nil {
		vs := make([]api.SessionViewerInfo, 0, len(*s.Viewers))
		for _, v := range *s.Viewers {
			vs = append(vs, api.SessionViewerInfo{ViewerId: mustUUID(v.ViewerID), DisplayName: v.DisplayName, DeviceName: v.DeviceName})
		}
		out.Viewers = &vs
	}
	if s.Invites != nil {
		is := make([]api.SessionInviteInfo, 0, len(*s.Invites))
		for _, i := range *s.Invites {
			is = append(is, api.SessionInviteInfo{UserId: i.UserID, DisplayName: i.DisplayName,
				State: api.SessionInviteInfoState(i.State), Online: i.Online})
		}
		out.Invites = &is
	}
	return out
}

func (s *Server) ListUsers(ctx context.Context, _ api.ListUsersRequestObject) (api.ListUsersResponseObject, error) {
	us, err := s.svc.UsersWithPresence(ctx)
	if err != nil {
		return nil, err
	}
	out := api.ListUsers200JSONResponse{Users: make([]api.UserInfo, 0, len(us))}
	for _, u := range us {
		out.Users = append(out.Users, api.UserInfo{Id: u.ID, DisplayName: u.DisplayName, Online: u.Online})
	}
	return out, nil
}

func (s *Server) ListSessions(ctx context.Context, _ api.ListSessionsRequestObject) (api.ListSessionsResponseObject, error) {
	ss, err := s.svc.ListSessions(ctx, principal(ctx))
	if err != nil {
		return nil, err
	}
	out := api.ListSessions200JSONResponse{Sessions: make([]api.Session, 0, len(ss))}
	for _, x := range ss {
		out.Sessions = append(out.Sessions, toAPISession(x))
	}
	return out, nil
}

func (s *Server) PublishSession(ctx context.Context, req api.PublishSessionRequestObject) (api.PublishSessionResponseObject, error) {
	if req.Body == nil {
		return nil, hub.ErrBadRequest
	}
	x, err := s.svc.PublishSession(ctx, principal(ctx), req.Body.GameId.String(), hub.Visibility(req.Body.Visibility))
	if err != nil {
		return nil, err
	}
	return api.PublishSession201JSONResponse(toAPISession(x)), nil
}

func (s *Server) GetSession(ctx context.Context, req api.GetSessionRequestObject) (api.GetSessionResponseObject, error) {
	x, err := s.svc.GetSession(ctx, principal(ctx), req.SessionId.String())
	if err != nil {
		return nil, err
	}
	return api.GetSession200JSONResponse(toAPISession(x)), nil
}

func (s *Server) UpdateSession(ctx context.Context, req api.UpdateSessionRequestObject) (api.UpdateSessionResponseObject, error) {
	if req.Body == nil {
		return nil, hub.ErrBadRequest
	}
	x, err := s.svc.SetSessionVisibility(ctx, principal(ctx), req.SessionId.String(), hub.Visibility(req.Body.Visibility))
	if err != nil {
		return nil, err
	}
	return api.UpdateSession200JSONResponse(toAPISession(x)), nil
}

func (s *Server) EndSession(ctx context.Context, req api.EndSessionRequestObject) (api.EndSessionResponseObject, error) {
	if err := s.svc.EndSession(ctx, principal(ctx), req.SessionId.String()); err != nil {
		return nil, err
	}
	return api.EndSession204Response{}, nil
}

func (s *Server) InviteSessionUser(ctx context.Context, req api.InviteSessionUserRequestObject) (api.InviteSessionUserResponseObject, error) {
	x, err := s.svc.InviteUser(ctx, principal(ctx), req.SessionId.String(), req.UserId)
	if err != nil {
		return nil, err
	}
	return api.InviteSessionUser200JSONResponse(toAPISession(x)), nil
}

func (s *Server) WithdrawSessionInvite(ctx context.Context, req api.WithdrawSessionInviteRequestObject) (api.WithdrawSessionInviteResponseObject, error) {
	if err := s.svc.WithdrawInvite(ctx, principal(ctx), req.SessionId.String(), req.UserId); err != nil {
		return nil, err
	}
	return api.WithdrawSessionInvite204Response{}, nil
}

func (s *Server) DeclineSession(ctx context.Context, req api.DeclineSessionRequestObject) (api.DeclineSessionResponseObject, error) {
	if err := s.svc.DeclineInvite(ctx, principal(ctx), req.SessionId.String()); err != nil {
		return nil, err
	}
	return api.DeclineSession204Response{}, nil
}

func (s *Server) JoinSession(ctx context.Context, req api.JoinSessionRequestObject) (api.JoinSessionResponseObject, error) {
	r, err := s.svc.JoinSession(ctx, principal(ctx), req.SessionId.String())
	if err != nil {
		return nil, err
	}
	out := api.JoinSession201JSONResponse{ViewerId: mustUUID(r.ViewerID), IceServers: r.ICEServers,
		Permissions: api.SessionPermissions{ViewVideo: r.Permissions.ViewVideo, HearAudio: r.Permissions.HearAudio, SendInput: r.Permissions.SendInput}}
	if len(r.TURN) > 0 {
		ts := make([]api.TurnServer, 0, len(r.TURN))
		for _, t := range r.TURN {
			ts1 := api.TurnServer{Urls: t.URLs, Username: t.Username, Credential: t.Credential}
			if !t.ExpiresAt.IsZero() {
				exp := t.ExpiresAt
				ts1.ExpiresAt = &exp
			}
			ts = append(ts, ts1)
		}
		out.TurnServers = &ts
	}
	return out, nil
}

func (s *Server) RemoveSessionViewer(ctx context.Context, req api.RemoveSessionViewerRequestObject) (api.RemoveSessionViewerResponseObject, error) {
	if err := s.svc.RemoveViewer(ctx, principal(ctx), req.SessionId.String(), req.ViewerId.String()); err != nil {
		return nil, err
	}
	return api.RemoveSessionViewer204Response{}, nil
}

func (s *Server) ListSessionChat(ctx context.Context, req api.ListSessionChatRequestObject) (api.ListSessionChatResponseObject, error) {
	msgs, err := s.svc.ListSessionChat(ctx, principal(ctx), req.SessionId.String())
	if err != nil {
		return nil, err
	}
	return api.ListSessionChat200JSONResponse{Messages: toAPIChat(msgs)}, nil
}
