package hub

import (
	"context"
	"encoding/json"
	"slices"
	"sync"
	"time"

	"github.com/google/uuid"
)

// Ready check and shared pause (0.23 Together, ADR 0033 D3). Everything lives in memory: nothing is persisted and
// a server restart forgets open checks and pauses.
//
// Scopes: "run" (members = participants with status joined) and "session" (members = the owner user plus the
// users of the active viewers). A user with several devices is one member: answers count once and every
// connected device of the user receives the messages. Locking: tg.mu is a leaf lock, never held while sending,
// reading the database or taking another lock.

const (
	scopeRun     = "run"
	scopeSession = "session"

	// DefaultReadyCheckTTL is how long a ready check stays open.
	DefaultReadyCheckTTL = 30 * time.Second

	readyPending  = "pending"
	readyReady    = "ready"
	readyNotReady = "not_ready"

	checkOpen     = "open"
	checkAllReady = "all_ready"
	checkCancel   = "cancelled"
	checkExpired  = "expired"
)

type scopeKey struct{ scope, id string }

type readyCheck struct {
	id        string
	key       scopeKey
	by        string
	expiresAt time.Time
	order     []string          // member user IDs in stable order
	state     map[string]string // user ID -> pending|ready|not_ready
	timer     *time.Timer       // nil while the check is not registered
}

type pauseInfo struct{ by string }

type togetherState struct {
	mu     sync.Mutex
	ttl    time.Duration
	closed bool
	checks map[string]*readyCheck // check ID -> open check
	scope  map[scopeKey]string    // scope -> open check ID (at most one)
	pauses map[scopeKey]pauseInfo
}

func (t *togetherState) init(o Options) {
	t.ttl = o.ReadyCheckTTL
	if t.ttl <= 0 {
		t.ttl = DefaultReadyCheckTTL
	}
	t.checks = map[string]*readyCheck{}
	t.scope = map[scopeKey]string{}
	t.pauses = map[scopeKey]pauseInfo{}
}

// stopTogether stops the expiry timers (service shutdown).
func (s *Service) stopTogether() {
	t := &s.tg
	t.mu.Lock()
	defer t.mu.Unlock()
	t.closed = true
	for _, c := range t.checks {
		c.timer.Stop()
	}
}

// membersOf returns the member user IDs of a run or session; ok is false when it does not exist, is over (an
// ended session) or the scope is unknown.
func (s *Service) membersOf(ctx context.Context, k scopeKey) (members []string, ok bool) {
	members, _, ok = s.membersOfState(ctx, k)
	return members, ok
}

// membersOfState is membersOf plus whether the scope is live for Together: a run is live only while it is active
// (not starting, paused, finished, abandoned or archived), a session while it has not ended. Ready checks and
// pauses may only be started on a live scope; chat is allowed on any existing one.
func (s *Service) membersOfState(ctx context.Context, k scopeKey) (members []string, live, ok bool) {
	switch k.scope {
	case scopeRun:
		run, err := loadRun(ctx, s.db, k.id, false)
		if err != nil {
			return nil, false, false
		}
		for _, p := range run.Participants {
			if p.Status == ParticipantJoined {
				members = append(members, p.UserID)
			}
		}
		return members, run.Status == RunActive && !run.Archived, true
	case scopeSession:
		d, err := s.loadSession(ctx, k.id)
		if err != nil || d.ended {
			return nil, false, false
		}
		return sessionMembers(d), true, true
	}
	return nil, false, false
}

func sessionMembers(d *sessionData) []string {
	out := []string{d.ownerUserID}
	for _, v := range d.viewers {
		if !slices.Contains(out, v.userID) {
			out = append(out, v.userID)
		}
	}
	return out
}

// sendToUsers queues a message to every connected device of the given users.
func (s *Service) sendToUsers(users []string, typ, id string, payload any) {
	b := encode(typ, id, payload)
	for _, c := range s.clients() {
		if slices.Contains(users, c.userID) {
			c.sendRaw(b)
		}
	}
}

// snapshot renders the ready_check payload with the given status. Callers hold tg.mu.
func (c *readyCheck) snapshot(status string) map[string]any {
	members := make([]map[string]string, 0, len(c.order))
	for _, u := range c.order {
		members = append(members, map[string]string{"user_id": u, "state": c.state[u]})
	}
	return map[string]any{"check_id": c.id, "scope": c.key.scope, "id": c.key.id, "by": c.by, "status": status,
		"expires_at": c.expiresAt.UTC().Format(time.RFC3339), "members": members}
}

