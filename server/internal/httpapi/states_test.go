package httpapi

import (
	"bytes"
	"context"
	"fmt"
	"mime/multipart"
	"net/http/httptest"
	"net/textproto"
	"os"
	"path/filepath"
	"testing"

	"github.com/phabioo/uzume/server/internal/hub"
)

// Save states over REST (0.20 package 2, feature states_v1). State blobs are dummy bytes, previews are generated PNGs.

type stateT struct {
	GameID       string `json:"game_id"`
	Slot         string `json:"slot"`
	N            int    `json:"n"`
	CoreID       string `json:"core_id"`
	CoreVersion  string `json:"core_version"`
	SizeBytes    int64  `json:"size_bytes"`
	HasPreview   bool   `json:"has_preview"`
	PreviewBytes int64  `json:"preview_bytes"`
	Sha256       string `json:"sha256"`
	DeviceID     string `json:"device_id"`
	DeviceName   string `json:"device_name"`
	CreatedAt    string `json:"created_at"`
}

type stateListT struct {
	States     []stateT `json:"states"`
	UsedBytes  int64    `json:"used_bytes"`
	QuotaBytes int64    `json:"quota_bytes"`
}

// stateBody builds the multipart body of PUT /games/{id}/states/{slot}/{n}: meta first, then state, then the preview.
func stateBody(t *testing.T, meta string, state, preview []byte, parts ...string) ([]byte, string) {
	t.Helper()
	var buf bytes.Buffer
	mw := multipart.NewWriter(&buf)
	add := func(name, ctype string, data []byte) {
		h := textproto.MIMEHeader{}
		h.Set("Content-Disposition", fmt.Sprintf(`form-data; name=%q; filename=%q`, name, name))
		h.Set("Content-Type", ctype)
		w, err := mw.CreatePart(h)
		if err != nil {
			t.Fatal(err)
		}
		w.Write(data)
	}
	for _, p := range parts { // custom part order or names for the error cases
		add(p, "application/octet-stream", []byte("x"))
	}
	if len(parts) == 0 {
		if meta != "" {
			add("meta", "application/json", []byte(meta))
		}
		if state != nil {
			add("state", "application/octet-stream", state)
		}
		if preview != nil {
			add("preview", "image/png", preview)
		}
	}
	mw.Close()
	return buf.Bytes(), mw.FormDataContentType()
}

const goodStateMeta = `{"core_id":"melonds","core_version":"2026.1"}`

func (s *sessEnv) statePath(game, slot string, n int, rest string) string {
	return fmt.Sprintf("/api/v1/games/%s/states/%s/%d%s", game, slot, n, rest)
}

func (s *sessEnv) putState(d dev, game, slot string, n int, meta string, state, preview []byte) *httptest.ResponseRecorder {
	s.t.Helper()
	data, ct := stateBody(s.t, meta, state, preview)
	return s.do("PUT", s.statePath(game, slot, n, ""), nil, opt{token: d.tok, data: data, ctype: ct})
}

// putStateRaw sends an upload that may break the spec on purpose.
func (s *sessEnv) putStateRaw(d dev, game, slot string, n int, meta string, state, preview []byte) *httptest.ResponseRecorder {
	s.t.Helper()
	data, ct := stateBody(s.t, meta, state, preview)
	return s.do("PUT", s.statePath(game, slot, n, ""), nil, opt{token: d.tok, data: data, ctype: ct, raw: true})
}

