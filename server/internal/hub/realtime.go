package hub

import (
	"context"
	"encoding/json"
	"errors"
	"net"
	"net/netip"
	"sync"
	"time"

	"github.com/google/uuid"

	"github.com/phabioo/uzume/server/internal/config"
)

// Realtime layer (ADR 0006 D3): the server-side protocol of the WSS endpoint, independent of the WebSocket
// library. The transport (internal/httpapi) feeds incoming text frames to Client.Handle and writes
// Client.Out(); it closes the connection when Client.Done() fires.

// CloseCodePolicy is the WebSocket close code (1008, policy violation) used for hub-initiated closes.
const CloseCodePolicy = 1008

const outBuffer = 64

// Incoming message rate limit per connection (token bucket). ICE candidates for one Session arrive in bursts
// of a few dozen messages, so the burst leaves headroom; the sustained rate is far above normal use.
const (
	wsMsgRate  = 50.0 // messages per second
	wsMsgBurst = 300.0
)

// maxPreHelloConns is how many connections per device may be open without having completed hello.
const maxPreHelloConns = 2

// presenceReserve is the free space in a client's out queue that presence broadcasts must leave for other
// messages (signals, errors, Session updates): presence is dropped for a busy receiver instead of closing it.
const presenceReserve = outBuffer / 2

type sessionState struct {
	mu    sync.Mutex // serializes Session mutations; lock order: mu, then rt.mu
	iceMu sync.RWMutex
	ice   []string // stun: URLs (nil = default STUN); guarded by iceMu (changed live from the app settings)
	grace time.Duration

	rt rtState
}

type timerEntry struct {
	t        *time.Timer
	deviceID string
	gen      uint64 // identifies the arming; a callback of a superseded timer must not act
}

type rtState struct {
	mu           sync.Mutex
	conns        map[string]*Client    // device ID -> connection
	ownerTimers  map[string]timerEntry // session ID -> owner grace timer
	viewerTimers map[string]timerEntry // viewer ID -> "never connected" timer
	timerGen     uint64                // last timerEntry generation handed out
	closed       bool                  // Service.Close ran: no new timers
	preHello     map[string]int        // device ID -> open connections that have not completed hello
}

func (ss *sessionState) init(o Options) {
	ss.ice = config.CloneICE(o.ICEServers)
	ss.grace = o.OwnerGrace
	if ss.grace <= 0 {
		ss.grace = DefaultOwnerGrace
	}
	ss.rt.conns = map[string]*Client{}
	ss.rt.preHello = map[string]int{}
	ss.rt.ownerTimers = map[string]timerEntry{}
	ss.rt.viewerTimers = map[string]timerEntry{}
}

// recoverSessions runs on start: Sessions that were active before a restart survive for the grace period
// if their owner (and viewers) reconnect; otherwise they end like after a dropped connection.
func (s *Service) recoverSessions(ctx context.Context) {
	s.sess.mu.Lock()
	defer s.sess.mu.Unlock()
	ids, err := s.activeSessionIDs(ctx)
	if err != nil {
		return
	}
	for _, id := range ids {
		d, err := s.loadSession(ctx, id)
		if err != nil {
			continue
		}
		s.armOwnerGrace(d.id, d.ownerDeviceID)
		for _, v := range d.viewers {
			s.armViewerGrace(v.id, v.deviceID)
		}
	}
}

// ---- registry ----

func (s *Service) clients() []*Client {
	rt := &s.sess.rt
	rt.mu.Lock()
	defer rt.mu.Unlock()
	out := make([]*Client, 0, len(rt.conns))
	for _, c := range rt.conns {
		out = append(out, c)
	}
	return out
}

func (s *Service) clientOf(deviceID string) *Client {
	rt := &s.sess.rt
	rt.mu.Lock()
	defer rt.mu.Unlock()
	return rt.conns[deviceID]
}

func (s *Service) deviceConnected(deviceID string) bool { return s.clientOf(deviceID) != nil }

func (s *Service) userOnline(userID string) bool {
	for _, c := range s.clients() {
		if c.userID == userID {
			return true
		}
	}
	return false
}

// sendTo queues a message to a device if it is connected.
func (s *Service) sendTo(deviceID, typ string, payload any) {
	if c := s.clientOf(deviceID); c != nil {
		c.sendMsg(typ, "", payload)
	}
}

