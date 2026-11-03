package hub_test

import (
	"database/sql"
	"encoding/json"
	"fmt"
	"strings"
	"testing"
	"time"

	"github.com/google/uuid"

	"github.com/phabioo/uzume/server/internal/hub"
	"github.com/phabioo/uzume/server/internal/hub/hubtest"
)

// Play time from presence (0.19 package 4).

type playEnv struct {
	t      *testing.T
	svc    *hub.Service
	clk    *hubtest.Clock
	db     *sql.DB
	dir    string
	admin  hub.User
	g1, g2 hub.Game
}

func newPlayEnv(t *testing.T, mod func(*hub.Options)) *playEnv {
	t.Helper()
	dir := t.TempDir()
	svc, clk, db := hubtest.OpenIn(t, dir, mod)
	admin, err := svc.CreateAdmin(ctx, "admin", "secret-12345")
	if err != nil {
		t.Fatal(err)
	}
	g1, err := svc.AddROM(ctx, strings.NewReader("homebrew-rom-one"), "one.nds", "One", "", admin.ID)
	if err != nil {
		t.Fatal(err)
	}
	g2, err := svc.AddROM(ctx, strings.NewReader("homebrew-rom-two"), "two.nds", "Two", "", admin.ID)
	if err != nil {
		t.Fatal(err)
	}
	return &playEnv{t: t, svc: svc, clk: clk, db: db, dir: dir, admin: admin, g1: g1, g2: g2}
}

// client connects a new device of the user (hello done).
func (e *playEnv) client(userID string) *hub.Client {
	e.t.Helper()
	dev := pairDevice(e.t, e.svc, userID, "PC")
	return e.clientOf(userID, dev)
}

func (e *playEnv) clientOf(userID, dev string) *hub.Client {
	e.t.Helper()
	d, err := e.svc.GetDevice(ctx, dev)
	if err != nil {
		e.t.Fatal(err)
	}
	u, err := e.svc.GetUser(ctx, userID)
	if err != nil {
		e.t.Fatal(err)
	}
	cl := e.svc.NewClient(hub.Principal{Device: d, User: u})
	if err := cl.Handle([]byte(fmt.Sprintf(`{"type":"hello","payload":{"protocol_version":1,"device_id":%q}}`, dev))); err != nil {
		e.t.Fatal(err)
	}
	return cl
}

func (e *playEnv) present(cl *hub.Client, state, game, run string) {
	e.t.Helper()
	p := fmt.Sprintf(`{"state":%q`, state)
	if game != "" {
		p += fmt.Sprintf(`,"game_id":%q`, game)
	}
	if run != "" {
		p += fmt.Sprintf(`,"run_id":%q`, run)
	}
	if err := cl.Handle([]byte(`{"type":"presence_update","payload":` + p + `}}`)); err != nil {
		e.t.Fatal(err)
	}
}

type playRow struct {
	game, run     string
	started, seen int64
	ended         sql.NullInt64
}

func (e *playEnv) plays() []playRow {
	e.t.Helper()
	rows, err := e.db.Query(`SELECT game_id, COALESCE(run_id, ''), started_at, last_seen_at, ended_at FROM plays ORDER BY started_at, rowid`)
	if err != nil {
		e.t.Fatal(err)
	}
	defer rows.Close()
	var out []playRow
	for rows.Next() {
		var r playRow
		if err := rows.Scan(&r.game, &r.run, &r.started, &r.seen, &r.ended); err != nil {
			e.t.Fatal(err)
		}
		out = append(out, r)
	}
	return out
}

func (e *playEnv) personal(userID, gameID string) hub.GamePersonal {
	e.t.Helper()
	m, err := e.svc.PersonalGames(ctx, userID, gameID)
	if err != nil {
		e.t.Fatal(err)
	}
	return m[gameID]
}

