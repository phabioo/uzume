package hub

import (
	"context"
	"sort"
)

// FeatureAdminV1 is the handshake feature flag of the admin API under /api/v1/admin/...; it is advertised to admins only.
const FeatureAdminV1 = "admin_v1"

// BaseFeatures are the server feature flags every server advertises; the API adds turn_v1 and uploads_v1 per request.
func BaseFeatures() []string {
	return []string{FeatureSavesV1, FeatureSessionsV1, FeatureUsersV1, FeatureFirmwareV1, FeatureCoresV1,
		FeatureSavesV2, FeatureCoresV2, FeatureSavesV3, FeatureSavesV4, FeatureSavesV5, FeatureSavesV6, FeatureLocalSetupV1, FeatureICEUpdateV1, FeaturePluginsV1, FeatureRunsV1, FeatureRunsV2, FeatureLocalROMsV1, FeatureReportsV1, FeatureAccountsV1, FeatureSetupCodeV1, FeatureStatesV1, FeatureRunRulesV1, FeatureToolsV1, FeatureVariantsV1, FeaturePluginStoreV1, FeatureCheatDBV1, FeatureReportsV2}
}

// ConnectedDeviceIDs returns the devices with an open WSS connection, sorted.
func (s *Service) ConnectedDeviceIDs() []string {
	var ids []string
	for _, c := range s.clients() {
		ids = append(ids, c.deviceID)
	}
	sort.Strings(ids)
	return ids
}

// DiagCounts are the row counts shown in the diagnostics bundle.
type DiagCounts struct {
	Users, Devices, Games, ActiveSessions int
}

// DiagCounts counts users, devices, games and running Sessions.
func (s *Service) DiagCounts(ctx context.Context) (DiagCounts, error) {
	var c DiagCounts
	for _, q := range []struct {
		sql string
		dst *int
	}{
		{`SELECT COUNT(*) FROM users`, &c.Users},
		{`SELECT COUNT(*) FROM devices`, &c.Devices},
		{`SELECT COUNT(*) FROM games`, &c.Games},
		{`SELECT COUNT(*) FROM sessions WHERE ended_at IS NULL`, &c.ActiveSessions},
	} {
		if err := s.db.QueryRowContext(ctx, q.sql).Scan(q.dst); err != nil {
			return c, internal(err)
		}
	}
	return c, nil
}

// DataDirFreeBytes is the free space on the data directory's volume (0: unknown).
func (s *Service) DataDirFreeBytes() uint64 { return freeBytes(s.dataDir) }