func (s *Service) armOwnerGrace(sessionID, deviceID string) {
	rt := &s.sess.rt
	rt.mu.Lock()
	defer rt.mu.Unlock()
	if old, ok := rt.ownerTimers[sessionID]; ok {
		old.t.Stop()
	}
	if rt.closed {
		return
	}
	rt.timerGen++
	gen := rt.timerGen
	rt.ownerTimers[sessionID] = timerEntry{deviceID: deviceID, gen: gen,
		t: time.AfterFunc(s.sess.grace, func() { s.ownerGraceExpired(sessionID, deviceID, gen) })}
}

// Close stops the Session grace timers and waits for a callback that is already running. Call it before the
// database closes (server shutdown, in-process restart). The service is not usable for Sessions afterwards.
func (s *Service) Close() {
	s.stopBackup()
	s.stopPlay()
	s.sess.mu.Lock() // waits for a running grace callback (they hold it while they work)
	defer s.sess.mu.Unlock()
	rt := &s.sess.rt
	rt.mu.Lock()
	defer rt.mu.Unlock()
	rt.closed = true
	for id, e := range rt.ownerTimers {
		e.t.Stop()
		delete(rt.ownerTimers, id)
	}
	for id, e := range rt.viewerTimers {
		e.t.Stop()
		delete(rt.viewerTimers, id)
	}
}

func (s *Service) cancelSessionGrace(sessionID string) {
	rt := &s.sess.rt
	rt.mu.Lock()
	defer rt.mu.Unlock()
	if e, ok := rt.ownerTimers[sessionID]; ok {
		e.t.Stop()
		delete(rt.ownerTimers, sessionID)
	}
}

// ownerGraceExpired acts only if the timer of generation gen still owns the map entry: a timer stopped too late
// by a reconnect (callback already running) must not remove or pre-empt a newer grace period.
func (s *Service) ownerGraceExpired(sessionID, deviceID string, gen uint64) {
	ctx := context.Background()
	s.sess.mu.Lock()
	defer s.sess.mu.Unlock()
	rt := &s.sess.rt
	rt.mu.Lock()
	if e, ok := rt.ownerTimers[sessionID]; !ok || e.gen != gen {
		rt.mu.Unlock()
		return
	}
	delete(rt.ownerTimers, sessionID)
	rt.mu.Unlock()
	if s.deviceConnected(deviceID) {
		return
	}
	if d, err := s.loadSession(ctx, sessionID); err == nil && !d.ended {
		s.endLocked(ctx, d, EndOwnerDisconnected)
	}
}

// armViewerGrace covers a viewer that joined by REST but never opened (or lost, over a server restart) its WSS
// connection: without a connection after the grace period the viewer is removed like after a drop.
func (s *Service) armViewerGrace(viewerID, deviceID string) {
	rt := &s.sess.rt
	rt.mu.Lock()
	defer rt.mu.Unlock()
	if old, ok := rt.viewerTimers[viewerID]; ok {
		old.t.Stop()
	}
	if rt.closed {
		return
	}
	rt.timerGen++
	gen := rt.timerGen
	rt.viewerTimers[viewerID] = timerEntry{deviceID: deviceID, gen: gen,
		t: time.AfterFunc(s.sess.grace, func() { s.viewerGraceExpired(viewerID, deviceID, gen) })}
}

func (s *Service) cancelViewerGrace(viewerID string) {
	rt := &s.sess.rt
	rt.mu.Lock()
	defer rt.mu.Unlock()
	if e, ok := rt.viewerTimers[viewerID]; ok {
		e.t.Stop()
		delete(rt.viewerTimers, viewerID)
	}
}

func (s *Service) viewerGraceExpired(viewerID, deviceID string, gen uint64) {
	s.sess.mu.Lock()
	defer s.sess.mu.Unlock()
	rt := &s.sess.rt
	rt.mu.Lock()
	if e, ok := rt.viewerTimers[viewerID]; !ok || e.gen != gen {
		rt.mu.Unlock()
		return
	}
	delete(rt.viewerTimers, viewerID)
	rt.mu.Unlock()
	if s.deviceConnected(deviceID) {
		return
	}
	s.dropViewersOfDevice(context.Background(), deviceID, LeftDisconnected)
}

// ---- client ----

// Client is one WSS connection of a device.
type Client struct {
	svc      *Service
	userID   string
	deviceID string
	name     string // display name of the user at connect time (presence)

	out  chan []byte
	done chan struct{}
	once sync.Once
	code int
	why  string

	mu         sync.Mutex
	ready      bool // hello processed, registered
	registered bool
	state      string
	gameID     string
	reqHost    string // Host of the WSS upgrade (TURN URLs)
	remoteIP   netip.Addr

	bucket   float64   // rate limit tokens; guarded by mu
	bucketAt time.Time // last refill

	runGate bool     // run catch-up is being delivered: live run messages wait in runBuf; guarded by mu
	runBuf  [][]byte // guarded by mu
}