// evalLocked closes the check as all_ready when every remaining member is ready. It returns the status to send.
func (t *togetherState) evalLocked(c *readyCheck) string {
	for _, u := range c.order {
		if c.state[u] != readyReady {
			return checkOpen
		}
	}
	t.dropLocked(c)
	return checkAllReady
}

func (t *togetherState) dropLocked(c *readyCheck) {
	if c.timer != nil {
		c.timer.Stop()
	}
	delete(t.checks, c.id)
	if t.scope[c.key] == c.id {
		delete(t.scope, c.key)
	}
}

// expireIfDueLocked closes a check whose time ran out. It is also checked lazily on every access, so the
// injectable clock works without waiting for the real timer.
func (s *Service) expireIfDueLocked(c *readyCheck) (map[string]any, []string) {
	if s.now().Before(c.expiresAt) {
		return nil, nil
	}
	s.tg.dropLocked(c)
	return c.snapshot(checkExpired), slices.Clone(c.order)
}

func (s *Service) expireReadyCheck(id string) {
	t := &s.tg
	t.mu.Lock()
	c := t.checks[id]
	if c == nil || t.closed {
		t.mu.Unlock()
		return
	}
	t.dropLocked(c)
	snap, to := c.snapshot(checkExpired), slices.Clone(c.order)
	t.mu.Unlock()
	s.sendToUsers(to, "ready_check", "", snap)
}

// ---- WSS handlers ----

type scopeMsg struct {
	Scope  string `json:"scope"`
	ID     string `json:"id"`
	Paused *bool  `json:"paused"`
}

// scopeMember parses a scope message and verifies that the sender is a member; it answers the error itself.
func (c *Client) scopeMember(env envelope) (scopeMsg, scopeKey, []string, bool) {
	m, k, members, _, ok := c.scopeMemberLive(env)
	return m, k, members, ok
}

// scopeMemberLive is scopeMember plus whether the scope is live (see membersOfState).
func (c *Client) scopeMemberLive(env envelope) (scopeMsg, scopeKey, []string, bool, bool) {
	var m scopeMsg
	if json.Unmarshal(env.Payload, &m) != nil || (m.Scope != scopeRun && m.Scope != scopeSession) || m.ID == "" {
		c.sendError(env.ID, CodeBadRequest, "scope must be run or session and id is required")
		return m, scopeKey{}, nil, false, false
	}
	k := scopeKey{m.Scope, m.ID}
	members, live, ok := c.svc.membersOfState(context.Background(), k)
	if !ok || !slices.Contains(members, c.userID) { // unknown, over and foreign look the same
		c.sendError(env.ID, CodeForbidden, "Not a member of this "+m.Scope)
		return m, k, nil, false, false
	}
	return m, k, members, live, true
}

// mayControl reports whether the sender may start a ready check and pause or resume for the scope: in a run every
// member, in a Session only the owner (the host; ADR 0033 D5: viewers watch, chat and talk). The owner is the first
// of sessionMembers. Answering a check stays open to every member.
func (c *Client) mayControl(k scopeKey, members []string) bool {
	return k.scope != scopeSession || (len(members) > 0 && members[0] == c.userID)
}

// readyCheckStart opens a check. Only one check per scope can be open: starting another one while it is open
// is not an error, the current check is sent to the requester again (idempotent for a double click).
func (c *Client) readyCheckStart(env envelope) {
	_, k, members, live, ok := c.scopeMemberLive(env)
	if !ok {
		return
	}
	if !c.mayControl(k, members) {
		c.sendError(env.ID, CodeForbidden, "Only the host starts a ready check in a Session")
		return
	}
	if !live {
		c.sendError(env.ID, CodeConflict, "The "+k.scope+" is not active")
		return
	}
	s := c.svc
	t := &s.tg
	t.mu.Lock()
	if t.closed {
		t.mu.Unlock()
		return
	}
	var expired map[string]any
	var expiredTo []string
	if cid, open := t.scope[k]; open {
		cur := t.checks[cid]
		if expired, expiredTo = s.expireIfDueLocked(cur); expired == nil {
			snap := cur.snapshot(checkOpen)
			t.mu.Unlock()
			c.sendMsg("ready_check", env.ID, snap)
			return
		}
	}
	chk := &readyCheck{id: uuid.NewString(), key: k, by: c.userID, expiresAt: s.now().Add(t.ttl),
		order: slices.Clone(members), state: map[string]string{}}
	for _, u := range members {
		chk.state[u] = readyPending
	}
	chk.state[c.userID] = readyReady // the starter is ready
	status := t.evalLocked(chk)
	if status == checkOpen {
		t.checks[chk.id], t.scope[k] = chk, chk.id
		chk.timer = time.AfterFunc(t.ttl, func() { s.expireReadyCheck(chk.id) })
	}
	snap := chk.snapshot(status)
	t.mu.Unlock()
	if expired != nil {
		s.sendToUsers(expiredTo, "ready_check", "", expired)
	}
	s.sendToUsers(members, "ready_check", "", snap)
	if status == checkOpen {
		s.resyncMembers(k)
	}
}

