package pluginpkg

import (
	"strings"
	"testing"
)

func TestManifestRunUpdateFrom(t *testing.T) {
	mk := func(extra string) []byte {
		return []byte(`{"api":1,"id":"net.example.x","name":"X","version":"1.0.0","entry":"main.lua"` + extra + `}`)
	}
	m, err := ParseManifest(mk(`,"run_update_from":"0.9.0"`))
	if err != nil || m.RunUpdateFrom != "0.9.0" {
		t.Fatalf("%+v %v", m, err)
	}
	if m, err = ParseManifest(mk(``)); err != nil || m.RunUpdateFrom != "" {
		t.Fatalf("%+v %v", m, err)
	}
	for _, bad := range []string{`"run_update_from":"1.0"`, `"run_update_from":"v1.0.0"`, `"run_update_from":5`} {
		if _, err := ParseManifest(mk("," + bad)); err == nil || !strings.Contains(err.Error(), "run_update_from") {
			t.Fatalf("%s: %v", bad, err)
		}
	}
}

func TestRangeMatches(t *testing.T) {
	for _, c := range []struct {
		rng, v string
		want   bool
	}{
		{"*", "3.0.0", true}, {"", "1.0.0", true},
		{"^1.2", "1.2.0", true}, {"^1.2", "1.9.9", true}, {"^1.2", "2.0.0", false}, {"^1.2", "1.1.9", false},
		{"^0.2", "0.2.5", true}, {"^0.2", "0.3.0", false}, {"^0.0.3", "0.0.3", true}, {"^0.0.3", "0.0.4", false},
		{"~1.2", "1.2.9", true}, {"~1.2", "1.3.0", false}, {"~1", "1.9.0", true}, {"~1", "2.0.0", false},
		{">=1.0", "1.0.0", true}, {">=1.0", "0.9.9", false}, {">=1.0", "7.0.0", true},
		{"1.2.3", "1.2.3", true}, {"1.2.3", "1.2.4", false}, {"1.2", "1.2.0", true},
		{"^1.0", "2.0.0-beta.1", false}, {"^1.0", "not-a-version", false}, {"junk", "1.0.0", false},
	} {
		if got := RangeMatches(c.rng, c.v); got != c.want {
			t.Errorf("RangeMatches(%q, %q) = %v, want %v", c.rng, c.v, got, c.want)
		}
	}
}