func TestPlayOpensAndClosesOnPresenceChange(t *testing.T) {
	e := newPlayEnv(t, nil)
	cl := e.client(e.admin.ID)
	e.present(cl, "in_game", e.g1.ID, "")
	if ps := e.plays(); len(ps) != 1 || ps[0].game != e.g1.ID || ps[0].ended.Valid || ps[0].run != "" {
		t.Fatalf("after start: %+v", ps)
	}
	e.clk.Advance(30 * time.Second)
	// An open play counts up to now.
	if p := e.personal(e.admin.ID, e.g1.ID); p.Seconds != 30 || p.Sessions != 1 || p.LastPlayedAt == nil {
		t.Fatalf("open: %+v", p)
	}
	e.present(cl, "in_game", e.g1.ID, "") // same state again: no new play
	if ps := e.plays(); len(ps) != 1 {
		t.Fatalf("repeat opened a play: %+v", ps)
	}
	e.clk.Advance(30 * time.Second)
	e.present(cl, "online", "", "")
	ps := e.plays()
	if len(ps) != 1 || !ps[0].ended.Valid || ps[0].ended.Int64-ps[0].started != 60 {
		t.Fatalf("after stop: %+v", ps)
	}
	e.clk.Advance(time.Hour) // closed plays do not grow
	if p := e.personal(e.admin.ID, e.g1.ID); p.Seconds != 60 || p.Sessions != 1 {
		t.Fatalf("closed: %+v", p)
	}
}

func TestPlayGameSwitchClosesAndOpens(t *testing.T) {
	e := newPlayEnv(t, nil)
	dev := pairDevice(t, e.svc, e.admin.ID, "PC")
	cl := e.clientOf(e.admin.ID, dev)
	e.present(cl, "in_game", e.g1.ID, "")
	e.clk.Advance(20 * time.Second)
	e.present(cl, "in_game", e.g2.ID, "")
	e.clk.Advance(40 * time.Second)
	e.present(cl, "online", "", "")
	if p := e.personal(e.admin.ID, e.g1.ID); p.Seconds != 20 || p.Sessions != 1 {
		t.Fatalf("g1: %+v", p)
	}
	if p := e.personal(e.admin.ID, e.g2.ID); p.Seconds != 40 || p.Sessions != 1 {
		t.Fatalf("g2: %+v", p)
	}
	acc, err := e.svc.GetAccount(ctx, e.admin.ID, dev)
	if err != nil || acc.Seconds != 60 || acc.Sessions != 2 || acc.LastPlayedAt == nil {
		t.Fatalf("account: %+v %v", acc, err)
	}
}

func TestPlayUnderTenSecondsIsDropped(t *testing.T) {
	e := newPlayEnv(t, nil)
	cl := e.client(e.admin.ID)
	e.present(cl, "in_game", e.g1.ID, "")
	e.clk.Advance(9 * time.Second)
	if p := e.personal(e.admin.ID, e.g1.ID); p.Seconds != 0 || p.Sessions != 0 {
		t.Fatalf("open short play counted: %+v", p)
	}
	e.present(cl, "online", "", "")
	if ps := e.plays(); len(ps) != 0 {
		t.Fatalf("short play kept: %+v", ps)
	}
	e.present(cl, "in_game", e.g1.ID, "")
	e.clk.Advance(10 * time.Second)
	e.present(cl, "online", "", "")
	if p := e.personal(e.admin.ID, e.g1.ID); p.Seconds != 10 || p.Sessions != 1 {
		t.Fatalf("10 s play: %+v", p)
	}
}

func TestPlayEndsWhenConnectionDropsOrIsReplaced(t *testing.T) {
	e := newPlayEnv(t, nil)
	dev := pairDevice(t, e.svc, e.admin.ID, "PC")
	cl := e.clientOf(e.admin.ID, dev)
	e.present(cl, "in_game", e.g1.ID, "")
	e.clk.Advance(25 * time.Second)
	cl.Disconnect()
	if ps := e.plays(); len(ps) != 1 || !ps[0].ended.Valid || ps[0].ended.Int64-ps[0].started != 25 {
		t.Fatalf("after disconnect: %+v", ps)
	}

	// A newer connection of the same device replaces the old one: the old play ends, the new connection starts online.
	old := e.clientOf(e.admin.ID, dev)
	e.present(old, "in_game", e.g2.ID, "")
	e.clk.Advance(15 * time.Second)
	fresh := e.clientOf(e.admin.ID, dev)
	if ps := e.plays(); len(ps) != 2 || !ps[1].ended.Valid || ps[1].ended.Int64-ps[1].started != 15 {
		t.Fatalf("after replace: %+v", ps)
	}
	old.Disconnect() // the replaced connection going away must not touch the new one
	e.present(fresh, "in_game", e.g2.ID, "")
	e.clk.Advance(12 * time.Second)
	old.Disconnect()
	if ps := e.plays(); len(ps) != 3 || ps[2].ended.Valid {
		t.Fatalf("new connection's play: %+v", ps)
	}
}

