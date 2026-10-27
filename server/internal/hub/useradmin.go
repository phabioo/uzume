package hub

import (
	"context"
	"database/sql"
	"errors"
	"log/slog"
	"os"
	"path/filepath"
	"time"
)

// UserRow is a user with the number of devices that still have access (Users page).
type UserRow struct {
	User
	Devices int
	// PlayStat is the user's play time over all games (0.19).
	PlayStat
}

// ListUserRows returns all users (oldest first) with their number of trusted devices.
func (s *Service) ListUserRows(ctx context.Context) ([]UserRow, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT u.id, u.username, u.display_name, u.role, u.created_at, u.disabled_at,
		(SELECT COUNT(*) FROM devices d WHERE d.user_id = u.id AND d.status = 'trusted')
		FROM users u ORDER BY u.created_at, u.username`)
	if err != nil {
		return nil, internal(err)
	}
	defer rows.Close()
	var out []UserRow
	for rows.Next() {
		var r UserRow
		var created int64
		var dis sql.NullInt64
		if err := rows.Scan(&r.ID, &r.Username, &r.DisplayName, &r.Role, &created, &dis, &r.Devices); err != nil {
			return nil, internal(err)
		}
		r.CreatedAt, r.DisabledAt = time.Unix(created, 0).UTC(), ts(dis)
		out = append(out, r)
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	rows.Close()
	stats, err := s.AllUserPlayStats(ctx)
	if err != nil {
		return nil, err
	}
	for i := range out {
		out[i].PlayStat = stats[out[i].ID]
	}
	return out, nil
}

// DisableUser disables a regular user. Admins cannot be disabled (ErrForbidden). The user's tokens stay in
// place but every authenticated call and the token exchange answer user_disabled; WSS connections are closed
// and the user's Sessions end (reason owner_disconnected). Saves, uploads and pending invites are untouched.
func (s *Service) DisableUser(ctx context.Context, userID string) (err error) {
	defer s.publishOK(&err, TopicUsers, TopicClients)
	u, err := s.GetUser(ctx, userID)
	if err != nil {
		return err
	}
	if u.Role == RoleAdmin {
		return &Error{Code: CodeForbidden, Message: "Admins cannot be disabled"}
	}
	if u.Disabled() {
		return nil
	}
	if _, err := s.db.ExecContext(ctx, `UPDATE users SET disabled_at = ? WHERE id = ? AND disabled_at IS NULL`, s.now().Unix(), userID); err != nil {
		return internal(err)
	}
	s.onUserDisabled(ctx, userID)
	return nil
}

// EnableUser enables a disabled user again (idempotent).
func (s *Service) EnableUser(ctx context.Context, userID string) (err error) {
	defer s.publishOK(&err, TopicUsers)
	res, err := s.db.ExecContext(ctx, `UPDATE users SET disabled_at = NULL WHERE id = ?`, userID)
	if err != nil {
		return internal(err)
	}
	if n, _ := res.RowsAffected(); n == 0 {
		return ErrNotFound
	}
	return nil
}

// userDeviceIDs lists all device IDs of a user.
func (s *Service) userDeviceIDs(ctx context.Context, userID string) ([]string, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT id FROM devices WHERE user_id = ?`, userID)
	if err != nil {
		return nil, internal(err)
	}
	defer rows.Close()
	var ids []string
	for rows.Next() {
		var id string
		if err := rows.Scan(&id); err != nil {
			return nil, internal(err)
		}
		ids = append(ids, id)
	}
	return ids, rows.Err()
}

// onUserDisabled ends the user's Sessions, removes the user's devices as viewers and closes their WSS connections.
func (s *Service) onUserDisabled(ctx context.Context, userID string) {
	devs, err := s.userDeviceIDs(ctx, userID)
	if err != nil && !errors.Is(err, context.Canceled) {
		devs = nil
	}
	s.sess.mu.Lock()
	defer s.sess.mu.Unlock()
	if ids, err := s.activeSessionIDs(ctx, `AND owner_user_id = ?`, userID); err == nil {
		for _, id := range ids {
			if d, err := s.loadSession(ctx, id); err == nil {
				s.endLocked(ctx, d, EndOwnerDisconnected)
			}
		}
	}
	for _, dev := range devs {
		s.dropViewersOfDevice(ctx, dev, LeftDisconnected)
		if c := s.clientOf(dev); c != nil {
			c.closeWith(CloseCodePolicy, "user_disabled")
		}
	}
}

// DeleteUser permanently deletes a regular user in one transaction: the user, their devices and tokens, invites they
// created, pairing requests, saves, save history and conflicts. Games the user uploaded stay in the library and are
// reassigned to actorID (the deleting admin), because games.uploaded_by is a foreign key without cascade. Admins
// (which covers the own account and the last admin) cannot be deleted (ErrForbidden); actorID must be an existing
// admin. Live Sessions and WSS connections end as on disable. Save blobs are removed from disk after the commit.
func (s *Service) DeleteUser(ctx context.Context, userID, actorID string) (err error) {
	defer s.publishOK(&err, TopicUsers, TopicClients, TopicSaves)
	u, err := s.GetUser(ctx, userID)
	if err != nil {
		return err
	}
	if u.Role == RoleAdmin || userID == actorID {
		return &Error{Code: CodeForbidden, Message: "Admins cannot be deleted"}
	}
	if a, err := s.GetUser(ctx, actorID); err != nil || a.Role != RoleAdmin {
		return ErrForbidden
	}
	// Block further authentication and end Sessions / close connections before the rows disappear.
	if err := s.DisableUser(ctx, userID); err != nil {
		return err
	}
	// If the delete transaction fails the user must not stay disabled by a half-done deletion: re-enable it
	// (unless it was disabled before), so the outcome is "deleted" or "unchanged".
	defer func() {
		if err != nil && !u.Disabled() {
			if _, rerr := s.db.ExecContext(context.Background(), `UPDATE users SET disabled_at = NULL WHERE id = ?`, userID); rerr != nil {
				slog.Error("re-enable user after failed deletion", "user_id", userID, "err", rerr)
			}
		}
	}()
	s.saveMu.Lock()
	defer s.saveMu.Unlock()
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return internal(err)
	}
	defer tx.Rollback()
	stmts := []struct {
		q    string
		args []any
	}{
		{`UPDATE games SET uploaded_by = ? WHERE uploaded_by = ?`, []any{actorID, userID}},
		{`DELETE FROM pairing_requests WHERE user_id = ? OR device_id IN (SELECT id FROM devices WHERE user_id = ?)`, []any{userID, userID}},
		{`DELETE FROM invites WHERE created_by = ?`, []any{userID}},
		// Cascades: devices, tokens, Sessions, web sessions, saves, history, conflicts.
		{`DELETE FROM users WHERE id = ?`, []any{userID}},
	}
	for _, st := range stmts {
		if _, err := tx.ExecContext(ctx, st.q, st.args...); err != nil {
			return internal(err)
		}
	}
	if err := tx.Commit(); err != nil {
		return internal(err)
	}
	_ = os.RemoveAll(filepath.Join(s.dataDir, "saves", userID)) // best effort: orphaned blobs are harmless
	s.removeUserScreenshotFiles(userID)
	s.removeUserStateFiles(userID)
	return nil
}