// AcquirePreHello reserves one of the few not-yet-hello connection slots of a device. It returns false when
// the device has too many; release (idempotent) frees the slot and must be called when hello completed or the
// connection ended.
func (s *Service) AcquirePreHello(deviceID string) (release func(), ok bool) {
	rt := &s.sess.rt
	rt.mu.Lock()
	defer rt.mu.Unlock()
	if rt.preHello[deviceID] >= maxPreHelloConns {
		return func() {}, false
	}
	rt.preHello[deviceID]++
	var once sync.Once
	return func() {
		once.Do(func() {
			rt.mu.Lock()
			defer rt.mu.Unlock()
			if rt.preHello[deviceID]--; rt.preHello[deviceID] <= 0 {
				delete(rt.preHello, deviceID)
			}
		})
	}, true
}

// NewClient creates the connection state for an authenticated device. It is not visible to others until hello.
func (s *Service) NewClient(p Principal) *Client {
	return &Client{svc: s, userID: p.User.ID, deviceID: p.Device.ID, name: p.User.DisplayName, out: make(chan []byte, outBuffer), done: make(chan struct{}), state: "online"}
}

// SetRequestHost records the Host header of the WSS upgrade (used for TURN URLs in hello_ack).
func (c *Client) SetRequestHost(h string) { c.reqHost = h }

// SetRemoteAddr records the peer IP of the WSS connection from r.RemoteAddr (port stripped).
func (c *Client) SetRemoteAddr(remoteAddr string) {
	host := remoteAddr
	if h, _, err := net.SplitHostPort(remoteAddr); err == nil {
		host = h
	}
	if a, err := netip.ParseAddr(host); err == nil {
		c.mu.Lock()
		c.remoteIP = a.Unmap()
		c.mu.Unlock()
	}
}

// ConnectedPlayerIP reports whether ip is the remote IP of a currently connected (hello-processed) app.
func (s *Service) ConnectedPlayerIP(ip netip.Addr) bool {
	ip = ip.Unmap()
	for _, c := range s.clients() {
		c.mu.Lock()
		ok := c.remoteIP.IsValid() && c.remoteIP == ip
		c.mu.Unlock()
		if ok {
			return true
		}
	}
	return false
}

// Out delivers encoded messages (text frames) to write; Done fires when the connection must be closed.
func (c *Client) Out() <-chan []byte    { return c.out }
func (c *Client) Done() <-chan struct{} { return c.done }
func (c *Client) Ready() bool           { c.mu.Lock(); defer c.mu.Unlock(); return c.ready }

// Fail asks the transport to close the connection (policy violation) after queued messages are written.
func (c *Client) Fail() { c.closeWith(CloseCodePolicy, "protocol violation") }

// CloseInfo is the WebSocket close code and reason to use after Done fired.
func (c *Client) CloseInfo() (int, string) { return c.code, c.why }

func (c *Client) closeWith(code int, reason string) {
	c.once.Do(func() { c.code, c.why = code, reason; close(c.done) })
}

type envelope struct {
	Type    string          `json:"type"`
	ID      string          `json:"id,omitempty"`
	Payload json.RawMessage `json:"payload"`
}

func encode(typ, id string, payload any) []byte {
	var raw json.RawMessage
	switch v := payload.(type) {
	case json.RawMessage:
		raw = v
	default:
		raw, _ = json.Marshal(payload)
	}
	b, _ := json.Marshal(envelope{Type: typ, ID: id, Payload: raw})
	return b
}

func (c *Client) sendRaw(b []byte) {
	select {
	case c.out <- b:
	default:
		c.closeWith(CloseCodePolicy, "slow consumer")
	}
}

// sendDroppable queues a message that may be lost (presence): it is dropped when the queue is more than half
// full instead of closing the receiver as slow consumer.
func (c *Client) sendDroppable(b []byte) {
	if len(c.out) >= outBuffer-presenceReserve {
		return
	}
	c.sendRaw(b)
}

