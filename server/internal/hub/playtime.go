package hub

import (
	"context"
	"database/sql"
	"log/slog"
	"strconv"
	"sync"
	"time"

	"github.com/google/uuid"
)

// Play time (0.19 package 4). The server counts it from the app's presence: a play opens when a device reports
// presence_update {state: in_game, game_id} and closes when state, game or run changes, the WSS connection ends or
// is replaced, and on shutdown. Sessions exist only for shared games, so presence is the game start and stop event.

// minPlaySeconds is the shortest play that is kept; shorter ones are deleted when they close.
const minPlaySeconds = 10

// DefaultPlayTick is how often the last_seen_at of open plays is refreshed.
const DefaultPlayTick = 60 * time.Second

type playRec struct {
	id        string
	userID    string
	gameID    string
	runID     string // "" = no run
	startedAt int64
	client    *Client
}

type playState struct {
	mu       sync.Mutex
	open     map[string]*playRec // device ID -> open play
	interval time.Duration
	closed   bool
	stop     chan struct{}
	wg       sync.WaitGroup
}

func (s *Service) initPlay(o Options) {
	s.play.open = map[string]*playRec{}
	s.play.interval = o.PlayTickInterval
	if s.play.interval <= 0 {
		s.play.interval = DefaultPlayTick
	}
}

// startPlay recovers plays left open by a crash and starts the refresh ticker. Only the serving process calls it:
// a CLI sub-command opening the data directory next to the running service must not touch its open plays.
func (s *Service) startPlay(ctx context.Context) {
	if _, err := s.db.ExecContext(ctx, `UPDATE plays SET ended_at = last_seen_at WHERE ended_at IS NULL`); err != nil {
		slog.Warn("play time: recover open plays failed", "err", err)
	}
	if _, err := s.db.ExecContext(ctx, `DELETE FROM plays WHERE ended_at IS NOT NULL AND ended_at - started_at < ?`, minPlaySeconds); err != nil {
		slog.Warn("play time: drop short plays failed", "err", err)
	}
	s.play.stop = make(chan struct{})
	s.play.wg.Add(1)
	go func() {
		defer s.play.wg.Done()
		t := time.NewTicker(s.play.interval)
		defer t.Stop()
		for {
			select {
			case <-s.play.stop:
				return
			case <-t.C:
				s.PlayTick(context.Background())
			}
		}
	}()
}

// stopPlay stops the ticker and closes every open play (service shutdown). Presence after it is ignored.
func (s *Service) stopPlay() {
	s.play.mu.Lock()
	if s.play.closed {
		s.play.mu.Unlock()
		return
	}
	s.play.closed = true
	stop := s.play.stop
	for _, r := range s.play.open {
		s.closePlayLocked(context.Background(), r)
	}
	s.play.mu.Unlock()
	if stop != nil {
		close(stop)
		s.play.wg.Wait()
	}
}

// PlayTick refreshes last_seen_at of the open plays (the ticker calls it every minute).
func (s *Service) PlayTick(ctx context.Context) {
	s.play.mu.Lock()
	defer s.play.mu.Unlock()
	if s.play.closed {
		return
	}
	now := s.now().Unix()
	for _, r := range s.play.open {
		if _, err := s.db.ExecContext(ctx, `UPDATE plays SET last_seen_at = ? WHERE id = ? AND ended_at IS NULL`, max(now, r.startedAt), r.id); err != nil {
			slog.Warn("play time: refresh failed", "err", err)
		}
	}
}

// closePlayLocked ends a play now: plays under minPlaySeconds are deleted. play.mu is held.
func (s *Service) closePlayLocked(ctx context.Context, r *playRec) {
	if s.play.open[r.client.deviceID] == r {
		delete(s.play.open, r.client.deviceID)
	}
	end := max(s.now().Unix(), r.startedAt)
	var err error
	if end-r.startedAt < minPlaySeconds {
		_, err = s.db.ExecContext(ctx, `DELETE FROM plays WHERE id = ?`, r.id)
	} else {
		_, err = s.db.ExecContext(ctx, `UPDATE plays SET ended_at = ?, last_seen_at = ? WHERE id = ?`, end, end, r.id)
	}
	if err != nil {
		slog.Warn("play time: close failed", "err", err)
	}
}

