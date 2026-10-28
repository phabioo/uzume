// Package errfp computes the fingerprint of an error event (ADR 0035 D1). The rules are shared with the app
// (C++); the shared test vectors are protocol/fixtures/error-fingerprint.json. Change both sides together.
package errfp

import (
	"crypto/sha256"
	"encoding/hex"
	"regexp"
	"strings"
	"unicode/utf8"
)

// MaxNormalized is the length cap of the normalized message in bytes.
const MaxNormalized = 300

var rules = []struct {
	re   *regexp.Regexp
	repl string
}{
	{regexp.MustCompile(`[a-z][a-z0-9+.-]*://\S+`), "<url>"},
	{regexp.MustCompile(`[a-z]:[\\/][^\s"']*`), "<path>"},
	{regexp.MustCompile(`(?:~|/)[^\s"':]*/[^\s"':]*`), "<path>"},
	{regexp.MustCompile(`\b\d{1,3}(?:\.\d{1,3}){3}(?::\d+)?\b`), "<ip>"},
	{regexp.MustCompile(`\b(?:[0-9a-f]{0,4}:){2,7}[0-9a-f]{0,4}\b`), "<ip>"},
	{regexp.MustCompile(`\b(?:0x)?[0-9a-f]{6,}\b`), "<hex>"},
	{regexp.MustCompile(`\d+`), "<n>"},
	{regexp.MustCompile(`\s+`), " "},
}

// lower lowercases ASCII letters only, so that every implementation agrees on non-ASCII text.
func lower(s string) string {
	b := []byte(s)
	for i, c := range b {
		if c >= 'A' && c <= 'Z' {
			b[i] = c + 'a' - 'A'
		}
	}
	return string(b)
}

// Normalize replaces numbers, paths, addresses and ids of a message by placeholders (see the rules in ADR 0035).
func Normalize(message string) string {
	s := lower(strings.ToValidUTF8(message, "\uFFFD"))
	for _, r := range rules {
		s = r.re.ReplaceAllString(s, r.repl)
	}
	s = strings.Trim(s, " ")
	if len(s) > MaxNormalized {
		n := MaxNormalized
		for n > 0 && !utf8.RuneStart(s[n]) {
			n--
		}
		s = s[:n]
	}
	return s
}

// Fingerprint returns "uz-" and the first 12 hex digits of SHA-256(kind \n component \n normalized message).
func Fingerprint(kind, component, message string) string {
	sum := sha256.Sum256([]byte(kind + "\n" + component + "\n" + Normalize(message)))
	return "uz-" + hex.EncodeToString(sum[:])[:12]
}
