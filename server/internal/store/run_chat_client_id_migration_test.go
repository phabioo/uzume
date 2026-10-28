package store

import (
	"path/filepath"
	"testing"
)

func TestMigrationRunChatClientIDPerUser(t *testing.T) {
	p := filepath.Join(t.TempDir(), "uzume.db")
	raw := rawDB(t, p)
	if err := migrate(raw, "", 27); err != nil {
		t.Fatal(err)
	}
	for _, q := range []string{
		`INSERT INTO users(id, username, display_name, role, created_at) VALUES ('u1', 'admin', 'admin', 'admin', 1)`,
		`INSERT INTO runs(id, name, status, created_by, created_at, updated_at, status_by) VALUES ('r1', 'Run', 'starting', 'u1', 1, 1, 'u1')`,
		`INSERT INTO run_chat_messages(run_id, seq, user_id, text, mentions, client_id, sent_at) VALUES ('r1', 1, 'u1', 'a', '["u2"]', 'c1', 5)`,
		`INSERT INTO run_chat_messages(run_id, seq, user_id, text, client_id, sent_at) VALUES ('r1', 2, 'u1', 'b', 'c2', 6)`,
	} {
		if _, err := raw.Exec(q); err != nil {
			t.Fatal(err)
		}
	}
	raw.Close()

	db, err := Open(p)
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	var n int
	var text, mentions string
	if err := db.QueryRow(`SELECT COUNT(*) FROM run_chat_messages`).Scan(&n); err != nil || n != 2 {
		t.Fatalf("rows after 0029: %d (%v)", n, err)
	}
	if err := db.QueryRow(`SELECT text, mentions FROM run_chat_messages WHERE run_id = 'r1' AND seq = 1`).Scan(&text, &mentions); err != nil || text != "a" || mentions != `["u2"]` {
		t.Fatalf("row 1: %q %q (%v)", text, mentions, err)
	}
	if err := db.QueryRow(`SELECT mentions FROM run_chat_messages WHERE seq = 2`).Scan(&mentions); err != nil || mentions != "[]" {
		t.Fatalf("mentions default %q (%v)", mentions, err)
	}
	if _, err := db.Exec(`INSERT INTO users(id, username, display_name, role, created_at) VALUES ('u2', 'anna', 'Anna', 'user', 1)`); err != nil {
		t.Fatal(err)
	}
	// another sender may reuse the client_id, the same sender may not
	if _, err := db.Exec(`INSERT INTO run_chat_messages(run_id, seq, user_id, text, client_id, sent_at) VALUES ('r1', 3, 'u2', 'c', 'c1', 7)`); err != nil {
		t.Fatalf("same client_id from another user must be accepted: %v", err)
	}
	if _, err := db.Exec(`INSERT INTO run_chat_messages(run_id, seq, user_id, text, client_id, sent_at) VALUES ('r1', 4, 'u1', 'd', 'c1', 8)`); err == nil {
		t.Fatal("a repeated client_id of the same user must be refused")
	}
	if _, err := db.Exec(`INSERT INTO run_chat_messages(run_id, seq, user_id, text, client_id, sent_at) VALUES ('r1', 2, 'u2', 'e', 'c9', 8)`); err == nil {
		t.Fatal("a repeated seq must be refused")
	}
	if _, err := db.Exec(`INSERT INTO run_chat_messages(run_id, seq, user_id, text, client_id, sent_at) VALUES ('missing', 1, 'u1', 'x', 'c1', 5)`); err == nil {
		t.Fatal("the foreign key to runs must survive the rebuild")
	}
	if _, err := db.Exec(`DELETE FROM runs WHERE id = 'r1'`); err != nil {
		t.Fatal(err)
	}
	if err := db.QueryRow(`SELECT COUNT(*) FROM run_chat_messages`).Scan(&n); err != nil || n != 0 {
		t.Fatalf("messages %d (%v): deleting a run must still remove its chat", n, err)
	}
	if err := db.QueryRow(`SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name LIKE 'run_chat_messages%'`).Scan(&n); err != nil || n != 1 {
		t.Fatalf("leftover tables: %d (%v)", n, err)
	}
}