// playClose closes the open play of a device when it belongs to connection c (a connection that was replaced
// by a newer one must not close the newer one's play).
func (s *Service) playClose(c *Client) {
	s.play.mu.Lock()
	defer s.play.mu.Unlock()
	if r := s.play.open[c.deviceID]; r != nil && r.client == c {
		s.closePlayLocked(context.Background(), r)
	}
}

// playDeviceReplaced closes the play of a device whose connection is replaced by a newer one.
func (s *Service) playDeviceReplaced(old *Client) { s.playClose(old) }

// playPresence applies a presence change of client c. runID is the run the app reported (may be empty); it is kept
// only when the user joined that run with exactly this game.
func (s *Service) playPresence(c *Client, state, gameID, runID string) {
	ctx := context.Background()
	if started := s.playPresenceLocked(ctx, c, state, gameID, runID); started != "" {
		// A run that is still starting becomes active (#439). This takes runs.mu, so it runs after play.mu is released:
		// startRunCatchUp holds runs.mu while attach waits for play.mu (playDeviceReplaced).
		s.autoStartRun(ctx, started, c.userID, AutoStartPlay)
	}
}

// playPresenceLocked does the work of playPresence under play.mu and returns the run id of a newly opened run play.
func (s *Service) playPresenceLocked(ctx context.Context, c *Client, state, gameID, runID string) string {
	s.play.mu.Lock()
	defer s.play.mu.Unlock()
	if s.play.closed {
		return ""
	}
	want := state == "in_game" && gameID != ""
	if want && runID != "" && !s.runMember(ctx, c.userID, runID, gameID) {
		runID = ""
	}
	if !want {
		runID = ""
	}
	cur := s.play.open[c.deviceID]
	if cur != nil && want && cur.client == c && cur.gameID == gameID && cur.runID == runID {
		return "" // nothing changed
	}
	if cur != nil {
		s.closePlayLocked(ctx, cur)
	}
	if !want {
		return ""
	}
	now := s.now().Unix()
	rec := &playRec{id: uuid.NewString(), userID: c.userID, gameID: gameID, runID: runID, startedAt: now, client: c}
	var run any
	if runID != "" {
		run = runID
	}
	if _, err := s.db.ExecContext(ctx, `INSERT INTO plays(id, user_id, device_id, game_id, run_id, started_at, last_seen_at) VALUES (?, ?, ?, ?, ?, ?, ?)`,
		rec.id, c.userID, c.deviceID, gameID, run, now, now); err != nil {
		slog.Warn("play time: open failed", "err", err)
		return ""
	}
	s.play.open[c.deviceID] = rec
	return runID
}

// runMember reports whether the user joined the run with this game.
func (s *Service) runMember(ctx context.Context, userID, runID, gameID string) bool {
	var one int
	err := s.db.QueryRowContext(ctx, `SELECT 1 FROM run_participants WHERE run_id = ? AND user_id = ? AND status = 'joined' AND game_id = ?`,
		runID, userID, gameID).Scan(&one)
	return err == nil
}

// ---- sums ----

// PlayStat is the play time of one user in one scope (a game, a run or everything).
type PlayStat struct {
	Seconds      int64
	Sessions     int
	LastPlayedAt *time.Time
}

// playDuration is the length of a play; open plays count up to ?1 (now). Only plays of at least minPlaySeconds count.
const (
	playDuration = `MAX(0, COALESCE(ended_at, ?1) - started_at)`
	playCounted  = playDuration + ` >= 10` // = minPlaySeconds
)

