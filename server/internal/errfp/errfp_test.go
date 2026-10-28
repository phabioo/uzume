package errfp

import (
	"encoding/json"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
	"unicode/utf8"
)

type vector struct {
	Kind, Component, Message, Normalized, Fingerprint string
}

func TestFixtureVectors(t *testing.T) {
	raw, err := os.ReadFile(filepath.Join("..", "..", "..", "protocol", "fixtures", "error-fingerprint.json"))
	if err != nil {
		t.Fatal(err)
	}
	var vs []vector
	if err := json.Unmarshal(raw, &vs); err != nil {
		t.Fatal(err)
	}
	if len(vs) < 8 {
		t.Fatalf("fixture has %d vectors, want at least 8", len(vs))
	}
	for i, v := range vs {
		if got := Normalize(v.Message); got != v.Normalized {
			t.Errorf("#%d normalize(%q) = %q, want %q", i, v.Message, got, v.Normalized)
		}
		if got := Fingerprint(v.Kind, v.Component, v.Message); got != v.Fingerprint {
			t.Errorf("#%d fingerprint = %s, want %s", i, got, v.Fingerprint)
		}
	}
}

func TestRules(t *testing.T) {
	for in, want := range map[string]string{
		"Fetch https://Hub.example.org:8443/api failed": "fetch <url> failed",
		`open C:\Users\Anna\x.sav: denied`:              "open <path> denied",
		"read /home/anna/uzume/a.cfg failed":            "read <path> failed",
		"no slash /root here":                           "no slash /root here", // one leading slash only: not a path
		"connect 192.0.2.17:9000 refused":               "connect <ip> refused",
		"peer 2001:db8::1 gone":                         "peer <ip> gone",
		"id 3F2504E0-4F89-11D3 and 0xDEADBEEF":          "id <hex>-<n>f<n>-<n>d<n> and <hex>",
		"frame 12 of 600":                               "frame <n> of <n>",
		"  a \t b\n\nc  ":                               "a b c",
	} {
		if got := Normalize(in); got != want {
			t.Errorf("Normalize(%q) = %q, want %q", in, got, want)
		}
	}
}

func TestCutAtUTF8Boundary(t *testing.T) {
	in := strings.Repeat("q", 299) + "é" + "tail" // é spans bytes 299 and 300
	got := Normalize(in)
	if !utf8.ValidString(got) || len(got) != 299 {
		t.Fatalf("len %d valid %v", len(got), utf8.ValidString(got))
	}
	if n := Normalize(strings.Repeat("q", 400)); len(n) != MaxNormalized {
		t.Fatalf("len %d", len(n))
	}
}

func TestFingerprintFormat(t *testing.T) {
	fp := Fingerprint("app_crash", "app", "boom 1")
	if !regexp.MustCompile(`^uz-[0-9a-f]{12}$`).MatchString(fp) {
		t.Fatal(fp)
	}
	if fp != Fingerprint("app_crash", "app", "BOOM 22") {
		t.Fatal("numbers and case must not change the fingerprint")
	}
	if fp == Fingerprint("core_failure", "app", "boom 1") || fp == Fingerprint("app_crash", "server", "boom 1") {
		t.Fatal("kind and component are part of the fingerprint")
	}
}
