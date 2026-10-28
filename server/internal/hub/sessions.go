package hub

import (
	"context"
	"database/sql"
	"errors"
	"log/slog"
	"slices"
	"time"

	"github.com/google/uuid"

	"github.com/phabioo/uzume/server/internal/config"
	"github.com/phabioo/uzume/server/internal/turnsrv"
)

// Sessions (ADR 0006): metadata, visibility, invites and viewers. Media never touches the server.
// All Session mutations run under sessionState.mu; events are queued non-blocking to the WSS clients.

// FeatureSessionsV1 is the handshake feature flag for the Sessions API and the WSS endpoint.
const FeatureSessionsV1 = "sessions_v1"

// MaxSessionViewers is the viewer limit per Session (session_full).
const MaxSessionViewers = 4

// DefaultOwnerGrace is how long a Session survives the owner's dropped WSS connection.
const DefaultOwnerGrace = 30 * time.Second

// Visibility of a Session.
type Visibility string

const (
	VisibilityPrivate    Visibility = "private"
	VisibilityHubUsers   Visibility = "hub_users"
	VisibilityInviteOnly Visibility = "invite_only"
)

// Valid reports whether v is a known visibility.
func (v Visibility) Valid() bool {
	return v == VisibilityPrivate || v == VisibilityHubUsers || v == VisibilityInviteOnly
}

// Reasons of session_ended.
const (
	EndEnded             = "ended" // owner DELETE
	EndReplaced          = "replaced"
	EndOwnerDisconnected = "owner_disconnected"
	EndDeviceRevoked     = "device_revoked"
	EndNoLongerVisible   = "no_longer_visible" // sent only to devices that lost sight of a Session that continues
)

// Reasons of viewer_left.
const (
	LeftLeft         = "left"
	LeftRemoved      = "removed"
	LeftRevoked      = "revoked"
	LeftDisconnected = "disconnected"
)

// Session is the caller-specific view of a Session (REST and WSS carry the same JSON).
type Session struct {
	SessionID   string           `json:"session_id"`
	GameID      string           `json:"game_id"`
	GameTitle   string           `json:"game_title"`
	Owner       SessionOwner     `json:"owner"`
	Visibility  Visibility       `json:"visibility"`
	CreatedAt   time.Time        `json:"created_at"`
	ViewerCount int              `json:"viewer_count"`
	Viewers     *[]SessionViewer `json:"viewers,omitempty"` // owner device only
	Invites     *[]SessionInvite `json:"invites,omitempty"` // owner device only
	IsOwner     bool             `json:"is_owner"`
	Invited     bool             `json:"invited"`
}

// SessionOwner identifies the owner user and device.
type SessionOwner struct {
	UserID      string `json:"user_id"`
	DisplayName string `json:"display_name"`
	DeviceName  string `json:"device_name"`
}

// SessionViewer is a viewer as the owner sees it.
type SessionViewer struct {
	ViewerID    string `json:"viewer_id"`
	DisplayName string `json:"display_name"`
	DeviceName  string `json:"device_name"`
}

// SessionInvite is an invite as the owner sees it (State: invited|declined|joined).
type SessionInvite struct {
	UserID      string `json:"user_id"`
	DisplayName string `json:"display_name"`
	State       string `json:"state"`
	Online      bool   `json:"online"`
}

// ViewerPermissions are fixed in the PoC.
type ViewerPermissions struct {
	ViewVideo bool `json:"view_video"`
	HearAudio bool `json:"hear_audio"`
	SendInput bool `json:"send_input"`
}

// JoinResult is the result of JoinSession.
type JoinResult struct {
	ViewerID    string
	Permissions ViewerPermissions
	ICEServers  []string
	// TURN holds relay credentials while the embedded TURN server is on.
	TURN []turnsrv.Credentials
}

// UserPresence is a user with online state (GET /users).
type UserPresence struct {
	ID          string
	DisplayName string
	Online      bool
}

// ---- internal data ----

type sessionData struct {
	id, gameID, gameTitle      string
	ownerDeviceID, ownerUserID string
	ownerName, ownerDeviceName string
	visibility                 Visibility
	createdAt                  time.Time
	ended                      bool
	viewers                    []viewerData
	invites                    []inviteData
}