// playStats runs a grouped sum over plays: key is the grouping column (game_id, user_id or run_id), where adds
// conditions on the same table (its placeholders start at ?2).
func (s *Service) playStats(ctx context.Context, key, where string, args ...any) (map[string]PlayStat, error) {
	q := `SELECT ` + key + `, COALESCE(SUM(` + playDuration + `), 0), COUNT(*), MAX(COALESCE(ended_at, ?1))
		FROM plays WHERE ` + playCounted + where + ` GROUP BY ` + key
	rows, err := s.db.QueryContext(ctx, q, append([]any{s.now().Unix()}, args...)...)
	if err != nil {
		return nil, internal(err)
	}
	defer rows.Close()
	out := map[string]PlayStat{}
	for rows.Next() {
		var k sql.NullString
		var st PlayStat
		var last int64
		if err := rows.Scan(&k, &st.Seconds, &st.Sessions, &last); err != nil {
			return nil, internal(err)
		}
		t := unixTime(last)
		st.LastPlayedAt = &t
		out[k.String] = st
	}
	return out, rows.Err()
}

// UserPlayStats returns the totals of one user over all games.
func (s *Service) UserPlayStats(ctx context.Context, userID string) (PlayStat, error) {
	m, err := s.playStats(ctx, `user_id`, ` AND user_id = ?2`, userID)
	return m[userID], err
}

// AllUserPlayStats returns the totals of every user that has played (key: user ID).
func (s *Service) AllUserPlayStats(ctx context.Context) (map[string]PlayStat, error) {
	return s.playStats(ctx, `user_id`, ``)
}

// runPlayStats returns the play time per run and user for the given runs: run ID -> user ID -> stat.
func (s *Service) runPlayStats(ctx context.Context, runIDs []string) (map[string]map[string]PlayStat, error) {
	out := map[string]map[string]PlayStat{}
	if len(runIDs) == 0 {
		return out, nil
	}
	// The IN list follows ?1 (now): its placeholders are numbered from ?2.
	args := make([]any, 0, len(runIDs)+1)
	args = append(args, s.now().Unix())
	inN := ""
	for i, id := range runIDs {
		if i > 0 {
			inN += ","
		}
		inN += "?" + strconv.Itoa(i+2)
		args = append(args, id)
	}
	rows, err := s.db.QueryContext(ctx, `SELECT run_id, user_id, COALESCE(SUM(`+playDuration+`), 0), COUNT(*), MAX(COALESCE(ended_at, ?1))
		FROM plays WHERE `+playCounted+` AND run_id IN (`+inN+`) GROUP BY run_id, user_id`, args...)
	if err != nil {
		return nil, internal(err)
	}
	defer rows.Close()
	for rows.Next() {
		var rid, uid string
		var st PlayStat
		var last int64
		if err := rows.Scan(&rid, &uid, &st.Seconds, &st.Sessions, &last); err != nil {
			return nil, internal(err)
		}
		t := unixTime(last)
		st.LastPlayedAt = &t
		if out[rid] == nil {
			out[rid] = map[string]PlayStat{}
		}
		out[rid][uid] = st
	}
	return out, rows.Err()
}

// fillRunPlayTimes sets play_seconds and last_played_at on the participants of the runs and play_seconds on the
// runs (REST only: pushes over WSS do not carry them). One grouped query for all runs.
func (s *Service) fillRunPlayTimes(ctx context.Context, runs []Run) error {
	ids := make([]string, len(runs))
	for i := range runs {
		ids[i] = runs[i].ID
	}
	stats, err := s.runPlayStats(ctx, ids)
	if err != nil {
		return err
	}
	for i := range runs {
		var total int64
		ps := make([]RunParticipant, len(runs[i].Participants))
		copy(ps, runs[i].Participants)
		for j := range ps {
			st := stats[runs[i].ID][ps[j].UserID]
			sec := st.Seconds
			ps[j].PlaySeconds, ps[j].LastPlayedAt = &sec, st.LastPlayedAt
		}
		for _, st := range stats[runs[i].ID] {
			total += st.Seconds
		}
		runs[i].Participants, runs[i].PlaySeconds = ps, &total
	}
	return nil
}
