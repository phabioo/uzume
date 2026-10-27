package httpapi

import (
	"context"
	"errors"

	"github.com/phabioo/uzume/server/internal/api"
	"github.com/phabioo/uzume/server/internal/hub"
)

// Admin API for plugins and the plugin index (0.18, `admin_v1`). The handlers call the same hub functions as the
// Plugins screen of the app.

func adminPluginBody(p hub.Plugin) api.AdminPlugin {
	return api.AdminPlugin{Info: pluginInfo(p), Source: api.AdminPluginSource(p.Source), UploadedByName: strPtr(p.UploadedByName)}
}

func adminPluginIndexBody(st hub.PluginIndexStatus) api.AdminPluginIndex {
	out := api.AdminPluginIndex{Configured: st.Configured, LastCheck: st.LastCheck, LastError: strPtr(st.LastError),
		Catalog: make([]api.AdminPluginCatalogEntry, 0, len(st.Catalog))}
	for _, c := range st.Catalog {
		out.Catalog = append(out.Catalog, api.AdminPluginCatalogEntry{Id: c.ID, Name: c.Name, Version: c.Version, Api: c.API,
			Description: c.Description, Permissions: nz(c.Permissions), Size: c.Size, State: api.AdminPluginCatalogEntryState(c.State),
			InstalledVersion: strPtr(c.InstalledVersion)})
	}
	return out
}

func (s *Server) ListAdminPlugins(ctx context.Context, _ api.ListAdminPluginsRequestObject) (api.ListAdminPluginsResponseObject, error) {
	if err := requireAdmin(ctx); err != nil {
		return nil, err
	}
	ps, err := s.svc.ListPlugins(ctx)
	if err != nil {
		return nil, err
	}
	st, err := s.svc.PluginIndexStatus(ctx)
	if err != nil {
		return nil, err
	}
	out := api.AdminPlugins{Plugins: make([]api.AdminPlugin, 0, len(ps)), Index: adminPluginIndexBody(st)}
	for _, p := range ps {
		out.Plugins = append(out.Plugins, adminPluginBody(p))
	}
	return api.ListAdminPlugins200JSONResponse(out), nil
}

// StageAdminPlugin takes the raw .uzplugin body (the request cap for this route is the plugin file limit).
func (s *Server) StageAdminPlugin(ctx context.Context, req api.StageAdminPluginRequestObject) (api.StageAdminPluginResponseObject, error) {
	if err := requireAdmin(ctx); err != nil {
		return nil, err
	}
	st, err := s.svc.StagePluginUpload(ctx, req.Body)
	if err != nil {
		return nil, err
	}
	return api.StageAdminPlugin201JSONResponse(api.AdminPluginStage{StageId: st.StageID, PluginId: st.PluginID, Name: st.Name, Version: st.Version,
		Description: st.Description, Permissions: nz(st.Permissions), Official: st.Official, Signed: st.Signed,
		SignatureProblem: strPtr(st.SignatureProblem), Sha256: st.SHA256, Size: st.Size, AlreadyOffered: st.AlreadyOffered, Conflict: st.Conflict}), nil
}

func (s *Server) OfferAdminPlugin(ctx context.Context, req api.OfferAdminPluginRequestObject) (api.OfferAdminPluginResponseObject, error) {
	if err := requireAdmin(ctx); err != nil {
		return nil, err
	}
	if req.Body == nil {
		return nil, hub.ErrBadRequest
	}
	p, err := s.svc.OfferStagedPlugin(ctx, req.StageId, req.Body.GrantedPermissions, principal(ctx).User.ID)
	if err != nil {
		return nil, err
	}
	return api.OfferAdminPlugin201JSONResponse(adminPluginBody(p)), nil
}

func (s *Server) DiscardAdminPlugin(ctx context.Context, req api.DiscardAdminPluginRequestObject) (api.DiscardAdminPluginResponseObject, error) {
	if err := requireAdmin(ctx); err != nil {
		return nil, err
	}
	s.svc.DiscardStagedPlugin(req.StageId)
	return api.DiscardAdminPlugin204Response{}, nil
}

func (s *Server) RemoveAdminPlugin(ctx context.Context, req api.RemoveAdminPluginRequestObject) (api.RemoveAdminPluginResponseObject, error) {
	if err := requireAdmin(ctx); err != nil {
		return nil, err
	}
	if err := s.svc.RemovePlugin(ctx, req.PluginId, req.Version); err != nil {
		return nil, err
	}
	return api.RemoveAdminPlugin204Response{}, nil
}

var errNoPluginIndex = &hub.Error{Code: hub.CodeBadRequest, Message: "No plugin index is configured"}

func (s *Server) RefreshAdminPluginIndex(ctx context.Context, _ api.RefreshAdminPluginIndexRequestObject) (api.RefreshAdminPluginIndexResponseObject, error) {
	if err := requireAdmin(ctx); err != nil {
		return nil, err
	}
	switch err := s.svc.RefreshPluginIndex(ctx); {
	case errors.Is(err, hub.ErrPluginIndexOff):
		return nil, errNoPluginIndex
	case err != nil:
		// The reason is stored and comes back in index.last_error.
		s.log.Warn("plugin index refresh failed", "err", err)
	}
	st, err := s.svc.PluginIndexStatus(ctx)
	if err != nil {
		return nil, err
	}
	return api.RefreshAdminPluginIndex200JSONResponse(adminPluginIndexBody(st)), nil
}

func (s *Server) InstallAdminIndexPlugin(ctx context.Context, req api.InstallAdminIndexPluginRequestObject) (api.InstallAdminIndexPluginResponseObject, error) {
	if err := requireAdmin(ctx); err != nil {
		return nil, err
	}
	if req.Body == nil {
		return nil, hub.ErrBadRequest
	}
	p, err := s.svc.InstallIndexPlugin(ctx, req.Body.Id, req.Body.Version)
	var he *hub.Error
	switch {
	case err == nil:
	case errors.Is(err, hub.ErrPluginIndexOff):
		return nil, errNoPluginIndex
	case errors.As(err, &he):
		return nil, err
	default:
		// Download, checksum or signature problems are shown to the admin; details stay in the server log.
		s.log.Warn("plugin install from the index failed", "plugin", req.Body.Id, "version", req.Body.Version, "err", err)
		return nil, &hub.Error{Code: hub.CodeInternal, Message: "The plugin could not be installed: " + err.Error()}
	}
	return api.InstallAdminIndexPlugin201JSONResponse(adminPluginBody(p)), nil
}
