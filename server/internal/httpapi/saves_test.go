package httpapi

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/google/uuid"

	"github.com/phabioo/uzume/server/internal/hub"
)

func hexSHA(b []byte) string { h := sha256.Sum256(b); return hex.EncodeToString(h[:]) }

type savesAPI struct {
	*env
	game hub.Game
	devA string
	tokA string
	devB string
	tokB string
}

func newSavesAPI(t *testing.T) *savesAPI {
	e := newEnv(t, nil)
	g, err := e.svc.AddROM(context.Background(), bytes.NewReader([]byte("homebrew-rom-dummy")), "demo.nds", "", "", e.admin.ID)
	if err != nil {
		t.Fatal(err)
	}
	a := &savesAPI{env: e, game: g, devA: uuid.NewString(), devB: uuid.NewString()}
	a.tokA, a.tokB = e.login(a.devA), e.login(a.devB)
	return a
}

func (a *savesAPI) path(suffix string) string {
	return "/api/v1/games/" + a.game.ID + "/saves/default" + suffix
}

func (a *savesAPI) put(tok string, base int, data []byte, reason string) *httptest.ResponseRecorder {
	return a.putSHA(tok, base, data, hexSHA(data), reason)
}

func (a *savesAPI) putSHA(tok string, base int, data []byte, sha, reason string) *httptest.ResponseRecorder {
	a.t.Helper()
	return a.do("PUT", a.path(""), nil, opt{token: tok, data: data, ctype: "application/octet-stream", header: map[string]string{
		"X-Uzume-Base-Revision": fmt.Sprint(base), "X-Uzume-Content-SHA256": sha, "X-Uzume-Sync-Reason": reason}})
}

type slotResp struct {
	GameID  string `json:"game_id"`
	Slot    string `json:"slot"`
	Current struct {
		Revision   int    `json:"revision"`
		Sha256     string `json:"sha256"`
		Size       int64  `json:"size"`
		DeviceID   string `json:"device_id"`
		DeviceName string `json:"device_name"`
		Reason     string `json:"reason"`
	} `json:"current"`
	OpenConflicts []conflictResp `json:"open_conflicts"`
}

type conflictResp struct {
	ID     string `json:"id"`
	Status string `json:"status"`
	Hub    struct {
		Revision int `json:"revision"`
	} `json:"hub"`
	Secured struct {
		Version      int    `json:"version"`
		Sha256       string `json:"sha256"`
		BaseRevision int    `json:"base_revision"`
		DeviceName   string `json:"device_name"`
	} `json:"secured"`
}

