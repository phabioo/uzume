package hub

import (
	"bytes"
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"regexp"
	"time"
)

// Plugin user store (0.20 package 3, feature plugin_store_v1): per user and plugin id a small key/value store of JSON
// values that follows the person to every PC (fb.store in the plugin API). The server never looks into a value.
// Writes carry the revision they are based on; a stale base is a conflict that returns the server's value so the app
// can keep both versions. Nobody but the owner reads a value (no admin access).

// FeaturePluginStoreV1 is the handshake feature flag for /plugins/{plugin_id}/store.
const FeaturePluginStoreV1 = "plugin_store_v1"

const (
	// MaxStoreValueBytes is the largest value (compact JSON).
	MaxStoreValueBytes = 256 << 10
	// MaxStorePluginBytes is the most one user stores for one plugin (all values together).
	MaxStorePluginBytes = 16 << 20
	// MaxStorePluginKeys is the most keys one user has for one plugin.
	MaxStorePluginKeys = 1024
)

// Error codes of the plugin store.
const (
	// CodeStoreConflict: base_revision is not the current revision of the key (409, carries the server value).
	CodeStoreConflict Code = "store_conflict"
	// CodeStoreQuotaExceeded: the plugin's store of this user is full (413).
	CodeStoreQuotaExceeded Code = "store_quota_exceeded"
)

var (
	storeKeyRe = regexp.MustCompile(`^[a-z0-9_.-]{1,64}$`)

	// ErrStoreQuotaExceeded is returned when a value does not fit into the 16 MiB (or 1024 keys) of a plugin.
	ErrStoreQuotaExceeded = &Error{Code: CodeStoreQuotaExceeded, Message: "The plugin store of this user is full"}
)

// StoreEntry is one key of a plugin's store.
type StoreEntry struct {
	Key       string
	Value     json.RawMessage // nil in listings
	Revision  int64
	SizeBytes int64
	UpdatedAt time.Time
}

// StoreList lists the keys of a plugin's store with the numbers of the cap.
type StoreList struct {
	Entries    []StoreEntry
	UsedBytes  int64
	QuotaBytes int64
}

// StoreConflict is the current server state returned with a conflict; Exists false = the key is not there.
type StoreConflict struct {
	Exists bool
	Entry  StoreEntry
}

// StoreConflictError is returned by PutStore and DeleteStore for a stale base_revision.
type StoreConflictError struct{ Current StoreConflict }

func (e *StoreConflictError) Error() string {
	return "store_conflict: base_revision is not the current revision; merge with the returned value and retry"
}

func validStoreRef(pluginID, key string) error {
	if !runPluginIDRe.MatchString(pluginID) || len(pluginID) > 128 {
		return badRequest("Invalid plugin id")
	}
	if key != "" && !storeKeyRe.MatchString(key) {
		return badRequest("The key must match ^[a-z0-9_.-]{1,64}$")
	}
	return nil
}

// ListStore returns the keys, revisions and sizes of the caller's store of a plugin (for the sync of the app).
func (s *Service) ListStore(ctx context.Context, userID, pluginID string) (StoreList, error) {
	if err := validStoreRef(pluginID, ""); err != nil {
		return StoreList{}, err
	}
	rows, err := s.db.QueryContext(ctx, `SELECT key, revision, size_bytes, updated_at FROM plugin_store
		WHERE user_id = ? AND plugin_id = ? ORDER BY key`, userID, pluginID)
	if err != nil {
		return StoreList{}, internal(err)
	}
	defer rows.Close()
	out := StoreList{Entries: []StoreEntry{}, QuotaBytes: MaxStorePluginBytes}
	for rows.Next() {
		var e StoreEntry
		var upd int64
		if err := rows.Scan(&e.Key, &e.Revision, &e.SizeBytes, &upd); err != nil {
			return StoreList{}, internal(err)
		}
		e.UpdatedAt = time.Unix(upd, 0).UTC()
		out.Entries = append(out.Entries, e)
		out.UsedBytes += e.SizeBytes
	}
	return out, rows.Err()
}

func (s *Service) storeEntry(ctx context.Context, q dbq, userID, pluginID, key string) (StoreEntry, error) {
	var e StoreEntry
	var val string
	var upd int64
	err := q.QueryRowContext(ctx, `SELECT key, value, revision, size_bytes, updated_at FROM plugin_store
		WHERE user_id = ? AND plugin_id = ? AND key = ?`, userID, pluginID, key).Scan(&e.Key, &val, &e.Revision, &e.SizeBytes, &upd)
	if errors.Is(err, sql.ErrNoRows) {
		return StoreEntry{}, ErrNotFound
	}
	if err != nil {
		return StoreEntry{}, internal(err)
	}
	e.Value, e.UpdatedAt = json.RawMessage(val), time.Unix(upd, 0).UTC()
	return e, nil
}

