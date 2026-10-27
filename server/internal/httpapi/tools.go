package httpapi

import (
	"context"
	"errors"

	"github.com/phabioo/uzume/server/internal/api"
	"github.com/phabioo/uzume/server/internal/hub"
)

// Tools catalog REST API (feature tools_v1, 0.21): the app side lists offered tools and downloads their zips; the
// admin side manages versions (sync, install, remove) and the offered switch.

func apiToolPlatform(p hub.ToolPlatform, admin bool) api.ToolPlatformEntry {
	out := api.ToolPlatformEntry{Platform: api.ToolPlatformName(p.Platform), Size: p.Size, Sha256: p.SHA256, Cached: p.Cached, Root: ptr(p.Root)}
	if admin {
		out.Origin = ptr(api.ToolPlatformEntryOrigin(p.Origin))
		out.Url = strPtr(p.URL)
	}
	return out
}

func apiToolUsage(u hub.ToolUsage) api.ToolUsage {
	return api.ToolUsage{Variants: u.Variants, Runs: u.Runs}
}

func (s *Server) ListTools(ctx context.Context, _ api.ListToolsRequestObject) (api.ListToolsResponseObject, error) {
	ts, err := s.svc.AppTools(ctx)
	if err != nil {
		return nil, err
	}
	out := api.ListTools200JSONResponse{Tools: make([]api.Tool, 0, len(ts))}
	for _, t := range ts {
		at := api.Tool{Id: t.ID, Name: t.Name, Offered: t.Offered, Version: t.NewestCached(), License: t.License, SourceUrl: t.SourceURL,
			Versions: make([]api.ToolVersionEntry, 0, len(t.Versions))}
		for _, v := range t.Versions {
			av := api.ToolVersionEntry{Version: v.Version, UsedBy: apiToolUsage(v.UsedBy), Platforms: make([]api.ToolPlatformEntry, 0, len(v.Platforms))}
			for _, p := range v.Platforms {
				av.Platforms = append(av.Platforms, apiToolPlatform(p, false))
			}
			at.Versions = append(at.Versions, av)
		}
		out.Tools = append(out.Tools, at)
	}
	return out, nil
}

// GetToolPackage streams a cached tool zip (ETag = "<sha256>", If-None-Match answers 304). The file is never loaded
// into memory.
func (s *Server) GetToolPackage(ctx context.Context, req api.GetToolPackageRequestObject) (api.GetToolPackageResponseObject, error) {
	f, pl, err := s.svc.OpenToolFile(ctx, req.ToolId, req.Version, string(req.Platform))
	if err != nil {
		return nil, err
	}
	etag := `"` + pl.SHA256 + `"`
	if req.Params.IfNoneMatch != nil && etagMatches(*req.Params.IfNoneMatch, etag) {
		f.Close()
		return api.GetToolPackage304Response{Headers: api.GetToolPackage304ResponseHeaders{ETag: ptr(etag)}}, nil
	}
	return api.GetToolPackage200ApplicationzipResponse{Body: f, ContentLength: pl.Size,
		Headers: api.GetToolPackage200ResponseHeaders{ContentLength: ptr(pl.Size), ETag: ptr(etag)}}, nil
}

func adminToolBody(t hub.Tool) api.AdminTool {
	out := api.AdminTool{Id: t.ID, Name: t.Name, License: t.License, SourceUrl: t.SourceURL, Offered: t.Offered,
		LatestVersion: strPtr(t.LatestVersion), Versions: make([]api.AdminToolVersion, 0, len(t.Versions)),
		Source: api.AdminToolSource{State: api.AdminToolSourceState(t.Source.State), Url: strPtr(t.Source.URL), LastCheck: t.Source.LastCheck,
			LastSuccess: t.Source.LastSuccess, LastError: strPtr(t.Source.LastError)}}
	out.UpdateAvailable = ptr(t.UpdateAvailable())
	for _, v := range t.Versions {
		av := api.AdminToolVersion{Version: v.Version, Installed: v.Installed, Complete: v.Complete, UsedBy: apiToolUsage(v.UsedBy),
			Platforms: make([]api.ToolPlatformEntry, 0, len(v.Platforms))}
		for _, p := range v.Platforms {
			av.Platforms = append(av.Platforms, apiToolPlatform(p, true))
		}
		out.Versions = append(out.Versions, av)
	}
	return out
}