type viewerData struct{ id, userID, userName, deviceID, deviceName string }

type inviteData struct{ userID, userName, state string }

func (d *sessionData) invite(userID string) *inviteData {
	for i := range d.invites {
		if d.invites[i].userID == userID {
			return &d.invites[i]
		}
	}
	return nil
}

func (d *sessionData) viewerByID(id string) *viewerData {
	for i := range d.viewers {
		if d.viewers[i].id == id {
			return &d.viewers[i]
		}
	}
	return nil
}

func (d *sessionData) viewerByDevice(deviceID string) *viewerData {
	for i := range d.viewers {
		if d.viewers[i].deviceID == deviceID {
			return &d.viewers[i]
		}
	}
	return nil
}

// allows is the ACL: may this device see and join the Session? private = devices of the owner user,
// hub_users = every authenticated device, invite_only = owner user plus invited users who did not decline.
func (d *sessionData) allows(userID, deviceID string) bool {
	if deviceID == d.ownerDeviceID || userID == d.ownerUserID {
		return true
	}
	switch d.visibility {
	case VisibilityHubUsers:
		return true
	case VisibilityInviteOnly:
		inv := d.invite(userID)
		return inv != nil && inv.state == "invited"
	}
	return false
}

func (d *sessionData) render(userID, deviceID string, online func(string) bool) Session {
	owner := deviceID == d.ownerDeviceID
	out := Session{SessionID: d.id, GameID: d.gameID, GameTitle: d.gameTitle,
		Owner:      SessionOwner{UserID: d.ownerUserID, DisplayName: d.ownerName, DeviceName: d.ownerDeviceName},
		Visibility: d.visibility, CreatedAt: d.createdAt, ViewerCount: len(d.viewers), IsOwner: owner}
	if inv := d.invite(userID); inv != nil && inv.state == "invited" && userID != d.ownerUserID {
		out.Invited = true
	}
	if owner {
		vs := make([]SessionViewer, 0, len(d.viewers))
		joined := map[string]bool{}
		for _, v := range d.viewers {
			vs = append(vs, SessionViewer{ViewerID: v.id, DisplayName: v.userName, DeviceName: v.deviceName})
			joined[v.userID] = true
		}
		is := make([]SessionInvite, 0, len(d.invites))
		for _, i := range d.invites {
			st := i.state
			if joined[i.userID] {
				st = "joined"
			}
			is = append(is, SessionInvite{UserID: i.userID, DisplayName: i.userName, State: st, Online: online(i.userID)})
		}
		out.Viewers, out.Invites = &vs, &is
	}
	return out
}

// loadSession reads a Session incl. viewers and invites (ended Sessions too). Caller holds sess.mu.
func (s *Service) loadSession(ctx context.Context, id string) (*sessionData, error) {
	d := &sessionData{}
	var created int64
	var ended sql.NullInt64
	err := s.db.QueryRowContext(ctx, `SELECT s.id, s.game_id, s.game_title, s.owner_device_id, s.owner_user_id, u.display_name, dv.name,
		s.visibility, s.created_at, s.ended_at
		FROM sessions s JOIN users u ON u.id = s.owner_user_id JOIN devices dv ON dv.id = s.owner_device_id WHERE s.id = ?`, id).
		Scan(&d.id, &d.gameID, &d.gameTitle, &d.ownerDeviceID, &d.ownerUserID, &d.ownerName, &d.ownerDeviceName, &d.visibility, &created, &ended)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, ErrSessionNotFound
	}
	if err != nil {
		return nil, internal(err)
	}
	d.createdAt, d.ended = time.Unix(created, 0).UTC(), ended.Valid
	rows, err := s.db.QueryContext(ctx, `SELECT v.viewer_id, v.user_id, u.display_name, v.device_id, dv.name
		FROM session_viewers v JOIN users u ON u.id = v.user_id JOIN devices dv ON dv.id = v.device_id
		WHERE v.session_id = ? AND v.left_at IS NULL ORDER BY v.joined_at, v.viewer_id`, id)
	if err != nil {
		return nil, internal(err)
	}
	for rows.Next() {
		var v viewerData
		if err := rows.Scan(&v.id, &v.userID, &v.userName, &v.deviceID, &v.deviceName); err != nil {
			rows.Close()
			return nil, internal(err)
		}
		d.viewers = append(d.viewers, v)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return nil, internal(err)
	}
	rows, err = s.db.QueryContext(ctx, `SELECT i.user_id, u.display_name, i.state FROM session_invites i
		JOIN users u ON u.id = i.user_id WHERE i.session_id = ? ORDER BY i.created_at, i.user_id`, id)
	if err != nil {
		return nil, internal(err)
	}
	for rows.Next() {
		var i inviteData
		if err := rows.Scan(&i.userID, &i.userName, &i.state); err != nil {
			rows.Close()
			return nil, internal(err)
		}
		d.invites = append(d.invites, i)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return nil, internal(err)
	}
	return d, nil
}