func TestSaveStatesAPI(t *testing.T) {
	s := newSessEnv(t, nil)
	anna, carol := s.device(s.anna.ID, "Annas PC"), s.device(s.anna.ID, "Annas Laptop")
	bob := s.device(s.bob.ID, "Bobs PC")
	gid := s.game.ID
	const unknown = "00000000-0000-4000-8000-000000000000"
	blob := bytes.Repeat([]byte{0xAB, 0xCD}, 1000)
	preview := testPNG(t, 256, 192)

	// Auth.
	wantStatus(t, s.do("GET", "/api/v1/games/"+gid+"/states", nil, opt{}), 401, "")
	wantStatus(t, s.do("GET", s.statePath(gid, "default", 1, ""), nil, opt{}), 401, "")
	wantStatus(t, s.do("DELETE", s.statePath(gid, "default", 1, ""), nil, opt{}), 401, "")
	wantStatus(t, s.do("GET", s.statePath(gid, "default", 1, "/preview"), nil, opt{}), 401, "")
	data, ct := stateBody(t, goodStateMeta, blob, nil)
	wantStatus(t, s.do("PUT", s.statePath(gid, "default", 1, ""), nil, opt{data: data, ctype: ct}), 401, "")

	// Empty list with the quota numbers.
	l := decode[stateListT](t, s.api(anna, "GET", "/api/v1/games/"+gid+"/states", nil))
	if len(l.States) != 0 || l.UsedBytes != 0 || l.QuotaBytes != hub.DefaultStateQuotaBytes {
		t.Fatalf("%+v", l)
	}

	// Store with preview: metadata comes back, blob and preview are served exactly as uploaded.
	rec := s.putState(anna, gid, "default", 1, goodStateMeta, blob, preview)
	wantStatus(t, rec, 200, "")
	st := decode[stateT](t, rec)
	if st.GameID != gid || st.Slot != "default" || st.N != 1 || st.CoreID != "melonds" || st.CoreVersion != "2026.1" ||
		st.SizeBytes != int64(len(blob)) || !st.HasPreview || st.PreviewBytes != int64(len(preview)) || st.Sha256 != hexSHA(blob) ||
		st.DeviceID != anna.id || st.DeviceName != "Annas PC" || st.CreatedAt == "" {
		t.Fatalf("%+v", st)
	}
	rec = s.api(carol, "GET", s.statePath(gid, "default", 1, ""), nil)
	wantStatus(t, rec, 200, "")
	if !bytes.Equal(rec.Body.Bytes(), blob) || rec.Header().Get("X-Uzume-Core-Id") != "melonds" || rec.Header().Get("X-Uzume-Core-Version") != "2026.1" ||
		rec.Header().Get("ETag") != `"`+hexSHA(blob)+`"` {
		t.Fatalf("blob or headers differ: %v", rec.Header())
	}
	rec = s.api(carol, "GET", s.statePath(gid, "default", 1, "/preview"), nil)
	wantStatus(t, rec, 200, "")
	if !bytes.Equal(rec.Body.Bytes(), preview) || rec.Header().Get("Content-Type") != "image/png" {
		t.Fatal("preview differs")
	}

	// Quick slot, a second save slot, listing order and the slot filter.
	wantStatus(t, s.putState(anna, gid, "default", 0, goodStateMeta, []byte("quick"), nil), 200, "")
	wantStatus(t, s.putState(anna, gid, "run-1a2b3c4d", 10, goodStateMeta, []byte("run state"), nil), 200, "")
	l = decode[stateListT](t, s.api(anna, "GET", "/api/v1/games/"+gid+"/states", nil))
	if len(l.States) != 3 || l.States[0].Slot != "default" || l.States[0].N != 0 || l.States[1].N != 1 || l.States[2].Slot != "run-1a2b3c4d" || l.States[2].N != 10 {
		t.Fatalf("%+v", l.States)
	}
	if want := int64(len(blob) + len(preview) + len("quick") + len("run state")); l.UsedBytes != want {
		t.Fatalf("used %d, want %d", l.UsedBytes, want)
	}
	l = decode[stateListT](t, s.api(anna, "GET", "/api/v1/games/"+gid+"/states?slot=default", nil))
	if len(l.States) != 2 || l.States[0].HasPreview {
		t.Fatalf("%+v", l.States)
	}
	wantStatus(t, s.do("GET", "/api/v1/games/"+gid+"/states?slot=Bad!", nil, opt{token: anna.tok, raw: true}), 400, "bad_request")

	// A replacement without a preview drops the old preview and changes the metadata.
	rec = s.putState(anna, gid, "default", 1, `{"core_id":"desmume","core_version":"1.0"}`, []byte("newer state"), nil)
	wantStatus(t, rec, 200, "")
	st = decode[stateT](t, rec)
	if st.CoreID != "desmume" || st.SizeBytes != int64(len("newer state")) || st.HasPreview || st.Sha256 != hexSHA([]byte("newer state")) {
		t.Fatalf("%+v", st)
	}
	wantStatus(t, s.api(anna, "GET", s.statePath(gid, "default", 1, "/preview"), nil), 404, "not_found")
	if _, err := os.Stat(filepath.Join(s.svc.DataDir(), "states", s.anna.ID, gid, "default", "1.png")); !os.IsNotExist(err) {
		t.Fatalf("old preview file still there: %v", err)
	}
	// A state without a preview has none to serve; a missing state is 404.
	wantStatus(t, s.api(anna, "GET", s.statePath(gid, "default", 5, ""), nil), 404, "not_found")

	// Errors: unknown game, number out of range, bad metadata, empty state, bad preview, wrong part order.
	wantStatus(t, s.putState(anna, unknown, "default", 1, goodStateMeta, blob, nil), 404, "not_found")
	wantStatus(t, s.do("GET", "/api/v1/games/"+unknown+"/states", nil, opt{token: anna.tok}), 404, "not_found")
	data, ct = stateBody(t, goodStateMeta, blob, nil)
	wantStatus(t, s.do("PUT", s.statePath(gid, "default", 11, ""), nil, opt{token: anna.tok, data: data, ctype: ct, raw: true}), 404, "not_found")
	wantStatus(t, s.do("PUT", s.statePath(gid, "default", -1, ""), nil, opt{token: anna.tok, data: data, ctype: ct, raw: true}), 404, "not_found")
	data, ct = stateBody(t, `{"core_id":"Bad Core","core_version":"1"}`, blob, nil)
	wantStatus(t, s.do("PUT", s.statePath(gid, "default", 2, ""), nil, opt{token: anna.tok, data: data, ctype: ct, raw: true}), 400, "bad_request")
	wantStatus(t, s.putStateRaw(anna, gid, "default", 2, `{"core_id":"melonds"}`, blob, nil), 400, "bad_request")
	wantStatus(t, s.putStateRaw(anna, gid, "default", 2, `not json`, blob, nil), 400, "bad_request")
	wantStatus(t, s.putStateRaw(anna, gid, "default", 2, goodStateMeta, []byte{}, nil), 400, "bad_request")
	wantStatus(t, s.putStateRaw(anna, gid, "default", 2, goodStateMeta, blob, []byte("not a png")), 400, "invalid_image")
	data, ct = stateBody(t, "", nil, nil, "state", "meta")
	wantStatus(t, s.do("PUT", s.statePath(gid, "default", 2, ""), nil, opt{token: anna.tok, data: data, ctype: ct, raw: true}), 400, "bad_request")
	data, ct = stateBody(t, "", nil, nil, "meta", "state", "other")
	wantStatus(t, s.do("PUT", s.statePath(gid, "default", 2, ""), nil, opt{token: anna.tok, data: data, ctype: ct, raw: true}), 400, "bad_request")
	big := bytes.Repeat([]byte{1}, hub.MaxStatePreviewBytes+1)
	wantStatus(t, s.putState(anna, gid, "default", 2, goodStateMeta, blob, append(append([]byte{}, preview...), big...)), 413, "payload_too_large")
	// None of the refused uploads left a row behind.
	l = decode[stateListT](t, s.api(anna, "GET", "/api/v1/games/"+gid+"/states", nil))
	if len(l.States) != 3 {
		t.Fatalf("%+v", l.States)
	}

	// States are private: Bob sees none of Anna's and cannot read or delete them.
	l = decode[stateListT](t, s.api(bob, "GET", "/api/v1/games/"+gid+"/states", nil))
	if len(l.States) != 0 || l.UsedBytes != 0 {
		t.Fatalf("%+v", l)
	}
	wantStatus(t, s.api(bob, "GET", s.statePath(gid, "default", 1, ""), nil), 404, "not_found")
	wantStatus(t, s.api(bob, "GET", s.statePath(gid, "default", 1, "/preview"), nil), 404, "not_found")
	wantStatus(t, s.api(bob, "DELETE", s.statePath(gid, "default", 1, ""), nil), 404, "not_found")
	wantStatus(t, s.putState(bob, gid, "default", 1, goodStateMeta, []byte("bobs"), nil), 200, "")
	rec = s.api(anna, "GET", s.statePath(gid, "default", 1, ""), nil)
	if rec.Body.String() != "newer state" {
		t.Fatalf("Bob replaced Anna's state: %q", rec.Body.String())
	}

	// Delete removes row and files.
	wantStatus(t, s.api(anna, "DELETE", s.statePath(gid, "run-1a2b3c4d", 10, ""), nil), 204, "")
	wantStatus(t, s.api(anna, "DELETE", s.statePath(gid, "run-1a2b3c4d", 10, ""), nil), 404, "not_found")
	wantStatus(t, s.api(anna, "GET", s.statePath(gid, "run-1a2b3c4d", 10, ""), nil), 404, "not_found")
	if _, err := os.Stat(filepath.Join(s.svc.DataDir(), "states", s.anna.ID, gid, "run-1a2b3c4d")); !os.IsNotExist(err) {
		t.Fatalf("empty directory left: %v", err)
	}
}