func (s *Server) ListAdminTools(ctx context.Context, _ api.ListAdminToolsRequestObject) (api.ListAdminToolsResponseObject, error) {
	if err := requireAdmin(ctx); err != nil {
		return nil, err
	}
	ts, err := s.svc.ListTools(ctx)
	if err != nil {
		return nil, err
	}
	out := api.ListAdminTools200JSONResponse{Tools: make([]api.AdminTool, 0, len(ts))}
	for _, t := range ts {
		out.Tools = append(out.Tools, adminToolBody(t))
	}
	return out, nil
}

func (s *Server) UpdateAdminTool(ctx context.Context, req api.UpdateAdminToolRequestObject) (api.UpdateAdminToolResponseObject, error) {
	if err := requireAdmin(ctx); err != nil {
		return nil, err
	}
	if req.Body == nil {
		return nil, hub.ErrBadRequest
	}
	t, err := s.svc.SetToolOffered(ctx, req.ToolId, req.Body.Offered)
	if err != nil {
		return nil, err
	}
	return api.UpdateAdminTool200JSONResponse(adminToolBody(t)), nil
}

func (s *Server) SyncAdminTool(ctx context.Context, req api.SyncAdminToolRequestObject) (api.SyncAdminToolResponseObject, error) {
	if err := requireAdmin(ctx); err != nil {
		return nil, err
	}
	t, err := s.svc.SyncTool(ctx, req.ToolId)
	var he *hub.Error
	switch {
	case err == nil:
	case errors.As(err, &he):
		return nil, err
	default:
		// A failed check is stored and shown in source.last_error, not an HTTP error.
		s.log.Warn("tools sync failed", "tool", req.ToolId, "err", err)
		t, err = s.svc.GetTool(ctx, req.ToolId)
		if err != nil {
			return nil, err
		}
	}
	return api.SyncAdminTool200JSONResponse(adminToolBody(t)), nil
}

func (s *Server) InstallAdminToolVersion(ctx context.Context, req api.InstallAdminToolVersionRequestObject) (api.InstallAdminToolVersionResponseObject, error) {
	if err := requireAdmin(ctx); err != nil {
		return nil, err
	}
	t, err := s.svc.InstallToolVersion(ctx, req.ToolId, req.Version)
	var he *hub.Error
	switch {
	case err == nil:
	case errors.As(err, &he):
		return nil, err
	default:
		// Download problems are shown to the admin; the details stay in the server log too.
		s.log.Warn("tool install failed", "tool", req.ToolId, "version", req.Version, "err", err)
		return nil, &hub.Error{Code: hub.CodeInternal, Message: "The tool could not be installed: " + err.Error()}
	}
	return api.InstallAdminToolVersion200JSONResponse(adminToolBody(t)), nil
}

func (s *Server) RemoveAdminToolVersion(ctx context.Context, req api.RemoveAdminToolVersionRequestObject) (api.RemoveAdminToolVersionResponseObject, error) {
	if err := requireAdmin(ctx); err != nil {
		return nil, err
	}
	err := s.svc.RemoveToolVersion(ctx, req.ToolId, req.Version)
	var he *hub.Error
	if errors.As(err, &he) && he.Code == hub.CodeToolInUse && he.UsedBy != nil {
		var out api.RemoveAdminToolVersion409JSONResponse
		out.Error.Code, out.Error.Message, out.UsedBy = api.ToolInUse, he.Message, apiToolUsage(*he.UsedBy)
		return out, nil
	}
	if err != nil {
		return nil, err
	}
	return api.RemoveAdminToolVersion204Response{}, nil
}
