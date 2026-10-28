package version

import "testing"

func TestStringDefault(t *testing.T) {
	if got := String(); got != "dev" {
		t.Fatalf("String() = %q, want %q", got, "dev")
	}
}

func TestStringOverride(t *testing.T) {
	old := Version
	t.Cleanup(func() { Version = old })
	Version = "1.2.3"
	if got := String(); got != "1.2.3" {
		t.Fatalf("String() = %q, want %q", got, "1.2.3")
	}
}

func TestChannelAndCommitDefaults(t *testing.T) {
	if Channel != "dev" || Commit != "" {
		t.Fatalf("Channel=%q Commit=%q", Channel, Commit)
	}
}

func TestDisplay(t *testing.T) {
	for in, want := range map[string]string{
		"21.1.0":       "21.1",
		"21.37.0":      "21.37",
		"21.0.0-dev.5": "21.0-dev.5",
		"0.20.15":      "0.20.15",
		"21.1.10":      "21.1.10",
		"21.1.0.1":     "21.1.0.1",
		"dev":          "dev",
		"":             "",
	} {
		if got := Display(in); got != want {
			t.Errorf("Display(%q) = %q, want %q", in, got, want)
		}
	}
}