// allow takes one token of the incoming message bucket.
func (c *Client) allow() bool {
	now := time.Now() // real time: independent of the injectable test clock
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.bucketAt.IsZero() {
		c.bucket = wsMsgBurst
	} else if d := now.Sub(c.bucketAt).Seconds(); d > 0 {
		c.bucket = min(wsMsgBurst, c.bucket+d*wsMsgRate)
	}
	c.bucketAt = now
	if c.bucket < 1 {
		return false
	}
	c.bucket--
	return true
}

func (c *Client) sendMsg(typ, id string, payload any) { c.sendRaw(encode(typ, id, payload)) }

func (c *Client) sendError(id string, code Code, msg string) {
	c.sendMsg("error", id, map[string]string{"code": string(code), "message": msg})
}

var errFatal = errors.New("hub: connection must close")

// Handle processes one incoming text frame. A non-nil error means the connection must be closed
// (the reason was already sent to the client as error message where possible).
func (c *Client) Handle(data []byte) error {
	if !c.allow() {
		c.closeWith(CloseCodePolicy, "rate limit exceeded")
		return errFatal
	}
	var env envelope
	if err := json.Unmarshal(data, &env); err != nil || env.Type == "" {
		c.sendError("", CodeBadRequest, "Invalid message")
		if !c.Ready() {
			return errFatal
		}
		return nil
	}
	if !c.Ready() {
		if env.Type != "hello" {
			c.sendError(env.ID, CodeBadRequest, "hello must be the first message")
			return errFatal
		}
		return c.hello(env)
	}
	switch env.Type {
	case "signal":
		c.signal(env)
	case "presence_update":
		c.presence(env)
	default:
		c.sendError(env.ID, CodeBadRequest, "Unknown message type")
	}
	return nil
}

func (c *Client) hello(env envelope) error {
	var h struct {
		ProtocolVersion int             `json:"protocol_version"`
		DeviceID        string          `json:"device_id"`
		Runs            json.RawMessage `json:"runs"`
	}
	if err := json.Unmarshal(env.Payload, &h); err != nil || h.ProtocolVersion < 1 {
		c.sendError(env.ID, CodeBadRequest, "hello needs protocol_version and device_id")
		return errFatal
	}
	if _, err := uuid.Parse(h.DeviceID); err != nil {
		c.sendError(env.ID, CodeBadRequest, "hello needs protocol_version and device_id")
		return errFatal
	}
	if h.DeviceID != c.deviceID {
		c.sendError(env.ID, CodeForbidden, "device_id does not match the access token")
		return errFatal
	}
	info := c.svc.Info()
	if h.ProtocolVersion < info.MinProtocolVersion {
		c.sendError(env.ID, "player_too_old", "App protocol version below the server's minimum")
		return errFatal
	}
	ice, turn := c.svc.iceFor(c.reqHost, c.deviceID)
	ack := map[string]any{"protocol_version": info.ProtocolVersion, "hub_version": info.HubVersion,
		"features": helloAckFeatures(len(turn) > 0), "ice_servers": ice}
	if len(turn) > 0 {
		ack["turn_servers"] = turnPayload(turn)
	}
	c.sendMsg("hello_ack", env.ID, ack)
	// The optional runs list (last seen event per run) is advisory: a malformed list is ignored, never fatal.
	var listed []RunHello
	if len(h.Runs) > 0 {
		if json.Unmarshal(h.Runs, &listed) != nil {
			listed = nil
		}
	}
	c.svc.startRunCatchUp(c, listed) // registers the client (attach) and queues the run catch-up behind hello_ack
	return nil
}

// attach registers the client (replacing an older connection of the same device), cancels grace timers of the
// device and sends the current presence and Session snapshot.
func (s *Service) attach(c *Client) {
	rt := &s.sess.rt
	rt.mu.Lock()
	old := rt.conns[c.deviceID]
	rt.conns[c.deviceID] = c
	for id, e := range rt.ownerTimers {
		if e.deviceID == c.deviceID {
			e.t.Stop()
			delete(rt.ownerTimers, id)
		}
	}
	for id, e := range rt.viewerTimers {
		if e.deviceID == c.deviceID {
			e.t.Stop()
			delete(rt.viewerTimers, id)
		}
	}
	rt.mu.Unlock()
	c.mu.Lock()
	c.ready, c.registered = true, true
	c.mu.Unlock()
	s.Publish(TopicClients) // the Clients page shows who is online
	if old != nil {
		s.playDeviceReplaced(old)
		old.closeWith(CloseCodePolicy, "replaced by a newer connection")
	}
	pb := s.newPresenceBuilder()
	for _, o := range s.clients() {
		if o != c {
			c.sendMsg("presence_update", "", pb.payload(o, c)) // snapshot: who is where
			o.sendMsg("presence_update", "", pb.payload(c, o))
		}
	}
	s.sess.mu.Lock()
	defer s.sess.mu.Unlock()
	ctx := context.Background()
	ids, err := s.activeSessionIDs(ctx)
	if err != nil {
		return
	}
	for _, id := range ids {
		if d, err := s.loadSession(ctx, id); err == nil && d.allows(c.userID, c.deviceID) {
			c.sendMsg("session_update", "", map[string]any{"session": d.render(c.userID, c.deviceID, s.userOnline)})
		}
	}
}