// activeSessionIDs lists active Sessions, newest first. Caller holds sess.mu.
// An optional extra condition ("AND ...") and its arguments may follow.
func (s *Service) activeSessionIDs(ctx context.Context, whereArgs ...any) ([]string, error) {
	where, args := "", []any(nil)
	if len(whereArgs) > 0 {
		where, args = whereArgs[0].(string), whereArgs[1:]
	}
	rows, err := s.db.QueryContext(ctx, `SELECT id FROM sessions WHERE ended_at IS NULL `+where+` ORDER BY created_at DESC, id`, args...)
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

// loadActive loads an active Session: ErrSessionNotFound / ErrSessionEnded.
func (s *Service) loadActive(ctx context.Context, id string) (*sessionData, error) {
	d, err := s.loadSession(ctx, id)
	if err != nil {
		return nil, err
	}
	if d.ended {
		return nil, ErrSessionEnded
	}
	return d, nil
}

// loadOwned is loadActive plus the owner device check.
func (s *Service) loadOwned(ctx context.Context, p Principal, id string) (*sessionData, error) {
	d, err := s.loadActive(ctx, id)
	if err != nil {
		return nil, err
	}
	if d.ownerDeviceID != p.Device.ID {
		return nil, ErrSessionForbidden
	}
	return d, nil
}

func (s *Service) view(d *sessionData, p Principal) Session {
	return d.render(p.User.ID, p.Device.ID, s.userOnline)
}

// deviceCaps returns the reported codec capabilities (nil = unknown).
func (s *Service) deviceCaps(ctx context.Context, deviceID string) (enc, dec *bool, err error) {
	var e, d sql.NullInt64
	if err := s.db.QueryRowContext(ctx, `SELECT h264_encode, h264_decode FROM devices WHERE id = ?`, deviceID).Scan(&e, &d); err != nil {
		return nil, nil, internal(err)
	}
	conv := func(n sql.NullInt64) *bool {
		if !n.Valid {
			return nil
		}
		b := n.Int64 != 0
		return &b
	}
	return conv(e), conv(d), nil
}

// ---- operations ----

// PublishSession starts a Session of the caller's device and ends the device's previous one.
func (s *Service) PublishSession(ctx context.Context, p Principal, gameID string, vis Visibility) (Session, error) {
	if !vis.Valid() {
		return Session{}, badRequest("visibility must be private, hub_users or invite_only")
	}
	g, err := s.GetGameFor(ctx, p.User.ID, gameID)
	if err != nil {
		return Session{}, err
	}
	enc, _, err := s.deviceCaps(ctx, p.Device.ID)
	if err != nil {
		return Session{}, err
	}
	if enc != nil && !*enc {
		return Session{}, &Error{Code: CodeCapabilityMissing, Message: "Device cannot encode H.264"}
	}
	s.sess.mu.Lock()
	defer s.sess.mu.Unlock()
	prev, err := s.activeSessionIDs(ctx, `AND owner_device_id = ?`, p.Device.ID)
	if err != nil {
		return Session{}, err
	}
	for _, id := range prev {
		if d, err := s.loadSession(ctx, id); err == nil {
			s.endLocked(ctx, d, EndReplaced)
		}
	}
	id := uuid.NewString()
	if _, err := s.db.ExecContext(ctx, `INSERT INTO sessions(id, game_id, game_title, owner_device_id, owner_user_id, visibility, created_at)
		VALUES (?,?,?,?,?,?,?)`, id, g.ID, g.Title, p.Device.ID, p.User.ID, string(vis), s.now().Unix()); err != nil {
		return Session{}, internal(err)
	}
	d, err := s.loadSession(ctx, id)
	if err != nil {
		return Session{}, err
	}
	if !s.deviceConnected(p.Device.ID) {
		s.armOwnerGrace(id, p.Device.ID)
	}
	s.broadcastSession(nil, d, "")
	return s.view(d, p), nil
}

// ListSessions returns the active Sessions the caller may join or owns (incl. open invites), newest first.
func (s *Service) ListSessions(ctx context.Context, p Principal) ([]Session, error) {
	s.sess.mu.Lock()
	defer s.sess.mu.Unlock()
	ids, err := s.activeSessionIDs(ctx)
	if err != nil {
		return nil, err
	}
	out := []Session{}
	for _, id := range ids {
		d, err := s.loadSession(ctx, id)
		if err != nil {
			return nil, err
		}
		if d.allows(p.User.ID, p.Device.ID) {
			out = append(out, s.view(d, p))
		}
	}
	return out, nil
}

// GetSession returns one Session if the caller may see it.
func (s *Service) GetSession(ctx context.Context, p Principal, id string) (Session, error) {
	s.sess.mu.Lock()
	defer s.sess.mu.Unlock()
	d, err := s.loadActive(ctx, id)
	if err != nil {
		return Session{}, err
	}
	if !d.allows(p.User.ID, p.Device.ID) {
		return Session{}, ErrSessionForbidden
	}
	return s.view(d, p), nil
}

// SetSessionVisibility changes the visibility (owner device) and revokes viewers that are no longer allowed.
func (s *Service) SetSessionVisibility(ctx context.Context, p Principal, id string, vis Visibility) (Session, error) {
	if !vis.Valid() {
		return Session{}, badRequest("visibility must be private, hub_users or invite_only")
	}
	s.sess.mu.Lock()
	defer s.sess.mu.Unlock()
	before, err := s.loadOwned(ctx, p, id)
	if err != nil {
		return Session{}, err
	}
	if _, err := s.db.ExecContext(ctx, `UPDATE sessions SET visibility = ? WHERE id = ?`, string(vis), id); err != nil {
		return Session{}, internal(err)
	}
	after, err := s.reconcile(ctx, before, nil, "")
	if err != nil {
		return Session{}, err
	}
	return s.view(after, p), nil
}

// EndSession ends the Session (owner device).
func (s *Service) EndSession(ctx context.Context, p Principal, id string) error {
	s.sess.mu.Lock()
	defer s.sess.mu.Unlock()
	d, err := s.loadOwned(ctx, p, id)
	if err != nil {
		return err
	}
	return s.endLocked(ctx, d, EndEnded)
}

// InviteUser invites a user of this server (owner device). A declined invite becomes open again.
func (s *Service) InviteUser(ctx context.Context, p Principal, id, userID string) (Session, error) {
	s.sess.mu.Lock()
	defer s.sess.mu.Unlock()
	before, err := s.loadOwned(ctx, p, id)
	if err != nil {
		return Session{}, err
	}
	if userID == before.ownerUserID {
		return Session{}, badRequest("The owner cannot be invited")
	}
	if _, err := s.GetUser(ctx, userID); err != nil {
		return Session{}, err
	}
	if _, err := s.db.ExecContext(ctx, `INSERT INTO session_invites(session_id, user_id, state, created_at) VALUES (?,?,'invited',?)
		ON CONFLICT(session_id, user_id) DO UPDATE SET state = 'invited'`, id, userID, s.now().Unix()); err != nil {
		return Session{}, internal(err)
	}
	after, err := s.reconcile(ctx, before, nil, userID)
	if err != nil {
		return Session{}, err
	}
	return s.view(after, p), nil
}

// WithdrawInvite removes an invite (owner device) and removes that user's viewers.
func (s *Service) WithdrawInvite(ctx context.Context, p Principal, id, userID string) error {
	s.sess.mu.Lock()
	defer s.sess.mu.Unlock()
	before, err := s.loadOwned(ctx, p, id)
	if err != nil {
		return err
	}
	res, err := s.db.ExecContext(ctx, `DELETE FROM session_invites WHERE session_id = ? AND user_id = ?`, id, userID)
	if err != nil {
		return internal(err)
	}
	if n, _ := res.RowsAffected(); n == 0 {
		return &Error{Code: CodeNotFound, Message: "Invite not found"}
	}
	_, err = s.reconcile(ctx, before, map[string]bool{userID: true}, "")
	return err
}

// DeclineInvite marks the caller's invite declined; the owner learns it via session_update.
func (s *Service) DeclineInvite(ctx context.Context, p Principal, id string) error {
	s.sess.mu.Lock()
	defer s.sess.mu.Unlock()
	before, err := s.loadActive(ctx, id)
	if err != nil {
		return err
	}
	if before.invite(p.User.ID) == nil {
		return ErrSessionForbidden
	}
	if _, err := s.db.ExecContext(ctx, `UPDATE session_invites SET state = 'declined' WHERE session_id = ? AND user_id = ?`, id, p.User.ID); err != nil {
		return internal(err)
	}
	_, err = s.reconcile(ctx, before, nil, "")
	return err
}

// JoinSession checks the ACL and the codec capability and registers the caller's device as viewer.
func (s *Service) JoinSession(ctx context.Context, p Principal, id string) (JoinResult, error) {
	_, dec, err := s.deviceCaps(ctx, p.Device.ID)
	if err != nil {
		return JoinResult{}, err
	}
	s.sess.mu.Lock()
	defer s.sess.mu.Unlock()
	before, err := s.loadActive(ctx, id)
	if err != nil {
		return JoinResult{}, err
	}
	if !before.allows(p.User.ID, p.Device.ID) {
		return JoinResult{}, ErrSessionForbidden
	}
	if before.ownerDeviceID == p.Device.ID {
		return JoinResult{}, badRequest("A device cannot join its own Session")
	}
	if dec != nil && !*dec {
		return JoinResult{}, &Error{Code: CodeCapabilityMissing, Message: "Device cannot decode H.264"}
	}
	res := JoinResult{Permissions: ViewerPermissions{ViewVideo: true, HearAudio: true}}
	res.ICEServers, res.TURN = s.iceFor(requestHost(ctx), p.Device.ID)
	if v := before.viewerByDevice(p.Device.ID); v != nil {
		res.ViewerID = v.id
		return res, nil
	}
	if len(before.viewers) >= MaxSessionViewers {
		return JoinResult{}, ErrSessionFull
	}
	res.ViewerID = uuid.NewString()
	if _, err := s.db.ExecContext(ctx, `INSERT INTO session_viewers(viewer_id, session_id, user_id, device_id, joined_at) VALUES (?,?,?,?,?)`,
		res.ViewerID, id, p.User.ID, p.Device.ID, s.now().Unix()); err != nil {
		return JoinResult{}, internal(err)
	}
	joined := map[string]any{"session_id": id, "viewer_id": res.ViewerID,
		"display_name": p.User.DisplayName, "device_name": p.Device.Name}
	if oc := s.clientOf(before.ownerDeviceID); oc != nil {
		// Fresh relay credentials for the owner (its hello_ack ones may have expired); host = the owner's WSS request host.
		if _, creds := s.relaysFor(oc.reqHost, before.ownerDeviceID); len(creds) > 0 {
			joined["turn_servers"] = turnPayload(creds)
		}
	}
	s.sendTo(before.ownerDeviceID, "viewer_joined", joined)
	if !s.deviceConnected(p.Device.ID) {
		s.armViewerGrace(res.ViewerID, p.Device.ID)
	}
	if _, err := s.reconcile(ctx, before, nil, ""); err != nil {
		return JoinResult{}, err
	}
	return res, nil
}

// RemoveViewer removes a viewer: the owner device removes ("removed"), the viewer's own device leaves ("left").
func (s *Service) RemoveViewer(ctx context.Context, p Principal, id, viewerID string) error {
	s.sess.mu.Lock()
	defer s.sess.mu.Unlock()
	before, err := s.loadActive(ctx, id)
	if err != nil {
		return err
	}
	owner := before.ownerDeviceID == p.Device.ID
	v := before.viewerByID(viewerID)
	switch {
	case v == nil && owner:
		return &Error{Code: CodeNotFound, Message: "Viewer not found"}
	case v == nil || (!owner && v.deviceID != p.Device.ID):
		return ErrSessionForbidden
	}
	reason := LeftLeft
	if owner {
		reason = LeftRemoved
	}
	s.removeViewerLocked(ctx, before, *v, reason)
	_, err = s.reconcile(ctx, before, nil, "")
	return err
}

// UsersWithPresence lists all users with their online state (GET /users).
func (s *Service) UsersWithPresence(ctx context.Context) ([]UserPresence, error) {
	us, err := s.ListUsers(ctx)
	if err != nil {
		return nil, err
	}
	out := make([]UserPresence, 0, len(us))
	for _, u := range us {
		out = append(out, UserPresence{ID: u.ID, DisplayName: u.DisplayName, Online: s.userOnline(u.ID)})
	}
	return out, nil
}

// ICEServers returns the effective external stun: URLs (never nil): the configured list, or the default STUN
// server when none is configured; empty only for an explicit "none".
func (s *Service) ICEServers() []string {
	s.sess.iceMu.RLock()
	defer s.sess.iceMu.RUnlock()
	return config.EffectiveICE(s.sess.ice)
}

// ---- shared mutation helpers (caller holds sess.mu) ----

// reconcile finishes a change of an active Session: viewers that are no longer allowed (or belong to a user in
// forceUsers) are removed, then the audience is told. inviteUser: that user's devices get session_invite.
func (s *Service) reconcile(ctx context.Context, before *sessionData, forceUsers map[string]bool, inviteUser string) (*sessionData, error) {
	after, err := s.loadSession(ctx, before.id)
	if err != nil {
		return nil, err
	}
	changed := false
	for _, v := range after.viewers {
		if !after.allows(v.userID, v.deviceID) || forceUsers[v.userID] {
			s.removeViewerLocked(ctx, after, v, LeftRevoked)
			changed = true
		}
	}
	if changed {
		if after, err = s.loadSession(ctx, before.id); err != nil {
			return nil, err
		}
	}
	s.broadcastSession(before, after, inviteUser)
	return after, nil
}

// removeViewerLocked marks the viewer left and sends viewer_left to the owner and the viewer device.
func (s *Service) removeViewerLocked(ctx context.Context, d *sessionData, v viewerData, reason string) {
	if _, err := s.db.ExecContext(ctx, `UPDATE session_viewers SET left_at = ?, left_reason = ? WHERE viewer_id = ? AND left_at IS NULL`,
		s.now().Unix(), reason, v.id); err != nil {
		return
	}
	s.cancelViewerGrace(v.id)
	payload := map[string]string{"session_id": d.id, "viewer_id": v.id, "reason": reason}
	s.sendTo(d.ownerDeviceID, "viewer_left", payload)
	s.sendTo(v.deviceID, "viewer_left", payload)
}

// endLocked ends a Session: all viewers leave, invites expire with it, the audience gets session_ended.
func (s *Service) endLocked(ctx context.Context, d *sessionData, reason string) error {
	now := s.now().Unix()
	if _, err := s.db.ExecContext(ctx, `UPDATE sessions SET ended_at = ?, end_reason = ? WHERE id = ? AND ended_at IS NULL`, now, reason, d.id); err != nil {
		return internal(err)
	}
	if _, err := s.db.ExecContext(ctx, `UPDATE session_viewers SET left_at = ?, left_reason = 'session_ended' WHERE session_id = ? AND left_at IS NULL`, now, d.id); err != nil {
		return internal(err)
	}
	s.cancelSessionGrace(d.id)
	for _, v := range d.viewers {
		s.cancelViewerGrace(v.id)
	}
	s.endTogether(scopeSession, d.id, sessionMembers(d)) // the ready check is cancelled and the pause cleared
	s.dropSessionChat(d.id)
	for _, c := range s.clients() {
		if d.allows(c.userID, c.deviceID) {
			c.sendMsg("session_ended", "", map[string]string{"session_id": d.id, "reason": reason})
		}
	}
	s.resendPresence(d.ownerDeviceID, nil, func(c *Client) bool { return d.allows(c.userID, c.deviceID) }) // session_id disappears
	return nil
}

// broadcastSession sends session_update (personalised) to every connected device that may see the Session and
// session_ended(no_longer_visible) to devices that could see it before and cannot now.
func (s *Service) broadcastSession(before, after *sessionData, inviteUser string) {
	if after.ended {
		recipients := sessionMembers(after)
		if before != nil {
			for _, u := range sessionMembers(before) {
				if !slices.Contains(recipients, u) {
					recipients = append(recipients, u)
				}
			}
		}
		s.endTogether(scopeSession, after.id, recipients)
		s.dropSessionChat(after.id)
	} else {
		s.reconcileTogether(scopeSession, after.id, sessionMembers(after))
	}
	for _, c := range s.clients() {
		now := after.allows(c.userID, c.deviceID)
		was := before != nil && before.allows(c.userID, c.deviceID)
		switch {
		case was && !now:
			c.sendMsg("session_ended", "", map[string]string{"session_id": after.id, "reason": EndNoLongerVisible})
		case now && c.userID == inviteUser && c.deviceID != after.ownerDeviceID:
			c.sendMsg("session_invite", "", map[string]any{"session": after.render(c.userID, c.deviceID, s.userOnline)})
		case now:
			c.sendMsg("session_update", "", map[string]any{"session": after.render(c.userID, c.deviceID, s.userOnline)})
		}
	}
	// session_id in the owner's presence appears or disappears for the devices whose access changed
	s.resendPresence(after.ownerDeviceID, after, func(c *Client) bool {
		return after.allows(c.userID, c.deviceID) != (before != nil && before.allows(c.userID, c.deviceID))
	})
}

// onDeviceRevoked ends the device's Session, removes its viewers and closes its WSS connection.
func (s *Service) onDeviceRevoked(deviceID string) {
	if t := s.turn.p.Load(); t != nil {
		if inv, ok := (*t).(interface{ InvalidateDevice(string) }); ok {
			inv.InvalidateDevice(deviceID) // drop the cached TURN authentication
		}
	}
	ctx := context.Background()
	s.sess.mu.Lock()
	defer s.sess.mu.Unlock()
	if ids, err := s.activeSessionIDs(ctx, `AND owner_device_id = ?`, deviceID); err == nil {
		for _, id := range ids {
			if d, err := s.loadSession(ctx, id); err == nil {
				s.endLocked(ctx, d, EndDeviceRevoked)
			}
		}
	}
	s.dropViewersOfDevice(ctx, deviceID, LeftRevoked)
	if c := s.clientOf(deviceID); c != nil {
		c.closeWith(CloseCodePolicy, "device_revoked")
	}
}

// dropViewersOfDevice removes all active viewers of a device from their Sessions.
func (s *Service) dropViewersOfDevice(ctx context.Context, deviceID, reason string) {
	rows, err := s.db.QueryContext(ctx, `SELECT v.session_id FROM session_viewers v JOIN sessions s ON s.id = v.session_id
		WHERE v.device_id = ? AND v.left_at IS NULL AND s.ended_at IS NULL`, deviceID)
	if err != nil {
		slog.Warn("drop viewers of device: query failed", "device_id", deviceID, "err", err)
		return
	}
	var ids []string
	for rows.Next() {
		var id string
		if err := rows.Scan(&id); err != nil {
			slog.Warn("drop viewers of device: scan failed", "device_id", deviceID, "err", err)
			continue
		}
		ids = append(ids, id)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		slog.Warn("drop viewers of device: iteration failed", "device_id", deviceID, "err", err)
	}
	for _, id := range ids {
		before, err := s.loadSession(ctx, id)
		if err != nil {
			continue
		}
		if v := before.viewerByDevice(deviceID); v != nil {
			s.removeViewerLocked(ctx, before, *v, reason)
			s.reconcile(ctx, before, nil, "")
		}
	}
}