func TestSaveStatesQuota(t *testing.T) {
	s := newSessEnv(t, nil)
	anna, bob := s.device(s.anna.ID, "Annas PC"), s.device(s.bob.ID, "Bobs PC")
	gid := s.game.ID
	ctx := context.Background()
	if q, err := s.svc.StateQuota(ctx); err != nil || q != 2<<30 {
		t.Fatalf("default quota %d %v", q, err)
	}
	if err := s.svc.SetStateQuota(ctx, hub.MinStateQuotaBytes-1); err == nil {
		t.Fatal("a quota below one state must be refused")
	}
	if err := s.svc.SetStateQuota(ctx, hub.MinStateQuotaBytes); err != nil {
		t.Fatal(err)
	}
	mib := func(n int) []byte { return bytes.Repeat([]byte{7}, n<<20) }

	wantStatus(t, s.putState(anna, gid, "default", 1, goodStateMeta, mib(40), nil), 200, "")
	// 40 + 40 MiB do not fit into 64.5 MiB; the refused upload changes nothing.
	wantStatus(t, s.putState(anna, gid, "default", 2, goodStateMeta, mib(40), nil), 413, "state_quota_exceeded")
	l := decode[stateListT](t, s.api(anna, "GET", "/api/v1/games/"+gid+"/states", nil))
	if len(l.States) != 1 || l.UsedBytes != 40<<20 || l.QuotaBytes != hub.MinStateQuotaBytes {
		t.Fatalf("%+v", l)
	}
	// The state that is replaced counts as freed: 60 MiB instead of 40 MiB fits.
	wantStatus(t, s.putState(anna, gid, "default", 1, goodStateMeta, mib(60), nil), 200, "")
	wantStatus(t, s.putState(anna, gid, "default", 2, goodStateMeta, mib(5), nil), 413, "state_quota_exceeded")
	wantStatus(t, s.putState(anna, gid, "default", 2, goodStateMeta, mib(4), nil), 200, "")
	// The preview counts too: 400 KiB alone fit into the 512 KiB that are left, with a 300 KiB preview they do not.
	bigPreview := append(testPNG(t, 8, 8), make([]byte, 300<<10)...) // data after the PNG is not looked at
	state400 := bytes.Repeat([]byte{9}, 400<<10)
	wantStatus(t, s.putState(anna, gid, "default", 3, goodStateMeta, state400, bigPreview), 413, "state_quota_exceeded")
	wantStatus(t, s.putState(anna, gid, "default", 3, goodStateMeta, state400, nil), 200, "")
	wantStatus(t, s.api(anna, "DELETE", s.statePath(gid, "default", 3, ""), nil), 204, "")
	// The quota is per user.
	wantStatus(t, s.putState(bob, gid, "default", 1, goodStateMeta, mib(50), nil), 200, "")
	// Deleting frees space.
	wantStatus(t, s.api(anna, "DELETE", s.statePath(gid, "default", 1, ""), nil), 204, "")
	wantStatus(t, s.putState(anna, gid, "default", 2, goodStateMeta, mib(50), nil), 200, "")

	// A state over 64 MiB is refused by size, not by quota.
	if err := s.svc.SetStateQuota(ctx, 4<<30); err != nil {
		t.Fatal(err)
	}
	wantStatus(t, s.putState(anna, gid, "default", 4, goodStateMeta, bytes.Repeat([]byte{1}, hub.MaxStateBytes+1), nil), 413, "payload_too_large")
	wantStatus(t, s.putState(anna, gid, "default", 4, goodStateMeta, bytes.Repeat([]byte{1}, hub.MaxStateBytes), nil), 200, "")
}