// resyncMembers re-reads the members of a scope after a check was registered with a list read before tg.mu was
// taken: a membership change in between would otherwise leave the check with stale members (the change's own
// reconcile found no check yet). A scope that is gone meanwhile drops the check and the pause. No database call
// under tg.mu.
func (s *Service) resyncMembers(k scopeKey) {
	members, ok := s.membersOf(context.Background(), k)
	if !ok {
		members = nil
	}
	s.reconcileTogether(k.scope, k.id, members)
}

func (c *Client) readyCheckAnswer(env envelope) {
	var m struct {
		CheckID string `json:"check_id"`
		Ready   *bool  `json:"ready"`
	}
	if json.Unmarshal(env.Payload, &m) != nil || m.CheckID == "" || m.Ready == nil {
		c.sendError(env.ID, CodeBadRequest, "check_id and ready are required")
		return
	}
	s := c.svc
	t := &s.tg
	t.mu.Lock()
	chk := t.checks[m.CheckID]
	if chk == nil {
		t.mu.Unlock()
		c.sendError(env.ID, CodeNotFound, "Ready check is not open")
		return
	}
	if snap, to := s.expireIfDueLocked(chk); snap != nil {
		t.mu.Unlock()
		s.sendToUsers(to, "ready_check", "", snap)
		c.sendError(env.ID, CodeNotFound, "Ready check is not open")
		return
	}
	if _, member := chk.state[c.userID]; !member {
		t.mu.Unlock()
		c.sendError(env.ID, CodeForbidden, "Not a member of this ready check")
		return
	}
	chk.state[c.userID] = readyNotReady
	if *m.Ready {
		chk.state[c.userID] = readyReady
	}
	snap, to := chk.snapshot(t.evalLocked(chk)), slices.Clone(chk.order)
	t.mu.Unlock()
	s.sendToUsers(to, "ready_check", "", snap)
}

func (c *Client) readyCheckCancel(env envelope) {
	var m struct {
		CheckID string `json:"check_id"`
	}
	if json.Unmarshal(env.Payload, &m) != nil || m.CheckID == "" {
		c.sendError(env.ID, CodeBadRequest, "check_id is required")
		return
	}
	s := c.svc
	t := &s.tg
	t.mu.Lock()
	chk := t.checks[m.CheckID]
	if chk == nil {
		t.mu.Unlock()
		c.sendError(env.ID, CodeNotFound, "Ready check is not open")
		return
	}
	if snap, to := s.expireIfDueLocked(chk); snap != nil {
		t.mu.Unlock()
		s.sendToUsers(to, "ready_check", "", snap)
		c.sendError(env.ID, CodeNotFound, "Ready check is not open")
		return
	}
	if chk.by != c.userID {
		t.mu.Unlock()
		c.sendError(env.ID, CodeForbidden, "Only the starter can cancel a ready check")
		return
	}
	t.dropLocked(chk)
	snap, to := chk.snapshot(checkCancel), slices.Clone(chk.order)
	t.mu.Unlock()
	s.sendToUsers(to, "ready_check", "", snap)
}

// setPause enters the shared pause of a scope and then looks the scope up again: when it ended between the caller's
// check and the entry (its cleanup found no pause yet), the entry is removed again, so an ended scope never keeps a
// pause. It returns the current members; ok is false when the scope is gone. The database is read outside tg.mu.
func (s *Service) setPause(ctx context.Context, k scopeKey, by string) (members []string, ok bool) {
	t := &s.tg
	t.mu.Lock()
	t.pauses[k] = pauseInfo{by: by}
	t.mu.Unlock()
	if members, ok = s.membersOf(ctx, k); !ok {
		s.reconcileTogether(k.scope, k.id, nil)
		return nil, false
	}
	return members, true
}

