package httpapi

import (
	"fmt"
	"net/http/httptest"
	"strings"
	"testing"
)

// The cheats_used marker on saves and the longer snapshot labels (0.20 package 2, feature run_rules_v1).

type cheatsVersion struct {
	Version    int     `json:"version"`
	Reason     string  `json:"reason"`
	Label      *string `json:"label"`
	CheatsUsed *bool   `json:"cheats_used"`
}

func (s *sessEnv) putSaveCheats(d dev, slot string, base int, data []byte, reason, cheats string) *httptest.ResponseRecorder {
	s.t.Helper()
	h := map[string]string{"X-Uzume-Base-Revision": fmt.Sprint(base), "X-Uzume-Content-SHA256": hexSHA(data), "X-Uzume-Sync-Reason": reason}
	if cheats != "" {
		h["X-Uzume-Cheats-Used"] = cheats
	}
	return s.do("PUT", s.savePath(slot, ""), nil, opt{token: d.tok, data: data, ctype: "application/octet-stream", header: h})
}

func (v cheatsVersion) used() bool { return v.CheatsUsed != nil && *v.CheatsUsed }

func TestSaveCheatsUsedMarker(t *testing.T) {
	s := newSessEnv(t, nil)
	d1, d2 := s.device(s.admin.ID, "Desktop"), s.device(s.admin.ID, "Laptop")
	current := func() bool {
		t.Helper()
		sl := decode[struct {
			Current struct {
				CheatsUsed *bool `json:"cheats_used"`
			} `json:"current"`
		}](t, s.api(d1, "GET", s.savePath("default", ""), nil))
		return sl.Current.CheatsUsed != nil && *sl.Current.CheatsUsed
	}
	history := func() []cheatsVersion {
		t.Helper()
		return decode[struct{ Versions []cheatsVersion }](t, s.api(d1, "GET", s.savePath("default", "/history"), nil)).Versions
	}

	// Without the header the marker is absent (false); the old clients keep working.
	wantStatus(t, s.putSaveCheats(d1, "default", 0, []byte("one"), "checkpoint", ""), 200, "")
	if current() {
		t.Fatal("no header means no cheats")
	}
	// A checkpoint made with cheats is marked, and so is the session-end version made from it.
	wantStatus(t, s.putSaveCheats(d1, "default", 1, []byte("two"), "checkpoint", "true"), 200, "")
	if !current() {
		t.Fatal("cheats_used missing on the checkpoint")
	}
	wantStatus(t, s.putSaveCheats(d1, "default", 2, []byte("three"), "final_session_end", "true"), 200, "")
	h := history()
	if len(h) != 1 || h[0].Reason != "session_end" || !h[0].used() {
		t.Fatalf("%+v", h)
	}
	// A later upload without cheats clears the checkpoint marker; the version keeps its own.
	wantStatus(t, s.putSaveCheats(d1, "default", 3, []byte("four"), "final_session_end", "false"), 200, "")
	if current() {
		t.Fatal("the new checkpoint was made without cheats")
	}
	h = history()
	if len(h) != 2 || h[0].used() || !h[1].used() {
		t.Fatalf("%+v", h)
	}

	// A manual snapshot copies the marker of the checkpoint; so does the version captured on a device change.
	wantStatus(t, s.putSaveCheats(d1, "default", 4, []byte("five"), "checkpoint", "true"), 200, "")
	rec := s.api(d1, "POST", s.savePath("default", "/snapshots"), map[string]any{"label": "Before cheats"})
	wantStatus(t, rec, 201, "")
	if v := decode[cheatsVersion](t, rec); v.Label == nil || *v.Label != "Before cheats" || !v.used() {
		t.Fatalf("%+v", v)
	}
	wantStatus(t, s.putSaveCheats(d2, "default", 5, []byte("six"), "checkpoint", ""), 200, "")
	h = history() // newest first
	if len(h) == 0 || h[0].Reason != "manual_snapshot" || !h[0].used() {
		t.Fatalf("%+v", h)
	}

	// Restoring a marked version marks the new checkpoint again.
	wantStatus(t, s.api(d1, "POST", s.savePath("default", "/history/1/restore"), map[string]any{"expected_revision": 6}), 200, "")
	if !current() {
		t.Fatal("a restored version keeps its marker")
	}
}

func TestSnapshotLabelsFromPlugins(t *testing.T) {
	s := newSessEnv(t, nil)
	d := s.device(s.admin.ID, "Desktop")
	wantStatus(t, s.putSave(d, "default", 0, []byte("one"), "checkpoint"), 200, "")
	label := func(l string) *httptest.ResponseRecorder {
		return s.do("POST", s.savePath("default", "/snapshots"), map[string]any{"label": l}, opt{token: d.tok, raw: true})
	}
	for _, l := range []string{"Before cheats", "Before memory changes", "Pokémon Editor: Before editing the team",
		"SoulLink: " + strings.Repeat("ä", 110)} {
		rec := label(l)
		wantStatus(t, rec, 201, "")
		if v := decode[cheatsVersion](t, rec); v.Label == nil || *v.Label != l || v.used() {
			t.Fatalf("%q: %+v", l, v)
		}
	}
	wantStatus(t, label(strings.Repeat("x", 120)), 201, "")
	wantStatus(t, label(strings.Repeat("x", 121)), 400, "bad_request")
	h := decode[struct{ Versions []cheatsVersion }](t, s.api(d, "GET", s.savePath("default", "/history"), nil)).Versions
	if len(h) != 5 {
		t.Fatalf("%+v", h)
	}
}
