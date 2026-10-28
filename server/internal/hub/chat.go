package hub

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"regexp"
	"slices"
	"strings"
	"sync"
	"time"
	"unicode/utf8"
)

// Chat (0.23 Together, ADR 0033 D4). Scopes "run" (stored in run_chat_messages, at most RunChatMax per run) and
// "session" (memory ring of sessionChatRing, dropped when the Session ends). Members are the same as for the ready
// check (membersOf). Locking: chat.mu is a leaf lock (ring, counters, rate buckets); chat.runMu serialises the
// run inserts and is never held while sending.

const (
	chatMaxRunes     = 500
	chatMaxClientID  = 100
	DefaultRunChat   = 10000
	sessionChatRing  = 200
	chatRate         = 2.0 // messages per second per user
	chatBurst        = 5.0
	DefaultChatPage  = 100
	MaxChatPage      = 500
	maxMentionTokens = 20
)

var errChatRate = &Error{Code: CodeRateLimited, Message: "Too many chat messages; slow down"}

// ChatMessage is one chat message.
type ChatMessage struct {
	Seq      int64     `json:"seq"`
	UserID   string    `json:"user_id"`
	Text     string    `json:"text"`
	SentAt   time.Time `json:"sent_at"`
	Mentions []string  `json:"mentions"`
	ClientID string    `json:"client_id"`
}

type sessionRing struct {
	next int64
	msgs []ChatMessage
}

type chatState struct {
	mu      sync.Mutex
	runMu   sync.Mutex
	buckets map[string]*tokenBucket // user ID
	rings   map[string]*sessionRing // session ID
	max     int
}

func (s *Service) runChatMax() int {
	if s.chat.max > 0 {
		return s.chat.max
	}
	return DefaultRunChat
}

// dropSessionChat forgets the ring of an ended Session.
func (s *Service) dropSessionChat(id string) {
	s.chat.mu.Lock()
	delete(s.chat.rings, id)
	s.chat.mu.Unlock()
}

func (s *Service) chatAllow(userID string) bool {
	c := &s.chat
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.buckets == nil {
		c.buckets = map[string]*tokenBucket{}
	}
	now := s.now()
	b := c.buckets[userID]
	if b == nil {
		b = &tokenBucket{tokens: chatBurst, at: now}
		c.buckets[userID] = b
	} else if d := now.Sub(b.at).Seconds(); d > 0 {
		b.tokens = min(chatBurst, b.tokens+d*chatRate)
		b.at = now
	}
	if b.tokens < 1 {
		return false
	}
	b.tokens--
	return true
}

var mentionRe = regexp.MustCompile(`@([\p{L}\p{N}_.\-]+)`)

// resolveMentions maps @name tokens (case-insensitive, display name or user name of a member) to user IDs, in
// order of appearance, without duplicates. Unknown names stay plain text.
func (s *Service) resolveMentions(ctx context.Context, members []string, text string) []string {
	out := []string{}
	toks := mentionRe.FindAllStringSubmatch(text, maxMentionTokens)
	if len(toks) == 0 {
		return out
	}
	names := map[string]string{} // lower-case name -> user ID
	for _, u := range members {
		var user, display string
		if s.db.QueryRowContext(ctx, `SELECT username, display_name FROM users WHERE id = ?`, u).Scan(&user, &display) != nil {
			continue
		}
		for _, n := range []string{user, display, strings.ReplaceAll(display, " ", "")} {
			if n = strings.ToLower(n); n != "" {
				if _, taken := names[n]; !taken {
					names[n] = u
				}
			}
		}
	}
	for _, m := range toks {
		if id, ok := names[strings.ToLower(m[1])]; ok && !slices.Contains(out, id) {
			out = append(out, id)
		}
	}
	return out
}

type chatSendMsg struct {
	Scope    string `json:"scope"`
	ID       string `json:"id"`
	ClientID string `json:"client_id"`
	Text     string `json:"text"`
}