// presenceBuilder builds the per-receiver presence_update payloads of one broadcast. It caches the sender's
// active Session and the game lookups per receiving user. The caller may hold sess.mu (the lookups only read).
type presenceBuilder struct {
	s     *Service
	sess  map[string]*sessionData // sender device -> its active Session (nil = none)
	games map[string]*Game        // receiver user + "\x00" + game id -> game (nil = unknown or not visible)
}

func (s *Service) newPresenceBuilder() *presenceBuilder {
	return &presenceBuilder{s: s, sess: map[string]*sessionData{}, games: map[string]*Game{}}
}

func (b *presenceBuilder) session(deviceID string) *sessionData {
	if d, ok := b.sess[deviceID]; ok {
		return d
	}
	var d *sessionData
	ctx := context.Background()
	var id string
	if err := b.s.db.QueryRowContext(ctx, `SELECT id FROM sessions WHERE ended_at IS NULL AND owner_device_id = ?
		ORDER BY created_at DESC, id LIMIT 1`, deviceID).Scan(&id); err == nil {
		if sd, err := b.s.loadSession(ctx, id); err == nil && !sd.ended {
			d = sd
		}
	}
	b.sess[deviceID] = d
	return d
}

func (b *presenceBuilder) game(userID, gameID string) *Game {
	k := userID + "\x00" + gameID
	if g, ok := b.games[k]; ok {
		return g
	}
	var out *Game
	if g, err := b.s.GetGameFor(context.Background(), userID, gameID); err == nil {
		out = &g
	}
	b.games[k] = out
	return out
}

// payload is the presence_update of sender c as receiver r may see it: display_name, game_title and system when
// r may see the game (library visibility), session_id only when the Session ACL allows r.
func (b *presenceBuilder) payload(c, r *Client) map[string]string {
	c.mu.Lock()
	state, gameID := c.state, c.gameID
	c.mu.Unlock()
	p := map[string]string{"user_id": c.userID, "device_id": c.deviceID, "state": state}
	if c.name != "" {
		p["display_name"] = c.name
	}
	if state == "in_game" && gameID != "" {
		p["game_id"] = gameID
		if g := b.game(r.userID, gameID); g != nil {
			p["game_title"], p["system"] = g.Title, g.System
		}
	}
	if d := b.session(c.deviceID); d != nil && d.allows(r.userID, r.deviceID) {
		p["session_id"] = d.id
	}
	return p
}

// resendPresence re-sends the presence of the owner device of a Session to the selected receivers so that
// session_id appears or disappears. The caller may hold sess.mu.
func (s *Service) resendPresence(ownerDeviceID string, only func(*Client) bool) {
	c := s.clientOf(ownerDeviceID)
	if c == nil {
		return
	}
	pb := s.newPresenceBuilder()
	for _, o := range s.clients() {
		if o != c && only(o) {
			o.sendDroppable(encode("presence_update", "", pb.payload(c, o)))
		}
	}
}

func (c *Client) presence(env envelope) {
	var p struct {
		State  string `json:"state"`
		GameID string `json:"game_id"`
		RunID  string `json:"run_id"`
		// SessionID is accepted for forward compatibility and ignored: the server derives the Session of the
		// device from the session table.
		SessionID string `json:"session_id"`
	}
	if err := json.Unmarshal(env.Payload, &p); err != nil || (p.State != "online" && p.State != "in_game") {
		c.sendError(env.ID, CodeBadRequest, "state must be online or in_game")
		return
	}
	if p.GameID != "" {
		if _, err := uuid.Parse(p.GameID); err != nil || p.State != "in_game" {
			c.sendError(env.ID, CodeBadRequest, "game_id is only valid with state in_game and must be a UUID")
			return
		}
	}
	if p.RunID != "" {
		if _, err := uuid.Parse(p.RunID); err != nil || p.State != "in_game" {
			c.sendError(env.ID, CodeBadRequest, "run_id is only valid with state in_game and must be a UUID")
			return
		}
	}
	c.mu.Lock()
	c.state, c.gameID = p.State, p.GameID
	c.mu.Unlock()
	c.svc.playPresence(c, p.State, p.GameID, p.RunID) // run_id is for play time only, never broadcast
	pb := c.svc.newPresenceBuilder()
	for _, o := range c.svc.clients() {
		if o != c {
			o.sendDroppable(encode("presence_update", "", pb.payload(c, o)))
		}
	}
}