func TestPlayRunIDKeptOnlyForJoinedMemberWithThatGame(t *testing.T) {
	e := newPlayEnv(t, nil)
	e.svc.SetPluginCatalog(onePlugin{"net.example.board", "1.0.0"})
	bob, err := e.svc.CreateUser(ctx, "bob", "Bob")
	if err != nil {
		t.Fatal(err)
	}
	carol, err := e.svc.CreateUser(ctx, "carol", "Carol")
	if err != nil {
		t.Fatal(err)
	}
	run, err := e.svc.CreateRun(ctx, hub.CreateRunInput{UserID: e.admin.ID, Name: "Run", GameID: e.g1.ID, InviteUserIDs: []string{bob.ID},
		Plugins: []hub.RunPluginRef{{PluginID: "net.example.board", Version: "1.0.0"}}})
	if err != nil {
		t.Fatal(err)
	}
	adminCl, bobCl, carolCl := e.client(e.admin.ID), e.client(bob.ID), e.client(carol.ID)

	e.present(adminCl, "in_game", e.g1.ID, run.ID) // member, same game: kept
	e.present(bobCl, "in_game", e.g1.ID, run.ID)   // only invited, not joined: dropped
	e.present(carolCl, "in_game", e.g1.ID, run.ID) // not a member: dropped
	e.clk.Advance(20 * time.Second)
	e.present(adminCl, "in_game", e.g2.ID, run.ID) // member, but another game than the run's: dropped
	e.clk.Advance(20 * time.Second)
	e.present(adminCl, "online", "", "")
	e.present(bobCl, "online", "", "")
	e.present(carolCl, "online", "", "")

	byGame := map[string]string{}
	for _, p := range e.plays() {
		byGame[p.game+"/"+p.run] = "x"
	}
	if len(e.plays()) != 4 || byGame[e.g1.ID+"/"+run.ID] == "" || byGame[e.g2.ID+"/"] == "" {
		t.Fatalf("plays: %+v", e.plays())
	}
	if n := countRows(t, e.db, `SELECT COUNT(*) FROM plays WHERE run_id IS NOT NULL`); n != 1 {
		t.Fatalf("%d plays carry the run", n)
	}

	got, err := e.svc.GetRun(ctx, e.admin.ID, run.ID)
	if err != nil {
		t.Fatal(err)
	}
	if got.PlaySeconds == nil || *got.PlaySeconds != 20 {
		t.Fatalf("run total: %v", got.PlaySeconds)
	}
	for _, p := range got.Participants {
		if p.PlaySeconds == nil {
			t.Fatalf("participant %s without play_seconds", p.UserID)
		}
		switch p.UserID {
		case e.admin.ID:
			if *p.PlaySeconds != 20 || p.LastPlayedAt == nil {
				t.Fatalf("admin: %d %v", *p.PlaySeconds, p.LastPlayedAt)
			}
		case bob.ID:
			if *p.PlaySeconds != 0 || p.LastPlayedAt != nil {
				t.Fatalf("bob: %d %v", *p.PlaySeconds, p.LastPlayedAt)
			}
		}
	}
	rs, err := e.svc.ListRuns(ctx, e.admin.ID)
	if err != nil || len(rs) != 1 || rs[0].PlaySeconds == nil || *rs[0].PlaySeconds != 20 {
		t.Fatalf("list: %+v %v", rs, err)
	}
	// The run keeps counting for admin's total in the game list as well.
	if p := e.personal(e.admin.ID, e.g1.ID); p.Seconds != 20 {
		t.Fatalf("game total: %+v", p)
	}
}

