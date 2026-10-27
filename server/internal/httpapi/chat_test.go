package httpapi

import (
	"fmt"
	"testing"
)

type chatList struct {
	Messages []struct {
		Seq      int64    `json:"seq"`
		UserID   string   `json:"user_id"`
		Text     string   `json:"text"`
		Mentions []string `json:"mentions"`
	} `json:"messages"`
	NextAfterSeq *int64 `json:"next_after_seq"`
}

func TestRunChatREST(t *testing.T) {
	r := newRunEnv(t)
	d1, a1, b1, e1 := r.device(r.admin.ID, "Desktop"), r.device(r.anna.ID, "Anna PC"), r.device(r.bob.ID, "Bob PC"), r.device(r.carol.ID, "Carol PC")
	run := r.create(d1, "Chat", r.anna.ID, r.bob.ID)
	r.mustJoin(a1, run.RunID)
	ws := r.dial(d1)
	for i := 1; i <= 3; i++ {
		ws.send("chat_send", map[string]any{"scope": "run", "id": run.RunID, "client_id": fmt.Sprint("c", i), "text": fmt.Sprint("hello @Anna ", i)})
		ws.await("chat_message")
	}
	path := "/api/v1/runs/" + run.RunID + "/chat"
	rec := r.api(a1, "GET", path, nil)
	wantStatus(t, rec, 200, "")
	l := decode[chatList](t, rec)
	if len(l.Messages) != 3 || l.NextAfterSeq != nil || l.Messages[0].Seq != 1 || len(l.Messages[0].Mentions) != 1 || l.Messages[0].Mentions[0] != r.anna.ID {
		t.Fatalf("chat: %+v", l)
	}
	l = decode[chatList](t, r.api(a1, "GET", path+"?after_seq=1&limit=1", nil))
	if len(l.Messages) != 1 || l.Messages[0].Seq != 2 || l.NextAfterSeq == nil || *l.NextAfterSeq != 2 {
		t.Fatalf("page: %+v", l)
	}
	wantStatus(t, r.api(b1, "GET", path, nil), 403, "forbidden") // invited, not joined
	wantStatus(t, r.api(e1, "GET", path, nil), 404, "not_found") // stranger
}

func TestSessionChatREST(t *testing.T) {
	s := newSessEnv(t, nil)
	owner, anna, bob := s.device(s.admin.ID, "Desktop"), s.device(s.anna.ID, "Anna PC"), s.device(s.bob.ID, "Bob PC")
	sess := s.publish(owner, "hub_users")
	s.mustJoin(anna, sess.SessionID)
	wo := s.dial(owner)
	wo.send("chat_send", map[string]any{"scope": "session", "id": sess.SessionID, "client_id": "c1", "text": "hi"})
	wo.await("chat_message")
	path := "/api/v1/sessions/" + sess.SessionID + "/chat"
	for _, d := range []dev{owner, anna} {
		rec := s.api(d, "GET", path, nil)
		wantStatus(t, rec, 200, "")
		if l := decode[chatList](t, rec); len(l.Messages) != 1 || l.Messages[0].Text != "hi" {
			t.Fatalf("ring: %+v", l)
		}
	}
	wantStatus(t, s.api(bob, "GET", path, nil), 403, "") // allowed to see the Session, but no viewer
	wantStatus(t, s.api(owner, "DELETE", "/api/v1/sessions/"+sess.SessionID, nil), 204, "")
	wantStatus(t, s.api(owner, "GET", path, nil), 410, "")
}
