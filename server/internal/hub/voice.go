package hub

import (
	"context"
	"encoding/json"
	"slices"
	"sync"
)

// Voice chat signaling (0.23, ADR 0033 D7): rooms are a run or a session (at most voiceRoomMax devices), the
// audio itself is a P2P mesh between the apps. The hub only knows who is in which room and relays offer / answer
// / candidate between devices of the same room. Rooms live in memory only. The TURN servers are the ones the app
// already gets in hello_ack / ice_update; nothing extra is announced here.

// voiceRoomMax is the number of devices in one voice room.
const voiceRoomMax = 8

const (
	micOn  = "on"
	micOff = "off"
	micPTT = "ptt"
)

type voiceMember struct {
	userID   string
	deviceID string
	mic      string
}

type voiceRoom struct {
	members []*voiceMember // join order
}

type voiceState struct {
	mu        sync.Mutex
	rooms     map[scopeKey]*voiceRoom
	byDevice  map[string]scopeKey          // device ID -> its room (at most one)
	hostMuted map[scopeKey]map[string]bool // session -> user ID -> muted by the host; lives as long as the session
}

func (v *voiceState) init() {
	v.rooms = map[scopeKey]*voiceRoom{}
	v.byDevice = map[string]scopeKey{}
	v.hostMuted = map[scopeKey]map[string]bool{}
}

func (r *voiceRoom) find(deviceID string) *voiceMember {
	for _, m := range r.members {
		if m.deviceID == deviceID {
			return m
		}
	}
	return nil
}

// voicePending is one voice_room broadcast prepared under the lock and sent after it is released.
type voicePending struct {
	msg []byte
	to  []string
}

// broadcastLocked renders the voice_room payload for k; it goes to the devices in the room and to extra (devices
// that just left). Callers hold v.mu.
func (v *voiceState) broadcastLocked(k scopeKey, extra ...string) voicePending {
	members := []map[string]any{}
	p := voicePending{to: slices.Clone(extra)}
	if r := v.rooms[k]; r != nil {
		for _, m := range r.members {
			members = append(members, map[string]any{"user_id": m.userID, "device_id": m.deviceID, "mic": m.mic, "muted_by_host": v.hostMuted[k][m.userID]})
			if !slices.Contains(p.to, m.deviceID) {
				p.to = append(p.to, m.deviceID)
			}
		}
	}
	p.msg = encode("voice_room", "", map[string]any{"scope": k.scope, "id": k.id, "members": members})
	return p
}

// removeLocked takes a device out of its room and reports which one it was.
func (v *voiceState) removeLocked(deviceID string) (k scopeKey, ok bool) {
	k, ok = v.byDevice[deviceID]
	if !ok {
		return k, false
	}
	delete(v.byDevice, deviceID)
	if r := v.rooms[k]; r != nil {
		r.members = slices.DeleteFunc(r.members, func(m *voiceMember) bool { return m.deviceID == deviceID })
		if len(r.members) == 0 {
			delete(v.rooms, k)
		}
	}
	return k, true
}

func (v *voiceState) inRoomLocked(deviceID string, k scopeKey) bool {
	cur, in := v.byDevice[deviceID]
	return in && cur == k
}

func (s *Service) sendVoice(ps ...voicePending) {
	for _, p := range ps {
		for _, d := range p.to {
			if c := s.clientOf(d); c != nil {
				c.sendRaw(p.msg)
			}
		}
	}
}

// voiceDeviceGone removes a disconnected device from its room (no grace: the app rejoins on reconnect).
func (s *Service) voiceDeviceGone(deviceID string) {
	v := &s.voice
	v.mu.Lock()
	k, ok := v.removeLocked(deviceID)
	var p voicePending
	if ok {
		p = v.broadcastLocked(k)
	}
	v.mu.Unlock()
	if ok {
		s.sendVoice(p)
	}
}

// reconcileVoice applies a membership change of a run or session (members empty = it ended or was deleted): the
// devices of users who left are removed, an ended scope closes its room and forgets the host mutes.
func (s *Service) reconcileVoice(scope, id string, members []string) {
	v := &s.voice
	k := scopeKey{scope, id}
	v.mu.Lock()
	if len(members) == 0 {
		delete(v.hostMuted, k)
	}
	r := v.rooms[k]
	if r == nil {
		v.mu.Unlock()
		return
	}
	var out []string
	for _, m := range slices.Clone(r.members) {
		if len(members) == 0 || !slices.Contains(members, m.userID) {
			v.removeLocked(m.deviceID)
			out = append(out, m.deviceID)
		}
	}
	if len(out) == 0 {
		v.mu.Unlock()
		return
	}
	p := v.broadcastLocked(k, out...) // the ones removed see the room without them
	v.mu.Unlock()
	s.sendVoice(p)
}

// ---- WSS handlers ----

type voiceMsg struct {
	Scope string `json:"scope"`
	ID    string `json:"id"`
	Mic   string `json:"mic"`
	Muted *bool  `json:"muted"`
	User  string `json:"user_id"`
}

func validMic(m string) bool { return m == micOn || m == micOff || m == micPTT }