// signal relays WebRTC signaling only between the owner device and the authorized viewer's device.
func (c *Client) signal(env envelope) {
	var sp struct {
		SessionID string `json:"session_id"`
		ViewerID  string `json:"viewer_id"`
		Kind      string `json:"kind"`
	}
	if err := json.Unmarshal(env.Payload, &sp); err != nil || sp.SessionID == "" || sp.ViewerID == "" ||
		(sp.Kind != "offer" && sp.Kind != "answer" && sp.Kind != "candidate") {
		c.sendError(env.ID, CodeBadRequest, "signal needs session_id, viewer_id and kind offer|answer|candidate")
		return
	}
	s := c.svc
	s.sess.mu.Lock()
	defer s.sess.mu.Unlock()
	forbidden := func() {
		c.sendError(env.ID, CodeSessionForbidden, "Signaling is not allowed for this Session or viewer")
	}
	d, err := s.loadSession(context.Background(), sp.SessionID)
	if err != nil || d.ended {
		forbidden()
		return
	}
	v := d.viewerByID(sp.ViewerID)
	if v == nil || !d.allows(v.userID, v.deviceID) {
		forbidden()
		return
	}
	var target string
	switch c.deviceID {
	case d.ownerDeviceID:
		target = v.deviceID
	case v.deviceID:
		target = d.ownerDeviceID
	default:
		forbidden()
		return
	}
	peer := s.clientOf(target)
	if peer == nil {
		c.sendError(env.ID, CodeNotFound, "Peer is not connected")
		return
	}
	peer.sendRaw(encode("signal", env.ID, env.Payload)) // the payload (SDP, candidates) is never parsed
}

// Disconnect must be called by the transport when the connection ended. A viewer whose connection drops is
// removed; a Session whose owner connection drops ends after the grace period unless the owner reconnects.
func (c *Client) Disconnect() {
	c.closeWith(CloseCodePolicy, "closed")
	c.mu.Lock()
	reg := c.registered
	c.mu.Unlock()
	if !reg {
		return
	}
	s := c.svc
	s.playClose(c) // also when replaced by a newer connection: its own play ends with it
	rt := &s.sess.rt
	rt.mu.Lock()
	current := rt.conns[c.deviceID] == c
	if current {
		delete(rt.conns, c.deviceID)
	}
	rt.mu.Unlock()
	if !current { // replaced by a newer connection: the device stays online
		return
	}
	s.Publish(TopicClients)
	ctx := context.Background()
	s.sess.mu.Lock()
	defer s.sess.mu.Unlock()
	msg := encode("presence_update", "", map[string]string{"user_id": c.userID, "device_id": c.deviceID, "state": "offline", "display_name": c.name})
	for _, o := range s.clients() {
		o.sendRaw(msg)
	}
	s.dropViewersOfDevice(ctx, c.deviceID, LeftDisconnected)
	if ids, err := s.activeSessionIDs(ctx, `AND owner_device_id = ?`, c.deviceID); err == nil {
		for _, id := range ids {
			s.armOwnerGrace(id, c.deviceID)
		}
	}
}

// FeatureTogetherV1 is announced in hello_ack: presence_update carries display_name, game_title, system and the
// ACL-filtered session_id (0.23 online status; ready check, pause and chat follow under the same flag).
const FeatureTogetherV1 = "together_v1"

// helloAckFeatures is the short feature list of hello_ack: the WSS-relevant flags only. The authoritative list
// is the one of the handshake response (BaseFeatures plus the per-request flags); turn_v1 is added here
// exactly when turn_servers is sent.
func helloAckFeatures(turn bool) []string {
	f := []string{FeatureSavesV1, FeatureSavesV2, FeatureSessionsV1, FeatureICEUpdateV1, FeatureRunsV1, FeatureRunsV2, FeatureLocalROMsV1, FeatureTogetherV1}
	if turn {
		f = append(f, FeatureTURNV1)
	}
	return f
}
