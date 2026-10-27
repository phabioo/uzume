package httpapi

import (
	"context"

	"github.com/google/uuid"

	"github.com/phabioo/uzume/server/internal/api"
	"github.com/phabioo/uzume/server/internal/hub"
)

// Game variants REST API (feature variants_v1, 0.21). Variants belong to their owner; every read and write is scoped
// to the caller, other people's variants do not exist for them (404).

func apiVariant(v hub.Variant) api.Variant {
	id, _ := uuid.Parse(v.ID)
	gid, _ := uuid.Parse(v.GameID)
	out := api.Variant{Id: id, GameId: gid, Name: v.Name, Slot: v.Slot, ToolId: v.ToolID, ToolVersion: v.ToolVersion, Settings: v.Settings,
		Seed: v.Seed, BaseSha256: strPtr(v.BaseSHA256), TitleId: strPtr(v.TitleID), UpdateVersion: strPtr(v.UpdateVersion),
		SpoilerLog: v.SpoilerLog, CreatedAt: v.CreatedAt, DeletedAt: v.DeletedAt}
	if v.RunID != nil {
		if rid, err := uuid.Parse(*v.RunID); err == nil {
			out.RunId = &rid
		}
	}
	return out
}

func apiVariants(vs []hub.Variant) api.VariantList {
	out := api.VariantList{Variants: make([]api.Variant, 0, len(vs))}
	for _, v := range vs {
		out.Variants = append(out.Variants, apiVariant(v))
	}
	return out
}

func (s *Server) ListVariants(ctx context.Context, req api.ListVariantsRequestObject) (api.ListVariantsResponseObject, error) {
	incl := req.Params.IncludeDeleted != nil && *req.Params.IncludeDeleted == 1
	vs, err := s.svc.ListVariants(ctx, principal(ctx).User.ID, "", incl)
	if err != nil {
		return nil, err
	}
	return api.ListVariants200JSONResponse(apiVariants(vs)), nil
}

func (s *Server) ListGameVariants(ctx context.Context, req api.ListGameVariantsRequestObject) (api.ListGameVariantsResponseObject, error) {
	uid := principal(ctx).User.ID
	if _, err := s.svc.GetGameFor(ctx, uid, req.GameId.String()); err != nil {
		return nil, err
	}
	incl := req.Params.IncludeDeleted != nil && *req.Params.IncludeDeleted == 1
	vs, err := s.svc.ListVariants(ctx, uid, req.GameId.String(), incl)
	if err != nil {
		return nil, err
	}
	return api.ListGameVariants200JSONResponse(apiVariants(vs)), nil
}

func (s *Server) CreateVariant(ctx context.Context, req api.CreateVariantRequestObject) (api.CreateVariantResponseObject, error) {
	if req.Body == nil {
		return nil, hub.ErrBadRequest
	}
	b := req.Body
	v, err := s.svc.CreateVariant(ctx, hub.CreateVariantInput{UserID: principal(ctx).User.ID, GameID: req.GameId.String(), Name: b.Name,
		ToolID: b.ToolId, ToolVersion: b.ToolVersion, Settings: b.Settings, Seed: b.Seed, BaseSHA256: deref(b.BaseSha256), TitleID: deref(b.TitleId),
		UpdateVer: deref(b.UpdateVersion), SpoilerLog: deref(b.SpoilerLog)})
	if err != nil {
		return nil, err
	}
	return api.CreateVariant201JSONResponse(apiVariant(v)), nil
}

func (s *Server) RenameVariant(ctx context.Context, req api.RenameVariantRequestObject) (api.RenameVariantResponseObject, error) {
	if req.Body == nil {
		return nil, hub.ErrBadRequest
	}
	v, err := s.svc.RenameVariant(ctx, principal(ctx).User.ID, req.VariantId.String(), req.Body.Name)
	if err != nil {
		return nil, err
	}
	return api.RenameVariant200JSONResponse(apiVariant(v)), nil
}

func (s *Server) DeleteVariant(ctx context.Context, req api.DeleteVariantRequestObject) (api.DeleteVariantResponseObject, error) {
	p := principal(ctx)
	deleteSaves := false
	if sv := req.Params.Saves; sv != nil {
		switch *sv {
		case api.DeleteVariantParamsSavesDelete:
			deleteSaves = true
		case api.DeleteVariantParamsSavesKeep:
		default:
			return nil, hub.ErrBadRequest
		}
	}
	if err := s.svc.DeleteVariant(ctx, p.User.ID, p.Device.ID, req.VariantId.String(), deleteSaves); err != nil {
		return nil, err
	}
	return api.DeleteVariant204Response{}, nil
}