func (c *Client) chatSend(env envelope) {
	var m chatSendMsg
	if json.Unmarshal(env.Payload, &m) != nil || (m.Scope != scopeRun && m.Scope != scopeSession) || m.ID == "" {
		c.sendError(env.ID, CodeBadRequest, "scope must be run or session and id is required")
		return
	}
	text := strings.TrimSpace(m.Text)
	if n := utf8.RuneCountInString(text); n < 1 || n > chatMaxRunes {
		c.sendError(env.ID, CodeBadRequest, "text must be 1 to 500 characters")
		return
	}
	if m.ClientID == "" || len(m.ClientID) > chatMaxClientID {
		c.sendError(env.ID, CodeBadRequest, "client_id is required (at most 100 characters)")
		return
	}
	s := c.svc
	ctx := context.Background()
	k := scopeKey{m.Scope, m.ID}
	members, ok := s.membersOf(ctx, k)
	if !ok || !slices.Contains(members, c.userID) {
		c.sendError(env.ID, CodeForbidden, "Not a member of this "+m.Scope)
		return
	}
	// A resend of a known client_id answers the sender without spending a token.
	if prev, found, err := s.findChat(ctx, k, c.userID, m.ClientID); err != nil {
		c.sendError(env.ID, CodeInternal, "Internal error")
		return
	} else if found {
		c.sendMsg("chat_message", env.ID, chatPayload(k, prev))
		return
	}
	if !s.chatAllow(c.userID) {
		c.sendError(env.ID, CodeRateLimited, errChatRate.Message)
		return
	}
	msg := ChatMessage{UserID: c.userID, Text: text, SentAt: s.now().UTC().Truncate(time.Second), ClientID: m.ClientID,
		Mentions: s.resolveMentions(ctx, members, text)}
	var err error
	if k.scope == scopeRun {
		msg, err = s.storeRunChat(ctx, k.id, msg)
	} else {
		msg = s.appendSessionChat(k.id, msg)
	}
	if err != nil {
		c.sendError(env.ID, CodeInternal, "Internal error")
		return
	}
	s.sendToUsers(members, "chat_message", "", chatPayload(k, msg))
}

func chatPayload(k scopeKey, m ChatMessage) map[string]any {
	return map[string]any{"scope": k.scope, "id": k.id, "seq": m.Seq, "user_id": m.UserID, "text": m.Text,
		"sent_at": m.SentAt.UTC().Format(time.RFC3339), "mentions": m.Mentions, "client_id": m.ClientID}
}

// findChat looks for an earlier message of the sender with this client_id (idempotent resend).
func (s *Service) findChat(ctx context.Context, k scopeKey, userID, clientID string) (ChatMessage, bool, error) {
	if k.scope == scopeSession {
		s.chat.mu.Lock()
		defer s.chat.mu.Unlock()
		if r := s.chat.rings[k.id]; r != nil {
			for _, m := range r.msgs {
				if m.ClientID == clientID && m.UserID == userID {
					return m, true, nil
				}
			}
		}
		return ChatMessage{}, false, nil
	}
	row := s.db.QueryRowContext(ctx, `SELECT `+chatCols+` FROM run_chat_messages WHERE run_id = ? AND client_id = ? AND user_id = ?`, k.id, clientID, userID)
	m, err := scanChat(row)
	if errors.Is(err, sql.ErrNoRows) {
		return ChatMessage{}, false, nil
	}
	return m, err == nil, err
}

func (s *Service) appendSessionChat(id string, m ChatMessage) ChatMessage {
	c := &s.chat
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.rings == nil {
		c.rings = map[string]*sessionRing{}
	}
	r := c.rings[id]
	if r == nil {
		r = &sessionRing{}
		c.rings[id] = r
	}
	r.next++
	m.Seq = r.next
	r.msgs = append(r.msgs, m)
	if len(r.msgs) > sessionChatRing {
		r.msgs = slices.Clone(r.msgs[len(r.msgs)-sessionChatRing:])
	}
	return m
}

