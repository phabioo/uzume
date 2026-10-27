package httpapi

import (
	"context"
	"sort"

	"github.com/phabioo/uzume/server/internal/api"
	"github.com/phabioo/uzume/server/internal/hub"
)

// ListPlugins lists every offered plugin version (`plugins_v1`).
func (s *Server) ListPlugins(ctx context.Context, _ api.ListPluginsRequestObject) (api.ListPluginsResponseObject, error) {
	ps, err := s.svc.ListPlugins(ctx)
	if err != nil {
		return nil, err
	}
	out := api.PluginList{Plugins: make([]api.PluginInfo, 0, len(ps))}
	for _, p := range ps {
		out.Plugins = append(out.Plugins, pluginInfo(p))
	}
	return api.ListPlugins200JSONResponse(out), nil
}

func pluginInfo(p hub.Plugin) api.PluginInfo {
	m := p.Manifest
	info := api.PluginInfo{PluginId: p.PluginID, Version: p.Version, Name: p.Name, Description: p.Description, Api: p.API,
		Origin: api.PluginOrigin(p.Origin), Sha256: p.SHA256, Size: p.Size, Permissions: p.Permissions(),
		Requires: map[string]string{}, Provides: []string{}, Games: []api.PluginGame{}, CreatedAt: p.CreatedAt}
	for k, v := range m.Requires {
		info.Requires[k] = v
	}
	info.Provides = append(info.Provides, m.Provides...)
	sort.Strings(info.Provides)
	for _, g := range m.Games {
		codes := g.Codes
		if codes == nil {
			codes = []string{}
		}
		info.Games = append(info.Games, api.PluginGame{System: g.System, Codes: codes})
	}
	if len(m.RunSettings) > 0 {
		rs := make([]api.PluginRunSetting, 0, len(m.RunSettings))
		for _, r := range m.RunSettings {
			e := api.PluginRunSetting{Key: r.Key, Type: api.PluginRunSettingType(r.Type), Label: r.Label}
			if r.Hint != "" {
				e.Hint = ptr(r.Hint)
			}
			switch d := r.Default.(type) {
			case bool:
				_ = e.Default.FromPluginRunSettingDefault0(d)
			case string:
				_ = e.Default.FromPluginRunSettingDefault1(d)
			}
			if len(r.Options) > 0 {
				opts := make([]api.PluginRunSettingOption, 0, len(r.Options))
				for _, o := range r.Options {
					opts = append(opts, api.PluginRunSettingOption{Value: o.Value, Label: o.Label})
				}
				e.Options = &opts
			}
			rs = append(rs, e)
		}
		strs := p.RunSettingsStrings
		if strs == nil {
			strs = map[string]map[string]string{}
		}
		info.RunSettings, info.RunSettingsStrings = &rs, &strs
	}
	return info
}

// GetPluginFile streams a stored .uzplugin file (ETag = "<sha256>", If-None-Match answers 304); 404 when unknown.
func (s *Server) GetPluginFile(ctx context.Context, req api.GetPluginFileRequestObject) (api.GetPluginFileResponseObject, error) {
	f, p, err := s.svc.OpenPluginFile(ctx, string(req.PluginId), string(req.Version))
	if err != nil {
		return nil, err
	}
	etag := `"` + p.SHA256 + `"`
	if req.Params.IfNoneMatch != nil && etagMatches(*req.Params.IfNoneMatch, etag) {
		f.Close()
		return api.GetPluginFile304Response{Headers: api.GetPluginFile304ResponseHeaders{ETag: ptr(etag)}}, nil
	}
	return api.GetPluginFile200ApplicationzipResponse{Body: f, ContentLength: p.Size,
		Headers: api.GetPluginFile200ResponseHeaders{ContentLength: ptr(p.Size), ETag: ptr(etag)}}, nil
}
