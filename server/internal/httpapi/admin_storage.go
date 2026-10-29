package httpapi

import (
	"context"

	"github.com/phabioo/uzume/server/internal/api"
	"github.com/phabioo/uzume/server/internal/hub"
	"github.com/phabioo/uzume/server/internal/sysmetrics"
)

// Admin storage overview and clean-up (0.19 package 7): /api/v1/admin/storage. Admin only. Clean-up never offers saves
// (see hub.StorageCleanup).

func (s *Server) GetAdminStorage(ctx context.Context, _ api.GetAdminStorageRequestObject) (api.GetAdminStorageResponseObject, error) {
	if err := requireAdmin(ctx); err != nil {
		return nil, err
	}
	ov, err := s.svc.StorageOverview(ctx)
	if err != nil {
		return nil, err
	}
	out := api.AdminStorage{MeasuredAt: ov.MeasuredAt, Kinds: make([]api.AdminStorageKind, 0, len(ov.Kinds)),
		Screenshots: make([]api.AdminStorageScreenshotUsage, 0, len(ov.Screenshots)), Cleanup: make([]api.AdminStorageCleanup, 0, len(ov.Cleanup))}
	var total, free uint64
	var ok bool
	if s.status != nil {
		total, free, ok = s.status.DataDisk()
	} else {
		total, free, ok = sysmetrics.Disk(s.svc.DataDir())
	}
	if ok {
		out.Disk = &struct {
			FreeBytes  int64 `json:"free_bytes"`
			TotalBytes int64 `json:"total_bytes"`
		}{FreeBytes: int64(free), TotalBytes: int64(total)}
	}
	if bs := s.svc.BackupStatus(ctx); bs.FolderSet {
		bd := &api.AdminStatusBackupDisk{Folder: bs.Settings.Folder}
		if bs.DiskOK {
			t, f := int64(bs.DiskTotal), int64(bs.DiskFree)
			bd.TotalBytes, bd.FreeBytes = &t, &f
		}
		out.BackupDisk = bd
	}
	for _, k := range ov.Kinds {
		out.Kinds = append(out.Kinds, api.AdminStorageKind{Id: api.AdminStorageKindId(k.ID), Bytes: k.Bytes, Count: k.Count})
	}
	for _, u := range ov.Screenshots {
		out.Screenshots = append(out.Screenshots, api.AdminStorageScreenshotUsage{UserId: u.UserID, UserName: u.UserName,
			GameId: u.GameID, GameName: u.GameName, Bytes: u.Bytes, Count: u.Count})
	}
	for _, c := range ov.Cleanup {
		out.Cleanup = append(out.Cleanup, api.AdminStorageCleanup{Id: api.AdminStorageCleanupId(c.ID), Bytes: c.Bytes, Count: c.Count})
	}
	return api.GetAdminStorage200JSONResponse(out), nil
}

// GetAdminStorageScreenshots lists the screenshot usage per user and game (all rows, largest first) with the totals.
func (s *Server) GetAdminStorageScreenshots(ctx context.Context, req api.GetAdminStorageScreenshotsRequestObject) (api.GetAdminStorageScreenshotsResponseObject, error) {
	if err := requireAdmin(ctx); err != nil {
		return nil, err
	}
	var userID, gameID string
	if req.Params.UserId != nil {
		userID = *req.Params.UserId
	}
	if req.Params.GameId != nil {
		gameID = *req.Params.GameId
	}
	res, err := s.svc.StorageScreenshots(ctx, userID, gameID)
	if err != nil {
		return nil, err
	}
	out := api.AdminStorageScreenshots{TotalCount: res.TotalCount, TotalBytes: res.TotalBytes,
		Items: make([]api.AdminStorageScreenshotUsage, 0, len(res.Items))}
	for _, u := range res.Items {
		out.Items = append(out.Items, api.AdminStorageScreenshotUsage{UserId: u.UserID, UserName: u.UserName,
			GameId: u.GameID, GameName: u.GameName, Bytes: u.Bytes, Count: u.Count})
	}
	return api.GetAdminStorageScreenshots200JSONResponse(out), nil
}

func (s *Server) CleanupAdminStorage(ctx context.Context, req api.CleanupAdminStorageRequestObject) (api.CleanupAdminStorageResponseObject, error) {
	if err := requireAdmin(ctx); err != nil {
		return nil, err
	}
	if err := s.requireConfirmed(ctx); err != nil {
		return nil, err
	}
	if req.Body == nil {
		return nil, hub.ErrBadRequest
	}
	res, err := s.svc.StorageCleanup(ctx, string(req.Body.Id))
	if err != nil {
		return nil, err
	}
	s.log.Info("storage clean-up", "id", res.ID, "removed", res.Removed, "freed_bytes", res.FreedBytes, "admin", principal(ctx).User.Username)
	return api.CleanupAdminStorage200JSONResponse(api.AdminStorageCleanupResult{Id: api.AdminStorageCleanupId(res.ID),
		FreedBytes: res.FreedBytes, Removed: res.Removed}), nil
}
