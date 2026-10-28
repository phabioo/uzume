package hub

import (
	"context"
	"time"

	"github.com/phabioo/uzume/server/internal/turnsrv"
)

// Test seams for the grace-timer generation check.

// ArmOwnerGraceForTest arms the owner grace timer and returns its generation.
func (s *Service) ArmOwnerGraceForTest(sessionID, deviceID string) uint64 {
	s.armOwnerGrace(sessionID, deviceID)
	rt := &s.sess.rt
	rt.mu.Lock()
	defer rt.mu.Unlock()
	return rt.ownerTimers[sessionID].gen
}

// OwnerGraceExpiredForTest runs the timer callback of the given generation.
func (s *Service) OwnerGraceExpiredForTest(sessionID, deviceID string, gen uint64) {
	s.ownerGraceExpired(sessionID, deviceID, gen)
}

// OwnerGraceArmedForTest reports whether an owner grace timer is registered.
func (s *Service) OwnerGraceArmedForTest(sessionID string) bool {
	rt := &s.sess.rt
	rt.mu.Lock()
	defer rt.mu.Unlock()
	_, ok := rt.ownerTimers[sessionID]
	return ok
}

// ArmViewerGraceForTest arms the viewer grace timer and returns its generation.
func (s *Service) ArmViewerGraceForTest(viewerID, deviceID string) uint64 {
	s.armViewerGrace(viewerID, deviceID)
	rt := &s.sess.rt
	rt.mu.Lock()
	defer rt.mu.Unlock()
	return rt.viewerTimers[viewerID].gen
}

// ViewerGraceExpiredForTest runs the timer callback of the given generation.
func (s *Service) ViewerGraceExpiredForTest(viewerID, deviceID string, gen uint64) {
	s.viewerGraceExpired(viewerID, deviceID, gen)
}

// ViewerGraceArmedForTest reports whether a viewer grace timer is registered.
func (s *Service) ViewerGraceArmedForTest(viewerID string) bool {
	rt := &s.sess.rt
	rt.mu.Lock()
	defer rt.mu.Unlock()
	_, ok := rt.viewerTimers[viewerID]
	return ok
}

// SetRawSettingForTest stores a raw settings value (legacy values).
func (s *Service) SetRawSettingForTest(ctx context.Context, key, value string) error {
	return s.setSetting(ctx, key, value)
}

// CleanTextForTest exposes cleanText.
func CleanTextForTest(v string, max int) string { return cleanText(v, max) }

// CheckCoresForTest exposes the handshake core check.
func CheckCoresForTest(reg []SystemEntry, cores []CoreReport) []Problem {
	return checkCores(reg, cores)
}

// CloudflareTURNURLs returns the TURN URLs of the cached Cloudflare credentials (tests only).
func (s *Service) CloudflareTURNURLs() []string {
	_, c := s.cf.current()
	if c == nil {
		return nil
	}
	return c.URLs
}

// ICEServersFor returns the ice_servers a device would get (tests only).
func (s *Service) ICEServersFor(reqHost, deviceID string) []string {
	ice, _ := s.iceFor(reqHost, deviceID)
	return ice
}

// ExternalCredentialsForTest returns the external TURN credentials issued for a device.
func (s *Service) ExternalCredentialsForTest(deviceID string) []turnsrv.Credentials {
	return s.externalFor(deviceID)
}

// SetFreeBytesForTest replaces the free-space probe of ROM uploads and returns the function that restores it.
func SetFreeBytesForTest(fn func(dir string) uint64) (restore func()) {
	old := freeBytesFn
	freeBytesFn = fn
	return func() { freeBytesFn = old }
}

// Test seams for the Together races (a scope that ends between a check and a registration).

// SetPauseForTest enters a shared pause the way pauseRequest does (with the look-up afterwards).
func (s *Service) SetPauseForTest(scope, id, by string) bool {
	_, ok := s.setPause(context.Background(), scopeKey{scope, id}, by)
	return ok
}

// AppendSessionChatForTest appends to a Session ring the way chat_send does and reports whether it was kept.
func (s *Service) AppendSessionChatForTest(id, userID, text string) bool {
	_, ok := s.appendSessionChatChecked(context.Background(), id, ChatMessage{UserID: userID, Text: text, ClientID: "t"})
	return ok
}

// SessionChatRingForTest reports whether an in-memory chat ring exists for the Session.
func (s *Service) SessionChatRingForTest(id string) bool {
	s.chat.mu.Lock()
	defer s.chat.mu.Unlock()
	return s.chat.rings[id] != nil
}

// PauseForTest reports whether a shared pause is registered for the scope.
func (s *Service) PauseForTest(scope, id string) bool {
	s.tg.mu.Lock()
	defer s.tg.mu.Unlock()
	_, ok := s.tg.pauses[scopeKey{scope, id}]
	return ok
}

// RegisterStaleCheckForTest registers an open ready check with the given (possibly outdated) member list and then
// runs the re-read that readyCheckStart does after registering.
func (s *Service) RegisterStaleCheckForTest(scope, id, by string, stale []string) {
	t := &s.tg
	k := scopeKey{scope, id}
	chk := &readyCheck{id: "stale-check", key: k, by: by, expiresAt: s.now().Add(t.ttl), order: stale, state: map[string]string{}}
	for _, u := range stale {
		chk.state[u] = readyPending
	}
	chk.state[by] = readyReady
	chk.timer = time.AfterFunc(time.Hour, func() {})
	t.mu.Lock()
	t.checks[chk.id], t.scope[k] = chk, chk.id
	t.mu.Unlock()
	s.resyncMembers(k)
}