func TestPlayRunIDValidation(t *testing.T) {
	e := newPlayEnv(t, nil)
	cl := e.client(e.admin.ID)
	for _, p := range []string{
		`{"state":"in_game","game_id":"` + e.g1.ID + `","run_id":"nope"}`,
		`{"state":"online","run_id":"` + e.g1.ID + `"}`,
	} {
		if err := cl.Handle([]byte(`{"type":"presence_update","payload":` + p + `}`)); err != nil {
			t.Fatal(err)
		}
	}
	if n := len(drain(cl)); n < 2 {
		t.Fatalf("expected error answers, got %d messages", n)
	}
	if ps := e.plays(); len(ps) != 0 {
		t.Fatalf("plays: %+v", ps)
	}
}

func drain(c *hub.Client) []string {
	var out []string
	for {
		select {
		case b := <-c.Out():
			out = append(out, string(b))
		default:
			return out
		}
	}
}

func countRows(t *testing.T, db *sql.DB, q string, args ...any) int {
	t.Helper()
	var n int
	if err := db.QueryRow(q, args...).Scan(&n); err != nil {
		t.Fatal(err)
	}
	return n
}

func TestPlayTickRefreshesLastSeenAndStartupClosesDanglingPlays(t *testing.T) {
	dir := t.TempDir()
	svc, clk, db := hubtest.OpenIn(t, dir, nil)
	admin, _ := svc.CreateAdmin(ctx, "admin", "secret-12345")
	g, _ := svc.AddROM(ctx, strings.NewReader("homebrew-rom-one"), "one.nds", "One", "", admin.ID)
	e := &playEnv{t: t, svc: svc, clk: clk, db: db, admin: admin, g1: g}
	cl := e.client(admin.ID)
	e.present(cl, "in_game", g.ID, "")
	start := clk.Now().Unix()
	clk.Advance(75 * time.Second)
	svc.PlayTick(ctx)
	if ps := e.plays(); len(ps) != 1 || ps[0].seen != start+75 || ps[0].ended.Valid {
		t.Fatalf("after tick: %+v", ps)
	}
	// A crash: the play stays open in the database; the next start closes it where it was last seen.
	clk.Advance(10 * time.Minute)
	_, _, db2 := hubtest.OpenIn(t, dir, nil)
	rows := db2.QueryRow(`SELECT ended_at, last_seen_at FROM plays`)
	var ended, seen int64
	if err := rows.Scan(&ended, &seen); err != nil || ended != start+75 || seen != start+75 {
		t.Fatalf("recovered: ended %d seen %d (%v), want %d", ended, seen, err, start+75)
	}

	// A dangling play shorter than 10 s is dropped by the recovery.
	if _, err := db2.Exec(`INSERT INTO plays(id, user_id, device_id, game_id, started_at, last_seen_at) VALUES ('x', ?, 'd', ?, 100, 105)`, admin.ID, g.ID); err != nil {
		t.Fatal(err)
	}
	hubtest.OpenIn(t, dir, nil)
	if n := countRows(t, db2, `SELECT COUNT(*) FROM plays WHERE id = 'x'`); n != 0 {
		t.Fatal("short dangling play kept")
	}
}

func TestPlayTickerRuns(t *testing.T) {
	e := newPlayEnv(t, func(o *hub.Options) { o.PlayTickInterval = 5 * time.Millisecond })
	cl := e.client(e.admin.ID)
	e.present(cl, "in_game", e.g1.ID, "")
	start := e.clk.Now().Unix()
	e.clk.Advance(45 * time.Second)
	deadline := time.Now().Add(5 * time.Second)
	for {
		if ps := e.plays(); len(ps) == 1 && ps[0].seen == start+45 {
			break
		}
		if time.Now().After(deadline) {
			t.Fatalf("ticker did not refresh last_seen_at: %+v", e.plays())
		}
		time.Sleep(5 * time.Millisecond)
	}
}

