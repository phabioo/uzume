package store

import (
	"path/filepath"
	"testing"
)

func TestMigrationRunChatKeepsDataAndCascades(t *testing.T) {
	p := filepath.Join(t.TempDir(), "uzume.db")
	raw := rawDB(t, p)
	if err := migrate(raw, "", 26); err != nil {
		t.Fatal(err)
	}
	raw.Close()

	db, err := Open(p)
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	if _, err := db.Exec(`PRAGMA foreign_keys = ON`); err != nil {
		t.Fatal(err)
	}
	if _, err := db.Exec(`INSERT INTO users(id, username, display_name, role, created_at) VALUES ('u1', 'admin', 'admin', 'admin', 1)`); err != nil {
		t.Fatal(err)
	}
	if _, err := db.Exec(`INSERT INTO run_chat_messages(run_id, seq, user_id, text, client_id, sent_at) VALUES ('missing', 1, 'u1', 'x', 'c1', 5)`); err == nil {
		t.Fatal("a message of a missing run must be refused")
	}
	var runCols string
	if err := db.QueryRow(`SELECT group_concat(name) FROM pragma_table_info('runs') WHERE pk = 1`).Scan(&runCols); err != nil || runCols != "id" {
		t.Fatalf("runs pk %q (%v)", runCols, err)
	}
	if _, err := db.Exec(`INSERT INTO runs(id, name, status, created_by, created_at, updated_at, status_by) VALUES ('r1', 'Run', 'starting', 'u1', 1, 1, 'u1')`); err != nil {
		t.Fatal(err)
	}
	for _, q := range []string{
		`INSERT INTO run_chat_messages(run_id, seq, user_id, text, client_id, sent_at) VALUES ('r1', 1, 'u1', 'a', 'c1', 5)`,
		`INSERT INTO run_chat_messages(run_id, seq, user_id, text, client_id, sent_at) VALUES ('r1', 2, 'u1', 'b', 'c2', 6)`,
	} {
		if _, err := db.Exec(q); err != nil {
			t.Fatal(err)
		}
	}
	if _, err := db.Exec(`INSERT INTO run_chat_messages(run_id, seq, user_id, text, client_id, sent_at) VALUES ('r1', 3, 'u1', 'c', 'c1', 7)`); err == nil {
		t.Fatal("a repeated client_id must be refused")
	}
	if _, err := db.Exec(`INSERT INTO run_chat_messages(run_id, seq, user_id, text, client_id, sent_at) VALUES ('r1', 2, 'u1', 'c', 'c3', 7)`); err == nil {
		t.Fatal("a repeated seq must be refused")
	}
	var m string
	if err := db.QueryRow(`SELECT mentions FROM run_chat_messages WHERE seq = 1`).Scan(&m); err != nil || m != "[]" {
		t.Fatalf("mentions default %q (%v)", m, err)
	}
	if _, err := db.Exec(`DELETE FROM runs WHERE id = 'r1'`); err != nil {
		t.Fatal(err)
	}
	var n int
	if err := db.QueryRow(`SELECT COUNT(*) FROM run_chat_messages`).Scan(&n); err != nil || n != 0 {
		t.Fatalf("messages %d (%v): deleting a run removes its chat", n, err)
	}
}