// GetStore returns one value of the caller; ErrNotFound when the key does not exist.
func (s *Service) GetStore(ctx context.Context, userID, pluginID, key string) (StoreEntry, error) {
	if err := validStoreRef(pluginID, key); err != nil {
		return StoreEntry{}, err
	}
	if key == "" {
		return StoreEntry{}, ErrBadRequest
	}
	return s.storeEntry(ctx, s.db, userID, pluginID, key)
}

// PutStore writes a value if baseRevision is the key's current revision (0 = the key must not exist yet). A different
// base returns a *StoreConflictError with the current value. The value must be valid JSON of at most 256 KiB.
func (s *Service) PutStore(ctx context.Context, userID, pluginID, key string, value json.RawMessage, baseRevision int64) (StoreEntry, error) {
	if err := validStoreRef(pluginID, key); err != nil {
		return StoreEntry{}, err
	}
	if key == "" || baseRevision < 0 {
		return StoreEntry{}, ErrBadRequest
	}
	var buf bytes.Buffer
	if len(bytes.TrimSpace(value)) == 0 || json.Compact(&buf, value) != nil {
		return StoreEntry{}, badRequest("The value must be JSON")
	}
	if buf.Len() > MaxStoreValueBytes {
		return StoreEntry{}, &Error{Code: CodePayloadTooLarge, Message: "A value is at most 256 KiB"}
	}
	s.storeMu.Lock()
	defer s.storeMu.Unlock()
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return StoreEntry{}, internal(err)
	}
	defer tx.Rollback()
	cur, err := s.storeEntry(ctx, tx, userID, pluginID, key)
	exists := err == nil
	if err != nil && !errors.Is(err, ErrNotFound) {
		return StoreEntry{}, err
	}
	curRev := int64(0)
	if exists {
		curRev = cur.Revision
	}
	if curRev != baseRevision {
		return StoreEntry{}, &StoreConflictError{Current: StoreConflict{Exists: exists, Entry: cur}}
	}
	var used, keys int64
	if err := tx.QueryRowContext(ctx, `SELECT COALESCE(SUM(size_bytes), 0), COUNT(*) FROM plugin_store WHERE user_id = ? AND plugin_id = ?`,
		userID, pluginID).Scan(&used, &keys); err != nil {
		return StoreEntry{}, internal(err)
	}
	size := int64(buf.Len())
	if exists {
		used -= cur.SizeBytes
		keys--
	}
	if used+size > MaxStorePluginBytes || keys+1 > MaxStorePluginKeys {
		return StoreEntry{}, ErrStoreQuotaExceeded
	}
	now := s.now().Unix()
	if _, err := tx.ExecContext(ctx, `INSERT INTO plugin_store(user_id, plugin_id, key, value, size_bytes, revision, updated_at)
		VALUES (?,?,?,?,?,?,?) ON CONFLICT(user_id, plugin_id, key) DO UPDATE SET value = excluded.value,
		size_bytes = excluded.size_bytes, revision = excluded.revision, updated_at = excluded.updated_at`,
		userID, pluginID, key, buf.String(), size, curRev+1, now); err != nil {
		return StoreEntry{}, internal(err)
	}
	if err := tx.Commit(); err != nil {
		return StoreEntry{}, internal(err)
	}
	return StoreEntry{Key: key, Revision: curRev + 1, SizeBytes: size, UpdatedAt: time.Unix(now, 0).UTC()}, nil
}

// DeleteStore removes a key. With baseRevision >= 0 the delete is conditional like a write (a stale base is a
// conflict); -1 deletes whatever is there. ErrNotFound when the key does not exist.
func (s *Service) DeleteStore(ctx context.Context, userID, pluginID, key string, baseRevision int64) error {
	if err := validStoreRef(pluginID, key); err != nil {
		return err
	}
	if key == "" {
		return ErrBadRequest
	}
	s.storeMu.Lock()
	defer s.storeMu.Unlock()
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return internal(err)
	}
	defer tx.Rollback()
	cur, err := s.storeEntry(ctx, tx, userID, pluginID, key)
	if err != nil {
		return err
	}
	if baseRevision >= 0 && cur.Revision != baseRevision {
		return &StoreConflictError{Current: StoreConflict{Exists: true, Entry: cur}}
	}
	if _, err := tx.ExecContext(ctx, `DELETE FROM plugin_store WHERE user_id = ? AND plugin_id = ? AND key = ?`, userID, pluginID, key); err != nil {
		return internal(err)
	}
	return tx.Commit()
}