func TestPlayClosedOnServiceClose(t *testing.T) {
	dir := t.TempDir()
	svc, clk, db := hubtest.OpenIn(t, dir, nil)
	admin, _ := svc.CreateAdmin(ctx, "admin", "secret-12345")
	g, _ := svc.AddROM(ctx, strings.NewReader("homebrew-rom-one"), "one.nds", "One", "", admin.ID)
	e := &playEnv{t: t, svc: svc, clk: clk, db: db, admin: admin, g1: g}
	cl := e.client(admin.ID)
	e.present(cl, "in_game", g.ID, "")
	clk.Advance(33 * time.Second)
	svc.Close()
	if ps := e.plays(); len(ps) != 1 || !ps[0].ended.Valid || ps[0].ended.Int64-ps[0].started != 33 {
		t.Fatalf("after close: %+v", ps)
	}
	e.present(cl, "in_game", g.ID, "") // ignored after close
	if ps := e.plays(); len(ps) != 1 {
		t.Fatalf("presence after close: %+v", ps)
	}
}

func TestFavoritesAndCollectionsService(t *testing.T) {
	e := newPlayEnv(t, nil)
	bob, _ := e.svc.CreateUser(ctx, "bob", "Bob")

	// Favorites are idempotent and per user; unknown games are not found.
	for i := 0; i < 2; i++ {
		if err := e.svc.SetFavorite(ctx, e.admin.ID, e.g1.ID, true); err != nil {
			t.Fatal(err)
		}
	}
	if !e.personal(e.admin.ID, e.g1.ID).Favorite || e.personal(bob.ID, e.g1.ID).Favorite {
		t.Fatal("favorite not per user")
	}
	if err := e.svc.SetFavorite(ctx, e.admin.ID, "00000000-0000-4000-8000-000000000000", true); err != hub.ErrNotFound {
		t.Fatalf("unknown game: %v", err)
	}
	for i := 0; i < 2; i++ {
		if err := e.svc.SetFavorite(ctx, e.admin.ID, e.g1.ID, false); err != nil {
			t.Fatal(err)
		}
	}
	if e.personal(e.admin.ID, e.g1.ID).Favorite {
		t.Fatal("favorite not removed")
	}

	// Name rules.
	c, err := e.svc.CreateCollection(ctx, e.admin.ID, "  Pokemon  ", []string{e.g1.ID, e.g1.ID})
	if err != nil || c.Name != "Pokemon" || len(c.GameIDs) != 1 {
		t.Fatalf("%+v %v", c, err)
	}
	for _, name := range []string{"", "   ", strings.Repeat("x", 41), "a\nb"} {
		if _, err := e.svc.CreateCollection(ctx, e.admin.ID, name, nil); !isCode(err, hub.CodeBadRequest) {
			t.Fatalf("name %q: %v", name, err)
		}
	}
	if _, err := e.svc.CreateCollection(ctx, e.admin.ID, strings.Repeat("ä", 40), nil); err != nil {
		t.Fatalf("40 characters: %v", err)
	}
	if _, err := e.svc.CreateCollection(ctx, e.admin.ID, "POKEMON", nil); !isCode(err, hub.CodeConflict) {
		t.Fatalf("duplicate: %v", err)
	}
	if _, err := e.svc.CreateCollection(ctx, e.admin.ID, "Other", []string{"00000000-0000-4000-8000-000000000000"}); !isCode(err, hub.CodeBadRequest) {
		t.Fatalf("unknown game in create: %v", err)
	}
	if _, err := e.svc.CreateCollection(ctx, bob.ID, "Pokemon", nil); err != nil {
		t.Fatalf("another user may use the name: %v", err)
	}

	// Isolation: bob cannot see, change or delete admin's collection.
	if _, err := e.svc.RenameCollection(ctx, bob.ID, c.ID, "Mine"); err != hub.ErrNotFound {
		t.Fatalf("rename: %v", err)
	}
	if err := e.svc.DeleteCollection(ctx, bob.ID, c.ID); err != hub.ErrNotFound {
		t.Fatalf("delete: %v", err)
	}
	if err := e.svc.SetCollectionGame(ctx, bob.ID, c.ID, e.g2.ID, true); err != hub.ErrNotFound {
		t.Fatalf("add: %v", err)
	}
	if cs, _ := e.svc.ListCollections(ctx, bob.ID); len(cs) != 1 || cs[0].ID == c.ID {
		t.Fatalf("bob sees: %+v", cs)
	}

	// Rename (also in another case) and membership.
	if c, err = e.svc.RenameCollection(ctx, e.admin.ID, c.ID, "pokemon"); err != nil || c.Name != "pokemon" {
		t.Fatalf("%+v %v", c, err)
	}
	for i := 0; i < 2; i++ {
		if err := e.svc.SetCollectionGame(ctx, e.admin.ID, c.ID, e.g2.ID, true); err != nil {
			t.Fatal(err)
		}
	}
	if got, _ := e.svc.GetCollection(ctx, e.admin.ID, c.ID); len(got.GameIDs) != 2 || got.GameIDs[0] != e.g1.ID {
		t.Fatalf("games: %+v", got.GameIDs)
	}
	if ids := e.personal(e.admin.ID, e.g2.ID).CollectionIDs; len(ids) != 1 || ids[0] != c.ID {
		t.Fatalf("collection_ids: %v", ids)
	}
	if err := e.svc.SetCollectionGame(ctx, e.admin.ID, c.ID, e.g2.ID, false); err != nil {
		t.Fatal(err)
	}
	if err := e.svc.SetCollectionGame(ctx, e.admin.ID, c.ID, e.g2.ID, false); err != nil { // idempotent
		t.Fatal(err)
	}

	// Deleting a collection keeps the games; removing a game removes favorites and collection entries.
	if err := e.svc.DeleteCollection(ctx, e.admin.ID, c.ID); err != nil {
		t.Fatal(err)
	}
	if _, err := e.svc.GetGame(ctx, e.g1.ID); err != nil {
		t.Fatalf("game deleted with the collection: %v", err)
	}
	c2, _ := e.svc.CreateCollection(ctx, e.admin.ID, "Again", []string{e.g1.ID})
	e.svc.SetFavorite(ctx, e.admin.ID, e.g1.ID, true)
	if err := e.svc.DeleteGame(ctx, e.g1.ID); err != nil {
		t.Fatal(err)
	}
	if got, _ := e.svc.GetCollection(ctx, e.admin.ID, c2.ID); len(got.GameIDs) != 0 {
		t.Fatalf("entry of a removed game stays: %+v", got.GameIDs)
	}
	if n := countRows(t, e.db, `SELECT COUNT(*) FROM favorites WHERE game_id = ?`, e.g1.ID); n != 0 {
		t.Fatal("favorite of a removed game stays")
	}
}

