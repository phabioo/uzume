package httpapi

import (
	"context"
	"encoding/json"
	"errors"

	"github.com/phabioo/uzume/server/internal/api"
	"github.com/phabioo/uzume/server/internal/hub"
)

// Plugin user store (0.20 package 3, feature plugin_store_v1). Every call works on the caller's own rows; there is no
// admin path to another person's values.

func toAPIStoreEntry(e hub.StoreEntry) api.PluginStoreEntry {
	return api.PluginStoreEntry{Key: e.Key, Revision: e.Revision, SizeBytes: e.SizeBytes, UpdatedAt: e.UpdatedAt}
}

// storeConflictBody is the 409 body: the server's current revision and value (revision 0 and no value when the key is gone).
func storeConflictBody(ce *hub.StoreConflictError) api.PluginStoreConflictError {
	var e api.PluginStoreConflictError
	e.Error.Code, e.Error.Message = api.ErrorCodeStoreConflict, ce.Error()
	rev := int64(0)
	if ce.Current.Exists {
		rev = ce.Current.Entry.Revision
		e.Value = api.PluginStoreJson(ce.Current.Entry.Value)
	}
	e.Revision = &rev
	return e
}

func (s *Server) ListPluginStore(ctx context.Context, req api.ListPluginStoreRequestObject) (api.ListPluginStoreResponseObject, error) {
	l, err := s.svc.ListStore(ctx, principal(ctx).User.ID, req.PluginId)
	if err != nil {
		return nil, err
	}
	out := api.ListPluginStore200JSONResponse{Keys: make([]api.PluginStoreEntry, 0, len(l.Entries)), UsedBytes: l.UsedBytes, QuotaBytes: l.QuotaBytes}
	for _, e := range l.Entries {
		out.Keys = append(out.Keys, toAPIStoreEntry(e))
	}
	return out, nil
}

func (s *Server) GetPluginStoreValue(ctx context.Context, req api.GetPluginStoreValueRequestObject) (api.GetPluginStoreValueResponseObject, error) {
	e, err := s.svc.GetStore(ctx, principal(ctx).User.ID, req.PluginId, req.Key)
	if err != nil {
		return nil, err
	}
	return api.GetPluginStoreValue200JSONResponse{Key: e.Key, Revision: e.Revision, Value: api.PluginStoreJson(e.Value), UpdatedAt: e.UpdatedAt}, nil
}

func (s *Server) PutPluginStoreValue(ctx context.Context, req api.PutPluginStoreValueRequestObject) (api.PutPluginStoreValueResponseObject, error) {
	if req.Body == nil {
		return nil, hub.ErrBadRequest
	}
	e, err := s.svc.PutStore(ctx, principal(ctx).User.ID, req.PluginId, req.Key, json.RawMessage(req.Body.Value), req.Body.BaseRevision)
	var ce *hub.StoreConflictError
	if errors.As(err, &ce) {
		return api.PutPluginStoreValue409JSONResponse(storeConflictBody(ce)), nil
	}
	if err != nil {
		return nil, err
	}
	return api.PutPluginStoreValue200JSONResponse(toAPIStoreEntry(e)), nil
}

func (s *Server) DeletePluginStoreValue(ctx context.Context, req api.DeletePluginStoreValueRequestObject) (api.DeletePluginStoreValueResponseObject, error) {
	base := int64(-1)
	if req.Params.BaseRevision != nil {
		base = *req.Params.BaseRevision
	}
	err := s.svc.DeleteStore(ctx, principal(ctx).User.ID, req.PluginId, req.Key, base)
	var ce *hub.StoreConflictError
	if errors.As(err, &ce) {
		return api.DeletePluginStoreValue409JSONResponse(storeConflictBody(ce)), nil
	}
	if err != nil {
		return nil, err
	}
	return api.DeletePluginStoreValue204Response{}, nil
}
