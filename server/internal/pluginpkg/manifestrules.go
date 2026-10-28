package pluginpkg

import (
	"encoding/json"
	"regexp"
	"strconv"
	"strings"
	"unicode/utf8"

	"github.com/phabioo/uzume/server/internal/updates"
)

// This file mirrors the manifest rules of the app (client/plugins/manifest.cpp): permission whitelist, the
// `requires` range grammar and the `ui` field. The shared corpus in testdata/manifests/ keeps both sides in step.

// supportedPermissions are the permissions the app can grant today.
var supportedPermissions = map[string]bool{
	"memory.read":   true,
	"rom.read":      true,
	"ui":            true,
	"run.state":     true,
	"video.capture": true,
	// 0.20 package 5, one line per permission
	"memory.write": true,
	"cheats":       true,
	"store":        true,
	// 0.21 Randomizer (ADR 0031 D5); the app allows it only for signed official plugins or in Developer mode
	"variants.build": true,
}

func permissionError(p string) error {
	if supportedPermissions[p] {
		return nil
	}
	if p == "tool.exec" {
		return invalid("permission %s is not available yet", p)
	}
	return invalid("unknown permission '%s'", p)
}

// looseVersion pads "1", "1.2" and "1.2.3[-pre]" like the app; pre-release only on full versions.
func looseVersion(s string) bool {
	core, _, hasPre := strings.Cut(s, "-")
	parts := strings.Split(core, ".")
	if len(parts) > 3 || (len(parts) < 3 && hasPre) {
		return false
	}
	var n [3]int64
	for i, p := range parts {
		v, err := strconv.ParseInt(p, 10, 64)
		if err != nil || v < 0 || len(p) > 9 {
			return false
		}
		n[i] = v
	}
	full := strconv.FormatInt(n[0], 10) + "." + strconv.FormatInt(n[1], 10) + "." + strconv.FormatInt(n[2], 10)
	if len(parts) == 3 && hasPre {
		full += s[len(core):]
	}
	return updates.ValidSemVer(full)
}

// validRange reports whether s is a version range the app accepts: "*" (or empty), ^x[.y[.z]], ~x[.y[.z]],
// >=x[.y[.z]] or an exact version.
func validRange(s string) bool {
	t := strings.TrimSpace(s)
	if t == "" || t == "*" {
		return true
	}
	switch {
	case strings.HasPrefix(t, "^"), strings.HasPrefix(t, "~"):
		return looseVersion(strings.TrimSpace(t[1:]))
	case strings.HasPrefix(t, ">="):
		return looseVersion(strings.TrimSpace(t[2:]))
	}
	return looseVersion(t)
}

// issuesURLRe is the form of the optional manifest field issues_url.
var issuesURLRe = regexp.MustCompile(`^https://github\.com/[A-Za-z0-9-]+/[A-Za-z0-9._-]+$`)

var (
	uiHotkeyIDRe = regexp.MustCompile(`^[a-z0-9-]{1,32}$`)
	uiKnownKeys  = map[string]bool{"panel": true, "hud": true, "tile": true, "screen": true, "detail": true, "run": true, "hotkeys": true}
)

func isNullJSON(v json.RawMessage) bool { return string(v) == "null" }

func validateUITitle(v json.RawMessage, field string) error {
	var o map[string]json.RawMessage
	if json.Unmarshal(v, &o) != nil || o == nil {
		return invalid("ui.%s must be an object like {\"title\": \"...\"}", field)
	}
	for k := range o {
		if k != "title" {
			return invalid("ui.%s: unknown key '%s'", field, k)
		}
	}
	var title string
	if json.Unmarshal(o["title"], &title) != nil {
		return invalid("ui.%s.title is required and must be a string", field)
	}
	if n := utf8.RuneCountInString(title); n < 1 || n > 40 {
		return invalid("ui.%s.title must be 1 to 40 characters", field)
	}
	return nil
}

