package hub

import (
	"bytes"
	"encoding/xml"
	"regexp"
	"strconv"
	"strings"
)

// Parsers for the cheat database (0.20 package 3): RetroArch .cht files, No-Intro .dat files (clrmamepro text or
// Logiqx XML) and the guess of the code format.

// Cheat is one entry of a cheat list.
type Cheat struct {
	Name        string
	Code        string
	FormatGuess string
}

// Cheat code formats (the contract list of the plugin API).
const (
	CheatRaw       = "raw"
	CheatGSA       = "gsa"
	CheatCB        = "cb"
	CheatARDS      = "ar_ds"
	CheatARGBA     = "ar_gba"
	CheatGameGenie = "game_genie"
	CheatGateway   = "gateway"
)

const (
	maxCheatsPerList = 5000
	maxCheatCodeLen  = 16 << 10
)

// ParseCht reads a RetroArch cheat file: "cheats = N", "cheatN_desc", "cheatN_code", "cheatN_enable". Entries without
// a code are dropped; unknown keys are ignored. The result keeps the order of the numbers.
func ParseCht(data []byte) []Cheat {
	type entry struct{ desc, code string }
	entries := map[int]*entry{}
	max := -1
	for _, line := range strings.Split(string(data), "\n") {
		line = strings.TrimSpace(strings.TrimPrefix(line, "\xef\xbb\xbf"))
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		k, v, ok := strings.Cut(line, "=")
		if !ok {
			continue
		}
		k, v = strings.TrimSpace(k), unquote(strings.TrimSpace(v))
		if !strings.HasPrefix(k, "cheat") {
			continue
		}
		rest := k[len("cheat"):]
		us := strings.IndexByte(rest, '_')
		if us < 1 {
			continue
		}
		n, err := strconv.Atoi(rest[:us])
		if err != nil || n < 0 || n >= maxCheatsPerList {
			continue
		}
		e := entries[n]
		if e == nil {
			e = &entry{}
			entries[n] = e
		}
		switch rest[us+1:] {
		case "desc":
			e.desc = v
		case "code":
			if len(v) <= maxCheatCodeLen {
				e.code = v
			}
		default:
			continue
		}
		if n > max {
			max = n
		}
	}
	var out []Cheat
	for n := 0; n <= max; n++ {
		if e := entries[n]; e != nil && strings.TrimSpace(e.code) != "" {
			name := e.desc
			if name == "" {
				name = "Cheat " + strconv.Itoa(n+1)
			}
			out = append(out, Cheat{Name: name, Code: e.code})
		}
	}
	return out
}

func unquote(v string) string {
	if len(v) >= 2 && v[0] == '"' && v[len(v)-1] == '"' {
		v = v[1 : len(v)-1]
	}
	return v
}

// DatIndex maps the hashes of ROMs to the game names of a No-Intro DAT.
type DatIndex struct {
	SHA1 map[string]string // lowercase hex
	CRC  map[string]string // 8 lowercase hex digits
}

func newDatIndex() *DatIndex { return &DatIndex{SHA1: map[string]string{}, CRC: map[string]string{}} }

func (d *DatIndex) add(name, sha1, crc string) {
	if name == "" {
		return
	}
	if sha1 = strings.ToLower(sha1); len(sha1) == 40 {
		d.SHA1[sha1] = name
	}
	if crc = strings.ToLower(crc); len(crc) == 8 {
		d.CRC[crc] = name
	}
}

// ParseDat reads a DAT file in the clrmamepro text format or as Logiqx XML (decided by the first character).
func ParseDat(data []byte) *DatIndex {
	idx := newDatIndex()
	if t := bytes.TrimSpace(data); len(t) > 0 && t[0] == '<' {
		parseDatXML(t, idx)
	} else {
		parseDatText(data, idx)
	}
	return idx
}

func parseDatXML(data []byte, idx *DatIndex) {
	type rom struct {
		SHA1 string `xml:"sha1,attr"`
		CRC  string `xml:"crc,attr"`
	}
	type game struct {
		Name string `xml:"name,attr"`
		ROMs []rom  `xml:"rom"`
	}
	dec := xml.NewDecoder(bytes.NewReader(data))
	dec.Strict = false
	for {
		tok, err := dec.Token()
		if err != nil {
			return
		}
		se, ok := tok.(xml.StartElement)
		if !ok || (se.Name.Local != "game" && se.Name.Local != "machine") {
			continue
		}
		var g game
		if dec.DecodeElement(&g, &se) != nil {
			return
		}
		for _, r := range g.ROMs {
			idx.add(g.Name, r.SHA1, r.CRC)
		}
	}
}

// parseDatText walks the clrmamepro tokens: word, "quoted string", "(" and ")". A "game ( ... )" block gives the name
// and every "rom ( ... )" inside it gives crc and sha1.
func parseDatText(data []byte, idx *DatIndex) {
	toks := tokenizeDat(data)
	for i := 0; i < len(toks); i++ {
		if toks[i] != "game" && toks[i] != "machine" || i+1 >= len(toks) || toks[i+1] != "(" {
			continue
		}
		i += 2
		depth := 1
		name := ""
		type h struct{ sha1, crc string }
		var roms []h
		for ; i < len(toks) && depth > 0; i++ {
			switch toks[i] {
			case "(":
				depth++
			case ")":
				depth--
			case "name":
				if depth == 1 && i+1 < len(toks) {
					name = toks[i+1]
					i++
				}
			case "rom":
				if i+1 < len(toks) && toks[i+1] == "(" {
					i += 2
					var r h
					for ; i < len(toks) && toks[i] != ")"; i++ {
						switch toks[i] {
						case "sha1":
							if i+1 < len(toks) {
								r.sha1 = toks[i+1]
								i++
							}
						case "crc":
							if i+1 < len(toks) {
								r.crc = toks[i+1]
								i++
							}
						}
					}
					roms = append(roms, r)
				}
			}
		}
		for _, r := range roms {
			idx.add(name, r.sha1, r.crc)
		}
		i--
	}
}