// storeRunChat inserts the message with the next seq of the run and trims the run to RunChatMax.
func (s *Service) storeRunChat(ctx context.Context, runID string, m ChatMessage) (ChatMessage, error) {
	s.chat.runMu.Lock()
	defer s.chat.runMu.Unlock()
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return m, err
	}
	defer tx.Rollback()
	if err := tx.QueryRowContext(ctx, `SELECT COALESCE(MAX(seq), 0) + 1 FROM run_chat_messages WHERE run_id = ?`, runID).Scan(&m.Seq); err != nil {
		return m, err
	}
	mentions, _ := json.Marshal(m.Mentions)
	if _, err := tx.ExecContext(ctx, `INSERT INTO run_chat_messages(run_id, seq, user_id, text, mentions, client_id, sent_at) VALUES (?, ?, ?, ?, ?, ?, ?)`,
		runID, m.Seq, m.UserID, m.Text, string(mentions), m.ClientID, m.SentAt.Unix()); err != nil {
		// The same client_id from another user, or a racing duplicate: never leak the other message.
		return m, err
	}
	if _, err := tx.ExecContext(ctx, `DELETE FROM run_chat_messages WHERE run_id = ? AND seq <= ?`, runID, m.Seq-int64(s.runChatMax())); err != nil {
		return m, err
	}
	return m, tx.Commit()
}

const chatCols = `seq, user_id, text, mentions, client_id, sent_at`

func scanChat(sc interface{ Scan(...any) error }) (ChatMessage, error) {
	var m ChatMessage
	var mentions string
	var sent int64
	if err := sc.Scan(&m.Seq, &m.UserID, &m.Text, &mentions, &m.ClientID, &sent); err != nil {
		return ChatMessage{}, err
	}
	m.SentAt = unixTime(sent)
	if json.Unmarshal([]byte(mentions), &m.Mentions) != nil || m.Mentions == nil {
		m.Mentions = []string{}
	}
	return m, nil
}

func loadRunChat(ctx context.Context, q dbq, runID string, after int64, limit int) ([]ChatMessage, error) {
	rows, err := q.QueryContext(ctx, `SELECT `+chatCols+` FROM run_chat_messages WHERE run_id = ? AND seq > ? ORDER BY seq LIMIT ?`, runID, after, limit)
	if err != nil {
		return nil, internal(err)
	}
	defer rows.Close()
	out := []ChatMessage{}
	for rows.Next() {
		m, err := scanChat(rows)
		if err != nil {
			return nil, internal(err)
		}
		out = append(out, m)
	}
	return out, rows.Err()
}

// ListRunChat returns the messages with seq > after (joined participants only). next is set when more follow.
func (s *Service) ListRunChat(ctx context.Context, userID, runID string, after int64, limit int) (msgs []ChatMessage, next *int64, err error) {
	if after < 0 || limit < 1 || limit > MaxChatPage {
		return nil, nil, badRequest("after_seq must be >= 0 and limit between 1 and %d", MaxChatPage)
	}
	if err := joinedParticipant(ctx, s.db, runID, userID); err != nil {
		return nil, nil, err
	}
	msgs, err = loadRunChat(ctx, s.db, runID, after, limit+1)
	if err != nil {
		return nil, nil, err
	}
	if len(msgs) > limit {
		msgs = msgs[:limit]
		n := msgs[limit-1].Seq
		next = &n
	}
	return msgs, next, nil
}

// ListSessionChat returns the ring of an active Session (owner and active viewers only).
func (s *Service) ListSessionChat(ctx context.Context, p Principal, id string) ([]ChatMessage, error) {
	d, err := s.loadActive(ctx, id)
	if err != nil {
		return nil, err
	}
	if !slices.Contains(sessionMembers(d), p.User.ID) {
		return nil, ErrSessionForbidden
	}
	s.chat.mu.Lock()
	defer s.chat.mu.Unlock()
	out := []ChatMessage{}
	if r := s.chat.rings[id]; r != nil {
		out = append(out, r.msgs...)
	}
	return out, nil
}