func isCode(err error, c hub.Code) bool {
	e, ok := err.(*hub.Error)
	return ok && e.Code == c
}

func TestPlayStatsAdminUsersAndRemovedGame(t *testing.T) {
	e := newPlayEnv(t, nil)
	bob, _ := e.svc.CreateUser(ctx, "bob", "Bob")
	a, b := e.client(e.admin.ID), e.client(bob.ID)
	e.present(a, "in_game", e.g1.ID, "")
	e.present(b, "in_game", e.g2.ID, "")
	e.clk.Advance(60 * time.Second)
	e.present(a, "online", "", "")
	e.clk.Advance(30 * time.Second) // bob's play is still open
	rows, err := e.svc.ListUserRows(ctx)
	if err != nil {
		t.Fatal(err)
	}
	got := map[string]hub.UserRow{}
	for _, r := range rows {
		got[r.ID] = r
	}
	if r := got[e.admin.ID]; r.Seconds != 60 || r.Sessions != 1 || r.LastPlayedAt == nil {
		t.Fatalf("admin row: %+v", r.PlayStat)
	}
	if r := got[bob.ID]; r.Seconds != 90 || r.Sessions != 1 {
		t.Fatalf("bob row: %+v", r.PlayStat)
	}
	// The play time stays in the totals when the game is removed.
	if err := e.svc.DeleteGame(ctx, e.g1.ID); err != nil {
		t.Fatal(err)
	}
	rows, _ = e.svc.ListUserRows(ctx)
	for _, r := range rows {
		if r.ID == e.admin.ID && r.Seconds != 60 {
			t.Fatalf("total after removal: %d", r.Seconds)
		}
	}
	// A user with no play has zero and no last-played time.
	carol, _ := e.svc.CreateUser(ctx, "carol", "Carol")
	if st, err := e.svc.UserPlayStats(ctx, carol.ID); err != nil || st.Seconds != 0 || st.LastPlayedAt != nil {
		t.Fatalf("%+v %v", st, err)
	}
}

