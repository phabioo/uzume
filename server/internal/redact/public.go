package redact

import (
	"regexp"
	"sort"
	"strconv"
	"strings"
	"unicode"
	"unicode/utf8"
)

// Public text is meant for a public GitHub issue (ADR 0035 D3). It is stricter than Redact: besides the secrets it
// removes everything that identifies an instance or a person.

// Known are the strings of one server that must never reach a public issue.
type Known struct {
	Names []string // user names, display names and device names
	Games []string // game titles and ROM file names
}

const minKnownLen = 2

// publicEarlyRules run first: URLs and home paths contain account names and must be cut as a whole.
var publicEarlyRules = []struct {
	re   *regexp.Regexp
	repl func(string) string
}{
	// Home and user paths: the account name goes, the rest of the path stays.
	{regexp.MustCompile(`(?i)\b[a-z]:[\\/]users[\\/][^\\/\s"'<>]+`), fixed("<home>")},
	{regexp.MustCompile(`(?i)(?:^|[^A-Za-z0-9_.-])(/home/[^/\s"'<>:]+|/Users/[^/\s"'<>:]+)`), func(m string) string {
		i := strings.IndexByte(m, '/')
		return m[:i] + "<home>"
	}},
}

// publicLateRules run after the known names and titles are gone.
var publicLateRules = []struct {
	re   *regexp.Regexp
	repl func(string) string
}{
	// Invite and pairing codes (UZ-XXXX-XXXX), and a code named as such.
	{regexp.MustCompile(`(?i)\buz-[a-z0-9]{4}-[a-z0-9]{4}\b`), fixed("<code>")},
	{regexp.MustCompile(`(?i)\b((?:invite|pairing|setup|device)[ _-]?code\W{0,3})[a-z0-9-]{4,}`), func(m string) string {
		key := publicCodeKey.FindString(m)
		if strings.EqualFold(m[len(key):], "redacted") {
			return m // already removed by Redact
		}
		return key + "<code>"
	}},
	// IPv4 addresses (with port).
	{regexp.MustCompile(`\b\d{1,3}(?:\.\d{1,3}){3}(?::\d{1,5})?\b`), fixed("<ip>")},
}

var urlRe = regexp.MustCompile(`(?i)[a-z][a-z0-9+.-]*://[^\s<>"'\x60)\]]+`)

var (
	githubURL     = regexp.MustCompile(`(?i)^https://github\.com(?:[/?#].*)?$`)
	publicCodeKey = regexp.MustCompile(`(?i)^((?:invite|pairing|setup|device)[ _-]?code\W{0,3})`)
	ipv6          = regexp.MustCompile(`(?i)(^|[^0-9a-z_:])\[?((?:[0-9a-f]{0,4}:){2,7}[0-9a-f]{0,4})\]?(?::\d{1,5})?`)
	hexDigit      = regexp.MustCompile(`(?i)[0-9a-f]`)
	// Host names (two or more labels), a port behind them goes along. File names keep their extension.
	hostName = regexp.MustCompile(`(?i)\b(?:[a-z0-9-]+\.)+([a-z]{2,})\b(?::\d{1,5})?`)
	fileExts = map[string]bool{"log": true, "txt": true, "json": true, "lua": true, "dll": true, "so": true, "dylib": true, "exe": true,
		"zip": true, "uzplugin": true, "cpp": true, "h": true, "go": true, "qml": true, "sav": true, "srm": true, "png": true,
		"jpg": true, "cfg": true, "ini": true, "xml": true, "md": true, "js": true}
)

func fixed(s string) func(string) string { return func(string) string { return s } }

// Public returns s with the secrets of Redact removed and, in addition, host names (github.com stays), IP addresses,
// ports behind hosts, URLs other than https://github.com/..., invite and pairing codes, home paths, the names and
// game titles in known and ROM file names. The placeholders are <host>, <ip>, <url>, <code>, <home>, <name>, <game>.
func Public(s string, known Known) string {
	s = RedactString(s)
	// URLs: only https://github.com/... stays; it is parked behind a token so that no later rule touches it.
	var kept []string
	s = urlRe.ReplaceAllStringFunc(s, func(m string) string {
		if githubURL.MatchString(m) {
			kept = append(kept, m)
			return "\x00" + strconv.Itoa(len(kept)-1) + "\x00"
		}
		return "<url>"
	})
	for _, r := range publicEarlyRules {
		s = r.re.ReplaceAllStringFunc(s, r.repl)
	}
	s = replaceWords(s, known.Games, "<game>")
	s = replaceWords(s, known.Names, "<name>")
	for _, r := range publicLateRules {
		s = r.re.ReplaceAllStringFunc(s, r.repl)
	}
	s = ipv6.ReplaceAllStringFunc(s, func(m string) string {
		sub := ipv6.FindStringSubmatch(m)
		if !hexDigit.MatchString(sub[2]) {
			return m
		}
		return sub[1] + "<ip>"
	})
	s = hostName.ReplaceAllStringFunc(s, func(m string) string {
		host, _, _ := strings.Cut(m, ":")
		if strings.EqualFold(host, "github.com") || fileExts[strings.ToLower(host[strings.LastIndexByte(host, '.')+1:])] {
			return m
		}
		return "<host>"
	})
	for i, u := range kept {
		s = strings.Replace(s, "\x00"+strconv.Itoa(i)+"\x00", u, 1)
	}
	return s
}

// replaceWords replaces every case-insensitive occurrence of a term that stands on its own (not inside a longer
// word). Terms shorter than minKnownLen are skipped, longer terms win over shorter ones.
func replaceWords(s string, terms []string, repl string) string {
	var list []string
	seen := map[string]bool{}
	for _, t := range terms {
		t = strings.TrimSpace(t)
		if utf8.RuneCountInString(t) < minKnownLen || seen[strings.ToLower(t)] {
			continue
		}
		seen[strings.ToLower(t)] = true
		list = append(list, regexp.QuoteMeta(t))
	}
	if len(list) == 0 {
		return s
	}
	sort.Slice(list, func(i, j int) bool { return len(list[i]) > len(list[j]) })
	re := regexp.MustCompile(`(?i)` + strings.Join(list, "|"))
	var b strings.Builder
	last := 0
	for _, loc := range re.FindAllStringIndex(s, -1) {
		start, end := loc[0], loc[1]
		if start < last {
			continue
		}
		before, _ := utf8.DecodeLastRuneInString(s[:start])
		after, _ := utf8.DecodeRuneInString(s[end:])
		if (start > 0 && isWordRune(before)) || (end < len(s) && isWordRune(after)) {
			continue
		}
		b.WriteString(s[last:start])
		b.WriteString(repl)
		last = end
	}
	b.WriteString(s[last:])
	return b.String()
}

func isWordRune(r rune) bool { return unicode.IsLetter(r) || unicode.IsDigit(r) || r == '_' }