func TestSavesAPIFlow(t *testing.T) {
	a := newSavesAPI(t)
	// No slot yet.
	wantStatus(t, a.do("GET", a.path(""), nil, opt{token: a.tokA}), 404, "not_found")
	if l := decode[struct {
		Saves []any `json:"saves"`
	}](t, a.do("GET", "/api/v1/saves", nil, opt{token: a.tokA})); len(l.Saves) != 0 {
		t.Fatalf("%v", l)
	}

	v1, v2, v3 := []byte("save-1"), []byte("save-2"), []byte("save-3-laptop")
	rec := a.put(a.tokA, 0, v1, "checkpoint")
	wantStatus(t, rec, 200, "")
	if s := decode[slotResp](t, rec); s.Current.Revision != 1 || s.Current.Sha256 != hexSHA(v1) || s.Current.Size != int64(len(v1)) {
		t.Fatalf("%+v", s)
	}
	// Idempotent retry.
	wantStatus(t, a.put(a.tokA, 0, v1, "checkpoint"), 200, "")
	rec = a.put(a.tokA, 1, v2, "final_session_end")
	wantStatus(t, rec, 200, "")
	if s := decode[slotResp](t, rec); s.Current.Revision != 2 {
		t.Fatalf("%+v", s)
	}

	// List and slot.
	l := decode[struct {
		Saves []struct {
			GameID            string `json:"game_id"`
			OpenConflictCount int    `json:"open_conflict_count"`
		} `json:"saves"`
	}](t, a.do("GET", "/api/v1/saves", nil, opt{token: a.tokA}))
	if len(l.Saves) != 1 || l.Saves[0].GameID != a.game.ID || l.Saves[0].OpenConflictCount != 0 {
		t.Fatalf("%+v", l)
	}
	// Content with headers.
	rec = a.do("GET", a.path("/content"), nil, opt{token: a.tokA})
	wantStatus(t, rec, 200, "")
	if !bytes.Equal(rec.Body.Bytes(), v2) || rec.Header().Get("ETag") != `"`+hexSHA(v2)+`"` || rec.Header().Get("X-Uzume-Save-Revision") != "2" {
		t.Fatalf("%q %v", rec.Body.String(), rec.Header())
	}

	// Stale base from another device -> 409 save_conflict with the conflict in the body.
	rec = a.put(a.tokB, 1, v3, "checkpoint")
	wantStatus(t, rec, 409, "save_conflict")
	c := decode[struct {
		Conflict conflictResp `json:"conflict"`
	}](t, rec).Conflict
	if c.Status != "open" || c.Hub.Revision != 2 || c.Secured.BaseRevision != 1 || c.Secured.Sha256 != hexSHA(v3) || c.Secured.DeviceName != "Living-Room-PC" {
		t.Fatalf("%+v", c)
	}
	rec = a.do("GET", a.path(""), nil, opt{token: a.tokA})
	wantStatus(t, rec, 200, "")
	if s := decode[slotResp](t, rec); s.Current.Revision != 2 || len(s.OpenConflicts) != 1 {
		t.Fatalf("%+v", s)
	}

	// History (newest first) and content of a history version.
	h := decode[struct {
		Versions []struct {
			Version  int    `json:"version"`
			Revision int    `json:"revision"`
			Reason   string `json:"reason"`
		} `json:"versions"`
	}](t, a.do("GET", a.path("/history"), nil, opt{token: a.tokA}))
	if len(h.Versions) != 2 || h.Versions[0].Reason != "conflict_upload" || h.Versions[1].Reason != "session_end" {
		t.Fatalf("%+v", h)
	}
	rec = a.do("GET", a.path(fmt.Sprintf("/history/%d/content", c.Secured.Version)), nil, opt{token: a.tokA})
	wantStatus(t, rec, 200, "")
	if !bytes.Equal(rec.Body.Bytes(), v3) {
		t.Fatal("secured upload content differs")
	}
	wantStatus(t, a.do("GET", a.path("/history/99/content"), nil, opt{token: a.tokA}), 404, "not_found")

	// Resolve with a stale expected_revision: 409 save_conflict_stale, nothing changed.
	resolve := func(tok string, id, resolution string, expected int) *httptest.ResponseRecorder {
		return a.do("POST", a.path("/conflicts/"+id+"/resolve"), map[string]any{"resolution": resolution, "expected_revision": expected}, opt{token: tok})
	}
	wantStatus(t, resolve(a.tokB, c.ID, "use_local", 1), 409, "save_conflict_stale")
	rec = a.do("GET", a.path(""), nil, opt{token: a.tokA})
	if s := decode[slotResp](t, rec); s.Current.Revision != 2 || len(s.OpenConflicts) != 1 {
		t.Fatalf("state changed: %+v", s)
	}
	wantStatus(t, resolve(a.tokB, "c_unknown", "use_hub", 2), 404, "not_found")
	// use_local: the secured upload becomes revision 3.
	rec = resolve(a.tokB, c.ID, "use_local", 2)
	wantStatus(t, rec, 200, "")
	if s := decode[slotResp](t, rec); s.Current.Revision != 3 || s.Current.Sha256 != hexSHA(v3) || len(s.OpenConflicts) != 0 {
		t.Fatalf("%+v", s)
	}
	// Resolving again: already resolved -> save_conflict_stale.
	wantStatus(t, resolve(a.tokB, c.ID, "use_hub", 3), 409, "save_conflict_stale")
	// before_conflict_resolution is in the history now.
	h = decode[struct {
		Versions []struct {
			Version  int    `json:"version"`
			Revision int    `json:"revision"`
			Reason   string `json:"reason"`
		} `json:"versions"`
	}](t, a.do("GET", a.path("/history"), nil, opt{token: a.tokA}))
	if len(h.Versions) != 2 { // session_end of Rev 2 already captured the checkpoint, so nothing new is added
		t.Fatalf("%+v", h)
	}
}

func TestSavesAPIResolveUseHub(t *testing.T) {
	a := newSavesAPI(t)
	wantStatus(t, a.put(a.tokA, 0, []byte("one"), "checkpoint"), 200, "")
	rec := a.put(a.tokB, 0, []byte("other"), "checkpoint")
	wantStatus(t, rec, 409, "save_conflict")
	id := decode[struct{ Conflict conflictResp }](t, rec).Conflict.ID
	rec = a.do("POST", a.path("/conflicts/"+id+"/resolve"), map[string]any{"resolution": "use_hub", "expected_revision": 1}, opt{token: a.tokB})
	wantStatus(t, rec, 200, "")
	if s := decode[slotResp](t, rec); s.Current.Revision != 1 || s.Current.Sha256 != hexSHA([]byte("one")) || len(s.OpenConflicts) != 0 {
		t.Fatalf("%+v", s)
	}
	h := decode[struct {
		Versions []struct{ Reason string } `json:"versions"`
	}](t, a.do("GET", a.path("/history"), nil, opt{token: a.tokA}))
	if len(h.Versions) != 2 || h.Versions[0].Reason != "before_conflict_resolution" || h.Versions[1].Reason != "conflict_upload" {
		t.Fatalf("%+v", h)
	}
}