// validateUI checks the (non-null) value of "ui" by the app rules.
func validateUI(raw json.RawMessage) error {
	var o map[string]json.RawMessage
	if json.Unmarshal(raw, &o) != nil || o == nil {
		return invalid("manifest field 'ui' must be an object")
	}
	for k := range o {
		if !uiKnownKeys[k] {
			return invalid("ui: unknown key '%s'", k)
		}
	}
	for _, k := range []string{"panel", "screen", "run"} {
		if v, ok := o[k]; ok && !isNullJSON(v) {
			if err := validateUITitle(v, k); err != nil {
				return err
			}
		}
	}
	for _, k := range []string{"hud", "tile", "detail"} {
		if v, ok := o[k]; ok && !isNullJSON(v) {
			var b bool
			if json.Unmarshal(v, &b) != nil {
				return invalid("ui.%s must be true or false", k)
			}
		}
	}
	hv, ok := o["hotkeys"]
	if !ok || isNullJSON(hv) {
		return nil
	}
	var arr []json.RawMessage
	if json.Unmarshal(hv, &arr) != nil {
		return invalid("ui.hotkeys must be an array")
	}
	if len(arr) > 16 {
		return invalid("ui.hotkeys: at most 16 hotkeys")
	}
	seen := map[string]bool{}
	for i, e := range arr {
		at := "ui.hotkeys[" + strconv.Itoa(i) + "]"
		var ho map[string]json.RawMessage
		if json.Unmarshal(e, &ho) != nil || ho == nil {
			return invalid("%s must be an object with id, label and optional key", at)
		}
		for k := range ho {
			if k != "id" && k != "label" && k != "key" {
				return invalid("%s: unknown key '%s'", at, k)
			}
		}
		var id, label string
		if json.Unmarshal(ho["id"], &id) != nil || !uiHotkeyIDRe.MatchString(id) {
			return invalid("%s.id must match [a-z0-9-] and be 1 to 32 characters", at)
		}
		if seen[id] {
			return invalid("%s.id '%s' is not unique", at, id)
		}
		seen[id] = true
		if json.Unmarshal(ho["label"], &label) != nil || label == "" || utf8.RuneCountInString(label) > 60 {
			return invalid("%s.label must be 1 to 60 characters", at)
		}
		if kv, ok := ho["key"]; ok && !isNullJSON(kv) {
			var key string
			if json.Unmarshal(kv, &key) != nil || strings.TrimSpace(key) == "" || utf8.RuneCountInString(key) > 40 || strings.Contains(key, ",") {
				return invalid("%s.key must be one key combination such as Ctrl+H", at)
			}
		}
	}
	return nil
}

// RangeMatches reports whether version satisfies a `requires` range (same grammar and bounds as the app's
// VersionRange::matches). An invalid range or version never matches.
func RangeMatches(rng, version string) bool {
	v, err := updates.ParseSemVer(version)
	if err != nil {
		return false
	}
	t := strings.TrimSpace(rng)
	if t == "" || t == "*" {
		return true
	}
	loose := func(s string) (updates.SemVer, int, bool) {
		s = strings.TrimSpace(s)
		if !looseVersion(s) {
			return updates.SemVer{}, 0, false
		}
		core, pre, _ := strings.Cut(s, "-")
		parts := strings.Split(core, ".")
		n := [3]string{"0", "0", "0"}
		for i, p := range parts {
			n[i] = strconv.FormatInt(mustInt(p), 10)
		}
		full := n[0] + "." + n[1] + "." + n[2]
		if pre != "" {
			full += "-" + pre
		}
		sv, err := updates.ParseSemVer(full)
		return sv, len(parts), err == nil
	}
	switch {
	case strings.HasPrefix(t, "^"), strings.HasPrefix(t, "~"):
		lo, parts, ok := loose(t[1:])
		if !ok {
			return false
		}
		maj, min, pat := mustInt(lo.Major), mustInt(lo.Minor), mustInt(lo.Patch)
		var hi [3]int64
		if t[0] == '^' {
			switch {
			case maj > 0 || parts == 1:
				hi = [3]int64{maj + 1, 0, 0}
			case min > 0 || parts == 2:
				hi = [3]int64{0, min + 1, 0}
			default:
				hi = [3]int64{0, 0, pat + 1}
			}
		} else if parts == 1 {
			hi = [3]int64{maj + 1, 0, 0}
		} else {
			hi = [3]int64{maj, min + 1, 0}
		}
		hv := updates.SemVer{Major: strconv.FormatInt(hi[0], 10), Minor: strconv.FormatInt(hi[1], 10), Patch: strconv.FormatInt(hi[2], 10), Pre: []string{"0"}}
		return v.Compare(lo) >= 0 && v.Compare(hv) < 0
	case strings.HasPrefix(t, ">="):
		lo, _, ok := loose(t[2:])
		return ok && v.Compare(lo) >= 0
	}
	lo, _, ok := loose(t)
	return ok && v.Compare(lo) == 0
}

func mustInt(s string) int64 { n, _ := strconv.ParseInt(s, 10, 64); return n }
