// Package version provides the hub version, channel and commit, settable via -ldflags.
package version

import (
	"regexp"
	"strconv"
)

// Build information, set at build time:
//
//	-ldflags "-X github.com/phabioo/uzume/server/internal/version.Version=1.2.3
//	          -X github.com/phabioo/uzume/server/internal/version.Channel=stable
//	          -X github.com/phabioo/uzume/server/internal/version.Commit=<sha>"
var (
	Version = "dev"
	// Channel is the compiled-in default update channel: stable, beta or dev.
	Channel = "dev"
	// Commit is the source commit (may be empty).
	Commit = ""
)

// String returns the current hub version.
func String() string { return Version }

// Display returns v for people (ADR 0036): from release 21 on a third field that is exactly 0 is dropped, "21.1.0" ->
// "21.1" and "21.1.0-dev.5" -> "21.1-dev.5". Anything else ("0.17.0", "0.20.15", "dev") is returned unchanged. Comparisons, tags, file
// names, JSON output and the update index keep the full string.
func Display(v string) string {
	m := displayRe.FindStringSubmatch(v)
	if m == nil {
		return v
	}
	if major, err := strconv.Atoi(m[2]); err != nil || major < 21 {
		return v
	}
	return m[1] + m[3]
}

var displayRe = regexp.MustCompile(`^(([0-9]+)\.[0-9]+)\.0((?:[-+].*)?)$`)