func TestAdminStateQuotaSetting(t *testing.T) {
	s, _, adm, _ := newPartBEnv(t)
	type settingsT struct {
		StateQuotaBytes int64 `json:"state_quota_bytes"`
	}
	rec := s.do("GET", "/api/v1/admin/settings", nil, opt{token: adm.tok})
	wantStatus(t, rec, 200, "")
	if q := decode[settingsT](t, rec).StateQuotaBytes; q != hub.DefaultStateQuotaBytes {
		t.Fatalf("default %d", q)
	}
	wantStatus(t, s.do("PATCH", "/api/v1/admin/settings", map[string]any{"state_quota_bytes": 1 << 20}, opt{token: adm.tok}), 400, "bad_request")
	rec = s.do("PATCH", "/api/v1/admin/settings", map[string]any{"state_quota_bytes": 5 << 30}, opt{token: adm.tok})
	wantStatus(t, rec, 200, "")
	if q := decode[settingsT](t, rec).StateQuotaBytes; q != 5<<30 {
		t.Fatalf("set %d", q)
	}
	if q, _ := s.svc.StateQuota(context.Background()); q != 5<<30 {
		t.Fatalf("service %d", q)
	}
}

func TestSaveStatesStorageAndCleanup(t *testing.T) {
	s := newSessEnv(t, nil)
	anna := s.device(s.anna.ID, "Annas PC")
	gid := s.game.ID
	ctx := context.Background()
	preview := testPNG(t, 8, 8)
	wantStatus(t, s.putState(anna, gid, "default", 1, goodStateMeta, []byte("0123456789"), preview), 200, "")
	wantStatus(t, s.putState(anna, gid, "default", 2, goodStateMeta, []byte("abcde"), nil), 200, "")

	ov, err := s.svc.StorageOverview(ctx)
	if err != nil {
		t.Fatal(err)
	}
	var kind hub.StorageKind
	for _, k := range ov.Kinds {
		if k.ID == hub.StorageStates {
			kind = k
		}
	}
	if kind.Count != 3 || kind.Bytes != int64(10+5+len(preview)) {
		t.Fatalf("states kind %+v", kind)
	}
	if c := ov.Cleanup[3]; c.ID != hub.CleanupDeletedGameStates || c.Count != 0 {
		t.Fatalf("nothing to clean yet: %+v", c)
	}

	// Deleting the game keeps the states (as with saves) and offers the clean-up.
	if err := s.svc.DeleteGame(ctx, gid); err != nil {
		t.Fatal(err)
	}
	wantStatus(t, s.api(anna, "GET", "/api/v1/games/"+gid+"/states", nil), 404, "not_found")
	ov, err = s.svc.StorageOverview(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if c := ov.Cleanup[3]; c.ID != hub.CleanupDeletedGameStates || c.Count != 2 || c.Bytes != int64(10+5+len(preview)) {
		t.Fatalf("clean-up offer: %+v", c)
	}
	res, err := s.svc.StorageCleanup(ctx, hub.CleanupDeletedGameStates)
	if err != nil || res.Removed != 2 || res.FreedBytes != int64(10+5+len(preview)) {
		t.Fatalf("%+v %v", res, err)
	}
	if _, err := os.Stat(filepath.Join(s.svc.DataDir(), "states", s.anna.ID)); !os.IsNotExist(err) {
		t.Fatalf("files left: %v", err)
	}
	ov, _ = s.svc.StorageOverview(ctx)
	if ov.Cleanup[3].Count != 0 {
		t.Fatalf("%+v", ov.Cleanup[3])
	}
}

func TestSaveStatesGoWithTheUser(t *testing.T) {
	s := newSessEnv(t, nil)
	anna := s.device(s.anna.ID, "Annas PC")
	wantStatus(t, s.putState(anna, s.game.ID, "default", 1, goodStateMeta, []byte("state"), nil), 200, "")
	dir := filepath.Join(s.svc.DataDir(), "states", s.anna.ID)
	if _, err := os.Stat(dir); err != nil {
		t.Fatal(err)
	}
	if err := s.svc.DeleteUser(context.Background(), s.anna.ID, s.admin.ID); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(dir); !os.IsNotExist(err) {
		t.Fatalf("state files of a removed user: %v", err)
	}
}
