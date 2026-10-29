package httpapi

import (
	"context"
	"strings"

	"github.com/phabioo/uzume/server/internal/api"
	"github.com/phabioo/uzume/server/internal/hub"
)

// Cheat database (0.20 package 3, feature cheat_db_v1): the list of a game for every signed-in device, the state and
// the source setting for admins.

func (s *Server) GetGameCheats(ctx context.Context, req api.GetGameCheatsRequestObject) (api.GetGameCheatsResponseObject, error) {
	l, err := s.svc.GameCheats(ctx, principal(ctx).User.ID, req.GameId.String())
	if err != nil {
		return nil, err
	}
	out := api.GetGameCheats200JSONResponse{Source: api.GameCheatsSource(l.Source), UpdatedAt: l.UpdatedAt, Cheats: make([]api.GameCheat, 0, len(l.Cheats))}
	if l.MatchedBy != "" {
		m := api.GameCheatsMatchedBy(l.MatchedBy)
		out.MatchedBy = &m
		if l.ListName != "" {
			out.ListName = &l.ListName
		}
	}
	if len(l.SimilarLists) > 0 {
		out.SimilarLists = &l.SimilarLists
	}
	for _, c := range l.Cheats {
		out.Cheats = append(out.Cheats, api.GameCheat{Name: c.Name, Code: c.Code, FormatGuess: api.CheatFormat(c.FormatGuess)})
	}
	return out, nil
}

func (s *Server) adminCheatDb(ctx context.Context) (api.AdminCheatDb, error) {
	st, err := s.svc.CheatDBStatus(ctx)
	if err != nil {
		return api.AdminCheatDb{}, err
	}
	out := api.AdminCheatDb{Url: st.URL, Url3ds: st.URL3DS, Lists3ds: st.Lists3DS, Enabled: st.Enabled, UpdatedAt: st.UpdatedAt, SizeBytes: st.SizeBytes, Lists: st.Lists, Dats: st.Dats,
		LastCheck: st.LastCheck, LastSuccess: st.LastSuccess}
	if st.Source != "" {
		src := api.AdminCheatDbSource(st.Source)
		out.Source = &src
	}
	if st.LastError != "" {
		out.LastError = &st.LastError
	}
	return out, nil
}

func (s *Server) GetAdminCheatDb(ctx context.Context, _ api.GetAdminCheatDbRequestObject) (api.GetAdminCheatDbResponseObject, error) {
	if err := requireAdmin(ctx); err != nil {
		return nil, err
	}
	out, err := s.adminCheatDb(ctx)
	if err != nil {
		return nil, err
	}
	return api.GetAdminCheatDb200JSONResponse(out), nil
}

func (s *Server) UpdateAdminCheatDb(ctx context.Context, req api.UpdateAdminCheatDbRequestObject) (api.UpdateAdminCheatDbResponseObject, error) {
	if err := requireAdmin(ctx); err != nil {
		return nil, err
	}
	if req.Body == nil {
		return nil, hub.ErrBadRequest
	}
	if err := s.requireConfirmed(ctx); err != nil {
		return nil, err
	}
	if req.Body.Url == nil && req.Body.Url3ds == nil {
		return nil, hub.ErrBadRequest
	}
	if req.Body.Url != nil {
		if !hub.ValidCheatDBURL(strings.TrimSpace(*req.Body.Url)) {
			return nil, &hub.Error{Code: hub.CodeBadRequest, Message: "The cheat database URL must be https or \"none\""}
		}
	}
	if req.Body.Url3ds != nil {
		if !hub.ValidCheatDBURL(strings.TrimSpace(*req.Body.Url3ds)) {
			return nil, &hub.Error{Code: hub.CodeBadRequest, Message: "The 3DS cheat URL must be https or \"none\""}
		}
		if err := s.svc.SetCheatDB3DSURL(ctx, *req.Body.Url3ds); err != nil {
			return nil, err
		}
	}
	if req.Body.Url != nil {
		if err := s.svc.SetCheatDBURL(ctx, *req.Body.Url); err != nil {
			return nil, err
		}
	}
	out, err := s.adminCheatDb(ctx)
	if err != nil {
		return nil, err
	}
	return api.UpdateAdminCheatDb200JSONResponse(out), nil
}

func (s *Server) RefreshAdminCheatDb(ctx context.Context, _ api.RefreshAdminCheatDbRequestObject) (api.RefreshAdminCheatDbResponseObject, error) {
	if err := requireAdmin(ctx); err != nil {
		return nil, err
	}
	u, err := s.svc.CheatDBURL(ctx)
	if err != nil {
		return nil, err
	}
	u3, err := s.svc.CheatDB3DSURL(ctx)
	if err != nil {
		return nil, err
	}
	if u == hub.CheatDBOff && u3 == hub.CheatDBOff {
		return nil, &hub.Error{Code: hub.CodeBadRequest, Message: "The cheat database is switched off"}
	}
	s.svc.TriggerCheatDBRefresh()
	return api.RefreshAdminCheatDb202Response{}, nil
}
