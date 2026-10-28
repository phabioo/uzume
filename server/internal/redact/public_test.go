package redact

import (
	"strings"
	"testing"
)

func TestPublic(t *testing.T) {
	known := Known{Names: []string{"Anna", "Annas Laptop", "Bo", "Uzume"}, Games: []string{"Homebrew Quest", "homebrew-quest.nds"}}
	for _, c := range []struct{ name, in, want string }{
		{"host", "cannot reach hub.example.org:8443 now", "cannot reach <host> now"},
		{"two-label host", "reach myhost.de and example.org:8080", "reach <host> and <host>"},
		{"file names kept", "see main.lua:12 in player.log and libcore.so, a.sav", "see main.lua:12 in player.log and libcore.so, a.sav"},
		{"plugin id", "plugin org.example.plugin failed", "plugin <host> failed"},
		{"github stays", "see https://github.com/phabioo/uzume/issues/12 and github.com/x", "see https://github.com/phabioo/uzume/issues/12 and github.com/x"},
		{"github url not renamed", "see https://github.com/phabioo/uzume/issues/3 by Uzume", "see https://github.com/phabioo/uzume/issues/3 by <name>"},
		{"other url", "GET https://hub.example.org/api/v1/x?a=1 and http://203.0.113.5:8080/x failed", "GET <url> and <url> failed"},
		{"ipv4", "peer 192.0.2.17 and 203.0.113.9:4000", "peer <ip> and <ip>"},
		{"ipv6", "peer 2001:db8::1 and [2001:db8::2]:443", "peer <ip> and <ip>"},
		{"invite", "invite UZ-ABCD-2345 used, invite code: XYZ12345", "invite <code> used, invite code: [redacted]"},
		{"pairing code", "pairing code ABCD2345 is wrong", "pairing code <code> is wrong"},
		{"windows home", `open C:\Users\Anna Smith\x.sav`, `open <home> Smith\x.sav`},
		{"windows home plain", `open C:\Users\carol\Documents\x.sav`, `open <home>\Documents\x.sav`},
		{"unix home", "read /home/carol/.config/app.cfg, /Users/dave/x", "read <home>/.config/app.cfg, <home>/x"},
		{"names", "device Annas Laptop of anna failed; annabel ok; Bo", "device <name> of <name> failed; annabel ok; <name>"},
		{"games", "load Homebrew Quest (homebrew-quest.nds) failed", "load <game> (<game>) failed"},
		{"secret", "Authorization: Bearer abcdefghij123456", "Authorization: [redacted]"},
		{"plain stays", "main.lua:12: attempt to index a nil value (field 'x'), version 0.23.1, 12 events", "main.lua:12: attempt to index a nil value (field 'x'), version 0.23.1, 12 events"},
	} {
		t.Run(c.name, func(t *testing.T) {
			if got := Public(c.in, known); got != c.want {
				t.Errorf("Public(%q)\n got  %q\n want %q", c.in, got, c.want)
			}
		})
	}
}

func TestPublicLeavesNothingBehind(t *testing.T) {
	in := "hub.example.org 192.0.2.1 203.0.113.7:9 https://hub.example.org/x /home/carol C:\\Users\\carol UZ-ABCD-2345 Homebrew Quest Anna"
	got := Public(in, Known{Names: []string{"Anna"}, Games: []string{"Homebrew Quest"}})
	for _, bad := range []string{"example.org", "192.0.2", "203.0.113", "carol", "UZ-ABCD", "Homebrew", "Anna"} {
		if strings.Contains(got, bad) {
			t.Errorf("%q still in %q", bad, got)
		}
	}
}