func TestRunStartsAutomaticallyWhenPlayBegins(t *testing.T) {
	e := newPlayEnv(t, nil)
	e.svc.SetPluginCatalog(onePlugin{"net.example.board", "1.0.0"})
	bob, err := e.svc.CreateUser(ctx, "bob", "Bob")
	if err != nil {
		t.Fatal(err)
	}
	run, err := e.svc.CreateRun(ctx, hub.CreateRunInput{UserID: e.admin.ID, Name: "Run", GameID: e.g1.ID, InviteUserIDs: []string{bob.ID},
		Plugins: []hub.RunPluginRef{{PluginID: "net.example.board", Version: "1.0.0"}}})
	if err != nil {
		t.Fatal(err)
	}
	status := func() hub.Run {
		got, err := e.svc.GetRun(ctx, e.admin.ID, run.ID)
		if err != nil {
			t.Fatal(err)
		}
		return got
	}
	// Browsing the game, playing outside the run, or an invited person playing does not start it.
	e.present(e.client(e.admin.ID), "in_game", e.g1.ID, "")
	e.present(e.client(bob.ID), "in_game", e.g1.ID, run.ID)
	if got := status(); got.Status != "starting" {
		t.Fatalf("status %s", got.Status)
	}
	// A joined participant playing the run's game starts it, recorded as automatic.
	e.present(e.client(e.admin.ID), "in_game", e.g1.ID, run.ID)
	got := status()
	if got.Status != "active" || got.StatusBy != e.admin.ID {
		t.Fatalf("status %s by %s", got.Status, got.StatusBy)
	}
	evs, _, err := e.svc.ListRunEvents(ctx, e.admin.ID, run.ID, 0, 50)
	if err != nil {
		t.Fatal(err)
	}
	last := evs[len(evs)-1]
	if last.Type != "run.status" || last.Source != "hub" ||
		!strings.Contains(string(last.Payload), `"auto":true`) || !strings.Contains(string(last.Payload), `"reason":"play_started"`) ||
		!strings.Contains(string(last.Payload), `"previous":"starting"`) {
		t.Fatalf("%+v %s", last, last.Payload)
	}
	// A pause is not undone by playing again.
	if _, err := e.svc.SetRunStatus(ctx, e.admin.ID, run.ID, "paused"); err != nil {
		t.Fatal(err)
	}
	e.present(e.client(e.admin.ID), "online", "", "")
	e.present(e.client(e.admin.ID), "in_game", e.g1.ID, run.ID)
	if got := status(); got.Status != "paused" {
		t.Fatalf("status %s", got.Status)
	}
}

func TestRunStartsAutomaticallyOnFirstEvent(t *testing.T) {
	// Safety net for runs whose app never reported play (and for old runs that stayed "starting").
	svc, _, admin, g := runFixture(t)
	run := newRun(t, svc, admin, g)
	add := func() {
		t.Helper()
		if _, _, err := svc.AppendRunEvent(ctx, hub.AppendRunEventInput{UserID: admin.ID, DeviceID: uuid.NewString(), RunID: run.ID,
			ClientID: uuid.NewString(), PluginID: "net.example.board", Source: "automatic", Type: "tick", Payload: json.RawMessage(`{}`)}); err != nil {
			t.Fatal(err)
		}
	}
	add()
	got, err := svc.GetRun(ctx, admin.ID, run.ID)
	if err != nil || got.Status != "active" || got.StatusBy != admin.ID {
		t.Fatalf("%+v %v", got, err)
	}
	evs, _, _ := svc.ListRunEvents(ctx, admin.ID, run.ID, 0, 50)
	last := evs[len(evs)-1]
	if last.Type != "run.status" || !strings.Contains(string(last.Payload), `"reason":"first_event"`) {
		t.Fatalf("%+v %s", last, last.Payload)
	}
	n := len(evs)
	add() // no second start event
	if evs, _, _ := svc.ListRunEvents(ctx, admin.ID, run.ID, 0, 50); len(evs) != n+1 {
		t.Fatalf("%d events, want %d", len(evs), n+1)
	}
}