func tokenizeDat(data []byte) []string {
	var toks []string
	i, n := 0, len(data)
	for i < n {
		c := data[i]
		switch {
		case c == ' ' || c == '\t' || c == '\r' || c == '\n':
			i++
		case c == '(' || c == ')':
			toks = append(toks, string(c))
			i++
		case c == '"':
			j := i + 1
			for j < n && data[j] != '"' {
				j++
			}
			toks = append(toks, string(data[i+1:min(j, n)]))
			i = j + 1
		default:
			j := i
			for j < n && !strings.ContainsRune(" \t\r\n()\"", rune(data[j])) {
				j++
			}
			toks = append(toks, string(data[i:j]))
			i = j
		}
	}
	return toks
}

var (
	hexPair8x8   = regexp.MustCompile(`^[0-9A-Fa-f]{8}[ :][0-9A-Fa-f]{8}$`)
	hexPair8x4   = regexp.MustCompile(`^[0-9A-Fa-f]{8}[ :][0-9A-Fa-f]{4}$`)
	hex16        = regexp.MustCompile(`^[0-9A-Fa-f]{16}$`)
	hex8         = regexp.MustCompile(`^[0-9A-Fa-f]{8}$`)
	gameGenieGB  = regexp.MustCompile(`^[0-9A-Fa-f]{3}-[0-9A-Fa-f]{3}(-[0-9A-Fa-f]{3})?$`)
	codePartSplt = regexp.MustCompile(`[+\n]`)
)

// GuessCheatFormat guesses the format of a code from the system and the shape of its lines (parts are separated by
// "+" or a line break). It is a guess for the cheats plugin to pre-select a format, never a promise:
// GBA "XXXXXXXX YYYY" is CodeBreaker, "XXXXXXXX YYYYYYYY" ActionReplay v3, 16 contiguous hex digits GameShark Advance;
// NDS "XXXXXXXX YYYYYYYY" is Action Replay DS; GB and GBC "XXX-YYY[-ZZZ]" is Game Genie and 8 hex digits GameShark;
// 3DS "XXXXXXXX YYYYYYYY" is Gateway. Everything else is raw.
func GuessCheatFormat(system, code string) string {
	parts := codePartSplt.Split(code, -1)
	all := func(re ...*regexp.Regexp) bool {
		for _, p := range parts {
			p = strings.TrimSpace(p)
			ok := false
			for _, r := range re {
				if r.MatchString(p) {
					ok = true
				}
			}
			if !ok {
				return false
			}
		}
		return true
	}
	switch system {
	case "gba":
		switch {
		case all(hexPair8x4):
			return CheatCB
		case all(hexPair8x8):
			return CheatARGBA
		case all(hex16):
			return CheatGSA
		}
	case "nds":
		if all(hexPair8x8) {
			return CheatARDS
		}
	case "3ds":
		if all(hexPair8x8) {
			return CheatGateway
		}
	case "gb", "gbc":
		switch {
		case all(gameGenieGB):
			return CheatGameGenie
		case all(hex8):
			return CheatGSA
		}
	}
	return CheatRaw
}

var gatewayCodeLine = regexp.MustCompile(`^[0-9A-Fa-f]{8} [0-9A-Fa-f]{8}$`)

// ParseGatewayList reads a Citra/Azahar-style Gateway list for one 3DS title: "[Cheat name]" header lines followed by
// code lines "XXXXXXXX YYYYYYYY". Lines starting with "*" (such as "*citra_enabled"), comments and anything that is no code
// line are ignored; cheats without a code are dropped. The code of a cheat joins its lines with a line break.
func ParseGatewayList(data []byte) []Cheat {
	var out []Cheat
	cur := -1
	for _, line := range strings.Split(string(data), "\n") {
		line = strings.TrimSpace(strings.TrimPrefix(line, "\xef\xbb\xbf"))
		switch {
		case line == "" || strings.HasPrefix(line, "*") || strings.HasPrefix(line, "#") || strings.HasPrefix(line, "//"):
		case strings.HasPrefix(line, "[") && strings.HasSuffix(line, "]"):
			if len(out) >= maxCheatsPerList {
				return out
			}
			out = append(out, Cheat{Name: strings.TrimSpace(line[1 : len(line)-1]), FormatGuess: CheatGateway})
			cur = len(out) - 1
		case cur >= 0 && gatewayCodeLine.MatchString(line):
			if c := &out[cur]; len(c.Code) < maxCheatCodeLen {
				if c.Code != "" {
					c.Code += "\n"
				}
				c.Code += strings.ToUpper(line)
			}
		}
	}
	kept := out[:0]
	for i, c := range out {
		if c.Code != "" {
			if c.Name == "" {
				c.Name = "Cheat " + strconv.Itoa(i+1)
			}
			kept = append(kept, c)
		}
	}
	return kept
}