func TestSavesAPIUploadErrors(t *testing.T) {
	a := newSavesAPI(t)
	// Hash mismatch -> 400, nothing stored.
	wantStatus(t, a.putSHA(a.tokA, 0, []byte("abc"), hexSHA([]byte("other")), "checkpoint"), 400, "bad_request")
	wantStatus(t, a.do("GET", a.path(""), nil, opt{token: a.tokA}), 404, "not_found")
	// Over 64 MiB -> 413.
	big := bytes.Repeat([]byte{1}, hub.MaxSaveBytes+1)
	rec := a.do("PUT", a.path(""), nil, opt{token: a.tokA, data: big, ctype: "application/octet-stream", raw: true, header: map[string]string{
		"X-Uzume-Base-Revision": "0", "X-Uzume-Content-SHA256": hexSHA(big), "X-Uzume-Sync-Reason": "checkpoint"}})
	wantStatus(t, rec, 413, "payload_too_large")
	wantStatus(t, a.do("GET", a.path(""), nil, opt{token: a.tokA}), 404, "not_found")
	// Unknown game -> 404; no token -> 401.
	rec = a.do("PUT", "/api/v1/games/"+uuid.NewString()+"/saves/default", nil, opt{token: a.tokA, data: []byte("x"), ctype: "application/octet-stream",
		header: map[string]string{"X-Uzume-Base-Revision": "0", "X-Uzume-Content-SHA256": hexSHA([]byte("x")), "X-Uzume-Sync-Reason": "checkpoint"}})
	wantStatus(t, rec, 404, "not_found")
	wantStatus(t, a.put("", 0, []byte("x"), "checkpoint"), 401, "")
}

func TestSavesAPICrossUserAndRevoked(t *testing.T) {
	a := newSavesAPI(t)
	wantStatus(t, a.put(a.tokA, 0, []byte("private"), "checkpoint"), 200, "")
	// A device of another user sees nothing of it.
	other, err := a.svc.CreateUser(context.Background(), "anna", "Anna")
	if err != nil {
		t.Fatal(err)
	}
	devC := uuid.NewString()
	p := a.request(devC)
	if err := a.svc.ApprovePairing(context.Background(), p.RequestID, other.ID); err != nil {
		t.Fatal(err)
	}
	cred := decode[pollResp](t, a.poll(p)).DeviceCredential
	tokC := decode[struct {
		AccessToken string `json:"access_token"`
	}](t, a.accessToken(devC, cred)).AccessToken
	for _, suffix := range []string{"", "/content", "/history", "/history/1/content"} {
		if code := a.do("GET", a.path(suffix), nil, opt{token: tokC}).Code; code != 404 && code != 403 {
			t.Fatalf("GET %s as another user: %d", suffix, code)
		}
	}
	if l := decode[struct {
		Saves []any `json:"saves"`
	}](t, a.do("GET", "/api/v1/saves", nil, opt{token: tokC})); len(l.Saves) != 0 {
		t.Fatalf("%v", l)
	}
	rec := a.do("POST", a.path("/conflicts/c_x/resolve"), map[string]any{"resolution": "use_hub", "expected_revision": 1}, opt{token: tokC})
	if rec.Code != 404 && rec.Code != 403 {
		t.Fatalf("resolve as another user: %d", rec.Code)
	}
	// The other user's own upload with base 0 starts an independent slot.
	wantStatus(t, a.do("PUT", a.path(""), nil, opt{token: tokC, data: []byte("mine"), ctype: "application/octet-stream", header: map[string]string{
		"X-Uzume-Base-Revision": "0", "X-Uzume-Content-SHA256": hexSHA([]byte("mine")), "X-Uzume-Sync-Reason": "checkpoint"}}), 200, "")
	// A revoked device (tokens deleted): 401 on all save endpoints.
	if err := a.svc.RevokeDevice(context.Background(), a.devA); err != nil {
		t.Fatal(err)
	}
	wantStatus(t, a.put(a.tokA, 1, []byte("x"), "checkpoint"), 401, "")
	wantStatus(t, a.do("GET", "/api/v1/saves", nil, opt{token: a.tokA}), 401, "")
	wantStatus(t, a.do("GET", a.path("/content"), nil, opt{token: a.tokA}), 401, "")
}