// voiceJoin puts the device into the voice room of a run or session it is a member of. A device is in one room
// at a time: joining another one leaves the old one. The optional mic defaults to push-to-talk.
func (c *Client) voiceJoin(env envelope) {
	var m voiceMsg
	_ = json.Unmarshal(env.Payload, &m)
	_, k, _, ok := c.scopeMember(env)
	if !ok {
		return
	}
	if m.Mic == "" {
		m.Mic = micPTT
	}
	if !validMic(m.Mic) {
		c.sendError(env.ID, CodeBadRequest, "mic must be on, off or ptt")
		return
	}
	s := c.svc
	v := &s.voice
	v.mu.Lock()
	var ps []voicePending
	if r := v.rooms[k]; (r == nil || r.find(c.deviceID) == nil) && r != nil && len(r.members) >= voiceRoomMax {
		v.mu.Unlock()
		c.sendError(env.ID, CodeConflict, "The voice room is full")
		return
	}
	if old, in := v.byDevice[c.deviceID]; in && old != k {
		v.removeLocked(c.deviceID)
		ps = append(ps, v.broadcastLocked(old))
	}
	r := v.rooms[k]
	if r == nil {
		r = &voiceRoom{}
		v.rooms[k] = r
	}
	if me := r.find(c.deviceID); me != nil {
		me.mic = m.Mic
	} else {
		r.members = append(r.members, &voiceMember{userID: c.userID, deviceID: c.deviceID, mic: m.Mic})
		v.byDevice[c.deviceID] = k
	}
	ps = append(ps, v.broadcastLocked(k))
	v.mu.Unlock()
	s.sendVoice(ps...)
}

func (c *Client) voiceLeave(env envelope) {
	var m voiceMsg
	if json.Unmarshal(env.Payload, &m) != nil || (m.Scope != scopeRun && m.Scope != scopeSession) || m.ID == "" {
		c.sendError(env.ID, CodeBadRequest, "scope must be run or session and id is required")
		return
	}
	k := scopeKey{m.Scope, m.ID}
	s := c.svc
	v := &s.voice
	v.mu.Lock()
	if !v.inRoomLocked(c.deviceID, k) { // leaving a room one is not in is harmless
		v.mu.Unlock()
		return
	}
	v.removeLocked(c.deviceID)
	p := v.broadcastLocked(k, c.deviceID)
	v.mu.Unlock()
	s.sendVoice(p)
}

func (c *Client) voiceStateMsg(env envelope) {
	var m voiceMsg
	if json.Unmarshal(env.Payload, &m) != nil || (m.Scope != scopeRun && m.Scope != scopeSession) || m.ID == "" || !validMic(m.Mic) {
		c.sendError(env.ID, CodeBadRequest, "scope, id and mic (on|off|ptt) are required")
		return
	}
	k := scopeKey{m.Scope, m.ID}
	s := c.svc
	v := &s.voice
	v.mu.Lock()
	if !v.inRoomLocked(c.deviceID, k) {
		v.mu.Unlock()
		c.sendError(env.ID, CodeForbidden, "Not in this voice room")
		return
	}
	v.rooms[k].find(c.deviceID).mic = m.Mic
	p := v.broadcastLocked(k)
	v.mu.Unlock()
	s.sendVoice(p)
}

// voiceMute lets the owner of a session mute or unmute one member for everyone. The flag stays for the
// lifetime of the session, also while the member is out of the room.
func (c *Client) voiceMute(env envelope) {
	var m voiceMsg
	if json.Unmarshal(env.Payload, &m) != nil || m.Scope != scopeSession || m.ID == "" || m.User == "" || m.Muted == nil {
		c.sendError(env.ID, CodeBadRequest, "scope session, id, user_id and muted are required")
		return
	}
	k := scopeKey{m.Scope, m.ID}
	s := c.svc
	d, err := s.loadSession(context.Background(), m.ID)
	if err != nil || d.ended || d.ownerUserID != c.userID {
		c.sendError(env.ID, CodeForbidden, "Only the owner of the Session can mute")
		return
	}
	if !slices.Contains(sessionMembers(d), m.User) {
		c.sendError(env.ID, CodeNotFound, "Not a member of this session")
		return
	}
	v := &s.voice
	v.mu.Lock()
	if *m.Muted {
		if v.hostMuted[k] == nil {
			v.hostMuted[k] = map[string]bool{}
		}
		v.hostMuted[k][m.User] = true
	} else {
		delete(v.hostMuted[k], m.User)
	}
	p := v.broadcastLocked(k)
	v.mu.Unlock()
	s.sendVoice(p)
}

// voiceSignal relays offer / answer / candidate to one other device of the same room. The payload is passed on
// verbatim, only to_device_id is replaced by from_device_id.
func (c *Client) voiceSignal(env envelope) {
	var raw map[string]json.RawMessage
	var m struct {
		Scope string `json:"scope"`
		ID    string `json:"id"`
		To    string `json:"to_device_id"`
		Kind  string `json:"kind"`
	}
	if json.Unmarshal(env.Payload, &raw) != nil || json.Unmarshal(env.Payload, &m) != nil ||
		(m.Scope != scopeRun && m.Scope != scopeSession) || m.ID == "" || m.To == "" ||
		(m.Kind != "offer" && m.Kind != "answer" && m.Kind != "candidate") {
		c.sendError(env.ID, CodeBadRequest, "voice_signal needs scope, id, to_device_id and kind offer|answer|candidate")
		return
	}
	k := scopeKey{m.Scope, m.ID}
	s := c.svc
	v := &s.voice
	v.mu.Lock()
	ok := v.inRoomLocked(c.deviceID, k) && m.To != c.deviceID && v.rooms[k].find(m.To) != nil
	v.mu.Unlock()
	if !ok {
		c.sendError(env.ID, CodeForbidden, "Voice signaling is only allowed between devices of the same room")
		return
	}
	peer := s.clientOf(m.To)
	if peer == nil {
		c.sendError(env.ID, CodeNotFound, "Peer is not connected")
		return
	}
	delete(raw, "to_device_id")
	raw["from_device_id"], _ = json.Marshal(c.deviceID)
	b, _ := json.Marshal(raw)
	peer.sendRaw(encode("voice_signal", env.ID, json.RawMessage(b)))
}