// pauseRequest sets the shared pause of a run or session; every member may pause and resume. The request is
// idempotent and always answered with pause_state to all members.
func (c *Client) pauseRequest(env envelope) {
	m, k, members, live, ok := c.scopeMemberLive(env)
	if !ok {
		return
	}
	if m.Paused == nil {
		c.sendError(env.ID, CodeBadRequest, "paused is required")
		return
	}
	if !c.mayControl(k, members) {
		c.sendError(env.ID, CodeForbidden, "Only the host pauses or resumes a Session")
		return
	}
	if *m.Paused && !live { // resuming stays possible; a new pause needs an active run or a running session
		c.sendError(env.ID, CodeConflict, "The "+k.scope+" is not active")
		return
	}
	if *m.Paused {
		cur, ok := c.svc.setPause(context.Background(), k, c.userID)
		if !ok {
			c.sendError(env.ID, CodeForbidden, "Not a member of this "+k.scope)
			return
		}
		members = cur
	} else {
		t := &c.svc.tg
		t.mu.Lock()
		delete(t.pauses, k)
		t.mu.Unlock()
	}
	c.svc.sendToUsers(members, "pause_state", "", map[string]any{"scope": k.scope, "id": k.id, "paused": *m.Paused, "by": c.userID})
}

// ---- membership changes and catch-up ----

// reconcileTogether applies a membership change of a run or session (members nil = it ended or was deleted):
// members who left are removed from an open check (the starter leaving cancels it, all remaining ready closes it),
// new members join it as pending, and an empty scope drops its check and pause.
func (s *Service) reconcileTogether(scope, id string, members []string) {
	s.reconcileVoice(scope, id, members)
	t := &s.tg
	k := scopeKey{scope, id}
	t.mu.Lock()
	if len(t.checks) == 0 && len(t.pauses) == 0 {
		t.mu.Unlock()
		return
	}
	if len(members) == 0 {
		delete(t.pauses, k)
	}
	cid, open := t.scope[k]
	if !open {
		t.mu.Unlock()
		return
	}
	chk := t.checks[cid]
	to := slices.Clone(chk.order) // the ones who left learn about the change too
	var kept []string
	for _, u := range chk.order {
		if slices.Contains(members, u) {
			kept = append(kept, u)
		}
	}
	for _, u := range members {
		if !slices.Contains(kept, u) {
			kept = append(kept, u)
			chk.state[u] = readyPending
			to = append(to, u)
		}
	}
	if slices.Equal(kept, chk.order) {
		t.mu.Unlock()
		return
	}
	for u := range chk.state {
		if !slices.Contains(kept, u) {
			delete(chk.state, u)
		}
	}
	chk.order = kept
	var snap map[string]any
	if len(kept) == 0 || !slices.Contains(kept, chk.by) {
		t.dropLocked(chk)
		snap = chk.snapshot(checkCancel)
	} else {
		snap = chk.snapshot(t.evalLocked(chk))
	}
	t.mu.Unlock()
	s.sendToUsers(to, "ready_check", "", snap)
}

// togetherCatchUp sends the current pause state and open ready check to a device that just attached.
func (s *Service) togetherCatchUp(c *Client) {
	t := &s.tg
	t.mu.Lock()
	type item struct {
		typ     string
		payload map[string]any
		key     scopeKey
	}
	var items []item
	type expiredItem struct {
		snap map[string]any
		to   []string
	}
	var expired []expiredItem
	for k, p := range t.pauses {
		items = append(items, item{"pause_state", map[string]any{"scope": k.scope, "id": k.id, "paused": true, "by": p.by}, k})
	}
	for _, chk := range t.checks {
		if _, member := chk.state[c.userID]; member {
			if snap, to := s.expireIfDueLocked(chk); snap != nil { // deleting while ranging over a map is allowed
				expired = append(expired, expiredItem{snap, to})
				continue
			}
			items = append(items, item{"ready_check", chk.snapshot(checkOpen), chk.key})
		}
	}
	t.mu.Unlock()
	for _, e := range expired {
		s.sendToUsers(e.to, "ready_check", "", e.snap)
	}
	for _, it := range items {
		if it.typ == "pause_state" { // pauses do not list members: check the membership now
			if members, ok := s.membersOf(context.Background(), it.key); !ok || !slices.Contains(members, c.userID) {
				continue
			}
		}
		c.sendMsg(it.typ, "", it.payload)
	}
}

// endTogether ends the ready check (cancelled) and clears the pause of a scope that is over for Together (a run that
// finished, was abandoned, archived or paused by the run lifecycle). A cleared pause is announced to recipients.
func (s *Service) endTogether(scope, id string, recipients []string) {
	t := &s.tg
	k := scopeKey{scope, id}
	t.mu.Lock()
	_, wasPaused := t.pauses[k]
	t.mu.Unlock()
	s.reconcileTogether(scope, id, nil) // cancels the check, clears the pause
	if wasPaused {
		s.sendToUsers(recipients, "pause_state", "", map[string]any{"scope": scope, "id": id, "paused": false, "by": ""})
	}
}