func TestHandshakeAdvertisesFeatures(t *testing.T) {
	e := newEnv(t, nil)
	tok := e.login(uuid.NewString())
	rec := e.do("POST", "/api/v1/handshake", handshakeBody(1, 1), opt{token: tok})
	wantStatus(t, rec, 200, "")
	f := decode[struct {
		Features []string `json:"features"`
	}](t, rec).Features
	// The test device belongs to the admin, who may always upload and gets the admin API.
	if strings.Join(f, ",") != "saves_v1,sessions_v1,users_v1,firmware_v1,cores_v1,saves_v2,cores_v2,saves_v3,saves_v4,saves_v5,saves_v6,local_setup_v1,ice_update_v1,plugins_v1,runs_v1,runs_v2,local_roms_v1,reports_v1,accounts_v1,setup_code_v1,states_v1,run_rules_v1,tools_v1,variants_v1,plugin_store_v1,cheat_db_v1,reports_v2,run_plugin_updates_v1,uploads_v1,admin_v1" {
		t.Fatalf("%v", f)
	}
}

func (a *savesAPI) upload(tok, slot string, expected any, data []byte, sha string) *httptest.ResponseRecorder {
	a.t.Helper()
	return a.do("POST", "/api/v1/games/"+a.game.ID+"/saves/"+slot+"/upload", nil, opt{token: tok, data: data, ctype: "application/octet-stream", raw: true,
		header: map[string]string{"X-Uzume-Expected-Revision": fmt.Sprint(expected), "X-Uzume-Content-SHA256": sha}})
}

// The body limit middleware must allow save uploads above the 1 MiB default of other endpoints.
func TestUploadSaveFileAboveDefaultBodyLimit(t *testing.T) {
	a := newSavesAPI(t)
	data := bytes.Repeat([]byte{7}, 2<<20)
	wantStatus(t, a.upload(a.tokA, "big", 0, data, hexSHA(data)), 200, "")
}

func TestUploadSaveFileAPI(t *testing.T) {
	a := newSavesAPI(t)
	data := []byte("other-emulator-sav")
	// New slot at revision 1.
	rec := a.upload(a.tokA, "default", 0, data, hexSHA(data))
	wantStatus(t, rec, 200, "")
	if c := decode[slotResp](t, rec).Current; c.Revision != 1 || c.Reason != "upload" || c.Sha256 != hexSHA(data) {
		t.Fatalf("%+v", c)
	}
	// Existing slot: expected 0 is stale, expected 1 replaces and keeps the old version in the history.
	wantStatus(t, a.upload(a.tokA, "default", 0, []byte("b"), hexSHA([]byte("b"))), 409, "save_conflict_stale")
	wantStatus(t, a.upload(a.tokB, "default", 1, []byte("b"), hexSHA([]byte("b"))), 200, "")
	h := decode[struct {
		Versions []versionResp
	}](t, a.do("GET", a.path("/history"), nil, opt{token: a.tokA})).Versions
	if len(h) != 1 || h[0].Reason != "before_upload" || h[0].Revision != 1 {
		t.Fatalf("%+v", h)
	}
	// Missing slot with N > 0, bad input.
	wantStatus(t, a.upload(a.tokA, "other", 3, data, hexSHA(data)), 409, "save_conflict_stale")
	wantStatus(t, a.upload(a.tokA, "other", 0, data, hexSHA([]byte("x"))), 400, "bad_request")
	wantStatus(t, a.upload(a.tokA, "other", 0, nil, hexSHA(nil)), 400, "bad_request")
	wantStatus(t, a.upload(a.tokA, "other", -1, data, hexSHA(data)), 400, "")
	wantStatus(t, a.upload(a.tokA, "BadSlot", 0, data, hexSHA(data)), 400, "")
	wantStatus(t, a.upload("", "other", 0, data, hexSHA(data)), 401, "")
	// Over 64 MiB -> 413.
	big := bytes.Repeat([]byte{1}, hub.MaxSaveBytes+1)
	wantStatus(t, a.upload(a.tokA, "other", 0, big, hexSHA(big)), 413, "payload_too_large")
	wantStatus(t, a.do("GET", "/api/v1/games/"+a.game.ID+"/saves/other", nil, opt{token: a.tokA}), 404, "not_found")
	// Unknown game -> 404.
	rec = a.do("POST", "/api/v1/games/"+uuid.NewString()+"/saves/default/upload", nil, opt{token: a.tokA, data: data, ctype: "application/octet-stream", raw: true,
		header: map[string]string{"X-Uzume-Expected-Revision": "0", "X-Uzume-Content-SHA256": hexSHA(data)}})
	wantStatus(t, rec, 404, "not_found")
}
