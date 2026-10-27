package config

import (
	"flag"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

func parse(t *testing.T, env map[string]string, args ...string) *Config {
	t.Helper()
	fs := flag.NewFlagSet("t", flag.ContinueOnError)
	c := Register(fs, func(k string) string { return env[k] })
	if err := fs.Parse(args); err != nil {
		t.Fatal(err)
	}
	return c
}

func TestDefaultsEnvAndFlags(t *testing.T) {
	c := parse(t, nil)
	if c.DataDir != DefaultDataDir(runtime.GOOS, func(string) string { return "" }) || c.Listen != ":8443" || c.Dev || !c.UseTLS() {
		t.Fatalf("defaults: %+v", c)
	}
	c = parse(t, map[string]string{"UZUME_LISTEN": ":9000", "UZUME_DEV": "true"}, "-data-dir", "/x")
	if c.Listen != ":9000" || !c.Dev || c.DataDir != "/x" || c.UseTLS() {
		t.Fatalf("env/flag: %+v", c)
	}
	c = parse(t, map[string]string{"UZUME_LISTEN": ":9000"}, "-listen", "127.0.0.1:1")
	if c.Listen != "127.0.0.1:1" || !c.ListenIsLoopback() {
		t.Fatalf("flag override: %+v", c)
	}
}

func TestValidate(t *testing.T) {
	if err := parse(t, nil, "-tls-cert", "a").Validate(); err == nil {
		t.Fatal("cert without key must fail")
	}
	if err := parse(t, nil, "-dev", "-tls-cert", "a", "-tls-key", "b").Validate(); err == nil {
		t.Fatal("dev + tls must fail")
	}
	if err := parse(t, nil).Validate(); err != nil {
		t.Fatal(err)
	}
}

func TestICEServers(t *testing.T) {
	if c := parse(t, nil); len(c.ICEServers) != 0 || c.Validate() != nil {
		t.Fatalf("default must be empty: %+v", c.ICEServers)
	}
	c := parse(t, map[string]string{"UZUME_ICE_SERVERS": "stun:a.example:3478"})
	if len(c.ICEServers) != 1 || c.ICEServers[0] != "stun:a.example:3478" || c.Validate() != nil {
		t.Fatalf("env: %v", c.ICEServers)
	}
	c = parse(t, map[string]string{"UZUME_ICE_SERVERS": "stun:env.example"}, "-ice-servers", "stun:a.example:1, stun:b.example:2")
	if len(c.ICEServers) != 2 || c.ICEServers[1] != "stun:b.example:2" || c.Validate() != nil {
		t.Fatalf("flag overrides env: %v", c.ICEServers)
	}
	if parse(t, nil, "-ice-servers", "turn:x.example").Validate() == nil {
		t.Fatal("non-stun URL must fail")
	}
}

func TestCoreSourceConfig(t *testing.T) {
	good := "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="
	c := parse(t, nil)
	if c.CoreBuildbotURL != "https://buildbot.libretro.com/nightly" || c.CoreInfoURL != "https://buildbot.libretro.com/assets/frontend/info.zip" ||
		c.CoreIndexURL != "" || len(c.CoreTrustKeys) != 0 || c.Validate() != nil {
		t.Fatalf("defaults: %+v", c)
	}
	c = parse(t, map[string]string{"UZUME_SERVER_CORE_BUILDBOT_URL": "https://mirror.example/nightly", "UZUME_SERVER_CORE_INFO_URL": "https://mirror.example/info.zip",
		"UZUME_SERVER_CORE_TRUST_KEYS": good + " , " + good})
	if c.CoreBuildbotURL != "https://mirror.example/nightly" || c.CoreInfoURL != "https://mirror.example/info.zip" || len(c.CoreTrustKeys) != 2 || c.Validate() != nil {
		t.Fatalf("env: %+v", c)
	}
	// Repeatable flag replaces the environment list, further uses append; flags win over the environment.
	c = parse(t, map[string]string{"UZUME_SERVER_CORE_TRUST_KEYS": "env", "UZUME_SERVER_CORE_BUILDBOT_URL": "https://env.example/n"},
		"-core-trust-key", good, "-core-trust-key", good, "-core-buildbot-url", "https://x.example/n", "-core-info-url", "https://x.example/info.zip")
	if len(c.CoreTrustKeys) != 2 || c.CoreBuildbotURL != "https://x.example/n" || c.CoreInfoURL != "https://x.example/info.zip" || c.Validate() != nil {
		t.Fatalf("flags: %+v", c)
	}
	// The deprecated -core-index-url is still accepted (and ignored), even with an old http value.
	c = parse(t, map[string]string{"UZUME_SERVER_CORE_INDEX_URL": "https://old.example/i.json"})
	if c.CoreIndexURL != "https://old.example/i.json" || c.Validate() != nil {
		t.Fatalf("deprecated env: %+v", c)
	}
	if c = parse(t, nil, "-core-index-url", "http://x.example/i"); c.Validate() != nil {
		t.Fatal("deprecated flag must not be validated")
	}
	for _, bad := range []*Config{parse(t, nil, "-core-buildbot-url", "http://x.example/n"), parse(t, nil, "-core-info-url", "http://x.example/i.zip"),
		parse(t, nil, "-core-trust-key", "nope")} {
		if bad.Validate() == nil {
			t.Fatalf("invalid core config accepted: %+v", bad)
		}
	}
}

func TestUpdateConfig(t *testing.T) {
	c := parse(t, nil)
	if c.Validate() != nil || c.UpdateIndexURL != "https://github.com/phabioo/uzume/releases/download/updates-index/updates-index.json" ||
		c.UpdateRequestDir != DefaultUpdateRequestDir(runtime.GOOS, c.DataDir) {
		t.Fatalf("defaults: %+v", c)
	}
	c = parse(t, map[string]string{"UZUME_SERVER_UPDATE_INDEX_URL": "file:///tmp/updates-index.json", "UZUME_SERVER_UPDATE_REQUEST_DIR": "/tmp/req"})
	if c.UpdateIndexURL != "file:///tmp/updates-index.json" || c.UpdateRequestDir != "/tmp/req" || c.Validate() != nil {
		t.Fatalf("env: %+v", c)
	}
	c = parse(t, map[string]string{"UZUME_SERVER_UPDATE_INDEX_URL": "https://env.example/i"}, "-update-index-url", "https://flag.example/i", "-update-request-dir", "/x")
	if c.UpdateIndexURL != "https://flag.example/i" || c.UpdateRequestDir != "/x" {
		t.Fatalf("flags: %+v", c)
	}
	if parse(t, nil, "-update-index-url", "http://x.example/i").Validate() == nil || parse(t, nil, "-update-request-dir", "").Validate() == nil {
		t.Fatal("invalid update config accepted")
	}
}

func TestPluginIndexConfig(t *testing.T) {
	if c := parse(t, nil); c.PluginIndexURL != "https://github.com/phabioo/uzume/releases/download/plugin-index/plugin-index.json" ||
		c.PluginIndex() != c.PluginIndexURL || c.Validate() != nil {
		t.Fatalf("default: %+v", c)
	}
	for _, c := range []*Config{
		parse(t, map[string]string{"UZUME_PLUGIN_INDEX_URL": "none"}),
		parse(t, nil, "-plugin-index-url", "none"),
		parse(t, nil, "-plugin-index-url", "NONE"),
	} {
		if c.PluginIndex() != "" || c.Validate() != nil {
			t.Fatalf("none: %+v", c)
		}
	}
	c := parse(t, map[string]string{"UZUME_PLUGIN_INDEX_URL": "https://env.example/plugin-index.json"})
	if c.PluginIndexURL != "https://env.example/plugin-index.json" || c.Validate() != nil {
		t.Fatalf("env: %+v", c)
	}
	c = parse(t, map[string]string{"UZUME_PLUGIN_INDEX_URL": "https://env.example/i"}, "-plugin-index-url", "file:///tmp/plugin-index.json")
	if c.PluginIndexURL != "file:///tmp/plugin-index.json" || c.Validate() != nil {
		t.Fatalf("flag: %+v", c)
	}
	if parse(t, nil, "-plugin-index-url", "http://x.example/i").Validate() == nil {
		t.Fatal("http plugin index accepted")
	}
}

func TestTURNConfig(t *testing.T) {
	c := parse(t, nil)
	if c.TURN || c.TURNPort != 3478 || c.TURNRelayPorts != "49160-49199" || c.Validate() != nil {
		t.Fatalf("defaults: %+v", c)
	}
	if c = parse(t, nil, "-turn"); c.Validate() == nil {
		t.Fatal("-turn without -public-host accepted")
	}
	c = parse(t, map[string]string{"UZUME_TURN": "1", "UZUME_PUBLIC_HOST": "hub.example.org", "UZUME_TURN_PORT": "3479",
		"UZUME_TURN_RELAY_PORTS": "50000-50009", "UZUME_TURN_RELAY_IP": "203.0.113.7"})
	lo, hi, rerr := c.TURNRelayRange()
	if err := c.Validate(); err != nil || !c.TURN || c.TURNPort != 3479 || lo != 50000 || hi != 50009 || rerr != nil {
		t.Fatalf("env: %+v %v", c, err)
	}
	c = parse(t, map[string]string{"UZUME_TURN_PORT": "3479"}, "-turn", "-public-host", "hub.example.org", "-turn-port", "4000")
	if c.TURNPort != 4000 || c.Validate() != nil {
		t.Fatalf("flag: %+v", c)
	}
	for _, bad := range [][]string{{"-turn-relay-ports", "5-"}, {"-turn-relay-ports", "50010-50000"}, {"-turn-relay-ports", "0-10"},
		{"-turn-relay-ports", "x"}, {"-turn-relay-ports", "1-70000"}, {"-turn-port", "0"}, {"-turn-relay-ip", "2001:db8::1"}, {"-turn-relay-ip", "nope"}} {
		c = parse(t, nil, append([]string{"-turn", "-public-host", "hub.example.org"}, bad...)...)
		if c.Validate() == nil {
			t.Errorf("%v accepted", bad)
		}
	}
}

func TestSaveRetentionConfig(t *testing.T) {
	c := parse(t, nil)
	if c.SaveKeepRecent != 20 || c.SaveKeepDaily != 30 || c.SaveKeepWeekly != 26 {
		t.Fatalf("defaults: %+v", c)
	}
	c = parse(t, map[string]string{"UZUME_SAVE_KEEP_RECENT": "5", "UZUME_SAVE_KEEP_DAILY": "0"}, "-save-keep-weekly", "3")
	if c.SaveKeepRecent != 5 || c.SaveKeepDaily != 0 || c.SaveKeepWeekly != 3 {
		t.Fatalf("env/flag: %+v", c)
	}
	if err := c.Validate(); err != nil {
		t.Fatal(err)
	}
	if err := parse(t, nil, "-save-keep-daily", "-1").Validate(); err == nil {
		t.Fatal("negative value must fail")
	}
}

func TestWindowsDefaults(t *testing.T) {
	env := map[string]string{"ProgramData": filepath.Join("D:", "PD")}
	get := func(k string) string { return env[k] }
	data := filepath.Join("D:", "PD", "Uzume", "Server")
	if got := DefaultDataDir("windows", get); got != data {
		t.Fatalf("data dir %q", got)
	}
	if got := DefaultDataDir("windows", func(string) string { return "" }); got != filepath.Join(`C:\ProgramData`, "Uzume", "Server") {
		t.Fatalf("fallback data dir %q", got)
	}
	if DefaultDataDir("linux", get) != "/var/lib/uzume" || DefaultUpdateRequestDir("linux", "/x") != "/run/uzume" {
		t.Fatal("linux defaults changed")
	}
	reg := func(goos string, args ...string) *Config {
		fs := flag.NewFlagSet("t", flag.ContinueOnError)
		c := register(fs, get, goos)
		if err := fs.Parse(args); err != nil {
			t.Fatal(err)
		}
		return c
	}
	c := reg("windows")
	if err := c.Validate(); err != nil {
		t.Fatal(err)
	}
	if c.DataDir != data || c.UpdateRequestDir != filepath.Join(data, "update-request") || c.ImportDir() != filepath.Join(data, "library-import") {
		t.Fatalf("windows defaults: %+v", c)
	}
	// The request directory follows -data-dir unless set itself.
	c = reg("windows", "-data-dir", filepath.Join("E:", "hub"))
	if err := c.Validate(); err != nil || c.UpdateRequestDir != filepath.Join("E:", "hub", "update-request") {
		t.Fatalf("custom data dir: %v %q", err, c.UpdateRequestDir)
	}
	c = reg("windows", "-update-request-dir", "")
	if c.Validate() == nil {
		t.Fatal("explicit empty request dir accepted")
	}
}

func TestICEServersDefaultAndNone(t *testing.T) {
	if c := parse(t, nil); c.ICEServers != nil || len(EffectiveICE(c.ICEServers)) != 1 || EffectiveICE(c.ICEServers)[0] != DefaultSTUNServer {
		t.Fatalf("unset must mean the default: %#v", c.ICEServers)
	}
	for _, c := range []*Config{parse(t, map[string]string{"UZUME_ICE_SERVERS": "none"}), parse(t, nil, "-ice-servers", " None ")} {
		if c.ICEServers == nil || len(c.ICEServers) != 0 || len(EffectiveICE(c.ICEServers)) != 0 || c.Validate() != nil {
			t.Fatalf("none must be explicitly empty: %#v", c.ICEServers)
		}
		if got := CloneICE(c.ICEServers); got == nil {
			t.Fatal("CloneICE lost the difference between none and default")
		}
	}
	if EncodeICEServers(nil) != "null" || EncodeICEServers([]string{}) != "none" {
		t.Fatal("stored forms must differ")
	}
	// Legacy stored values ("[]", "null") mean the default; only "none" is explicitly empty.
	for _, legacy := range []string{"null", "[]"} {
		if l, err := DecodeICEServers(legacy); err != nil || l != nil {
			t.Fatal(legacy, l, err)
		}
	}
	if l, err := DecodeICEServers("none"); err != nil || l == nil || len(l) != 0 {
		t.Fatal(l, err)
	}
}

func TestCloudflareTURNConfig(t *testing.T) {
	c := parse(t, map[string]string{"UZUME_CLOUDFLARE_TURN_KEY_ID": "abc-123"})
	if c.CloudflareTURNKeyID != "abc-123" || c.Validate() != nil {
		t.Fatalf("%+v", c)
	}
	if _, err := c.CloudflareTURNToken(func(string) string { return "" }); err == nil {
		t.Fatal("token source missing must fail")
	}
	if tok, err := c.CloudflareTURNToken(func(k string) string {
		if k == "UZUME_CLOUDFLARE_TURN_API_TOKEN" {
			return " tok "
		}
		return ""
	}); err != nil || tok != "tok" {
		t.Fatal(tok, err)
	}
	f := filepath.Join(t.TempDir(), "token")
	os.WriteFile(f, []byte("filetok\n"), 0o600)
	c = parse(t, nil, "-cloudflare-turn-key-id", "k", "-cloudflare-turn-token-file", f)
	if tok, err := c.CloudflareTURNToken(func(string) string { return "" }); err != nil || tok != "filetok" {
		t.Fatal(tok, err)
	}
	if parse(t, nil, "-cloudflare-turn-token-file", f).Validate() == nil || parse(t, nil, "-cloudflare-turn-key-id", "a/b").Validate() == nil {
		t.Fatal("token file without key id and bad key id must fail")
	}
}

func TestExternalTURNFlagAndEnv(t *testing.T) {
	fs := flag.NewFlagSet("t", flag.ContinueOnError)
	env := map[string]string{"UZUME_TURN_EXTERNAL": "urls=turn:env.example.org:3478"}
	c := Register(fs, func(k string) string { return env[k] })
	if err := fs.Parse([]string{
		"-turn-external", "name=vps;urls=turn:turn.example.org:3478?transport=udp,turns:turn.example.org:5349?transport=tcp;secret-file=/etc/uzume/turn.secret",
		"-turn-external", "mode=static;username=u1;urls=turn:turn2.example.org:3478;password-file=/etc/uzume/pw"}); err != nil {
		t.Fatal(err)
	}
	specs, err := c.ExternalTURNSpecs()
	if err != nil || len(specs) != 2 { // the flag replaces the environment value
		t.Fatalf("%v %+v", err, specs)
	}
	if s := specs[0]; s.Name != "vps" || s.Mode != ExternalModeSecret || len(s.URLs) != 2 || s.SecretFile != "/etc/uzume/turn.secret" {
		t.Errorf("%+v", s)
	}
	if s := specs[1]; s.Name != "external-2" || s.Mode != ExternalModeStatic || s.Username != "u1" || s.PasswordFile != "/etc/uzume/pw" {
		t.Errorf("%+v", s)
	}
	// Environment only.
	fs = flag.NewFlagSet("t", flag.ContinueOnError)
	c = Register(fs, func(k string) string { return env[k] })
	fs.Parse(nil)
	if specs, err := c.ExternalTURNSpecs(); err != nil || len(specs) != 1 || specs[0].Name != "external-1" {
		t.Fatalf("%v %+v", err, specs)
	}
}

func TestExternalTURNInvalid(t *testing.T) {
	for _, v := range []string{
		"mode=secret",                                    // no urls
		"urls=stun:turn.example.org:3478",                // not turn:
		"urls=turn:user:pw@turn.example.org:3478",        // credentials in the URL
		"urls=turn:turn.example.org:3478?transport=sctp", // transport
		"urls=turn:turn.example.org:3478;mode=static",    // static without username
		"urls=turn:turn.example.org:3478;mode=secret;username=u",
		"urls=turn:turn.example.org:3478;mode=static;username=u;secret-file=/x",
		"urls=turn:turn.example.org:3478;mode=other",
		"urls=turn:turn.example.org:3478;secret=abc", // the secret itself is never a field
		"urls=turn:turn.example.org:3478;name=a b",
	} {
		c := Config{TURNExternal: []string{v}}
		if _, err := c.ExternalTURNSpecs(); err == nil {
			t.Errorf("%q accepted", v)
		} else if strings.Contains(v, "secret=abc") && strings.Contains(err.Error(), "abc") {
			t.Errorf("error repeats the value: %v", err)
		}
	}
	c := Config{TURNExternal: []string{"name=a;urls=turn:a.example.org:3478", "name=a;urls=turn:b.example.org:3478"}}
	if _, err := c.ExternalTURNSpecs(); err == nil {
		t.Error("duplicate name accepted")
	}
}

func TestExternalTURNSecretSources(t *testing.T) {
	dir := t.TempDir()
	file := filepath.Join(dir, "secret")
	os.WriteFile(file, []byte("file-secret-0123\n"), 0o600)
	c := Config{}
	sp := ExternalTURNSpec{Name: "vps", Mode: ExternalModeSecret, SecretFile: file}
	if v, err := c.ExternalTURNSecret(sp, 1, func(string) string { return "" }); err != nil || v != "file-secret-0123" {
		t.Fatalf("%q %v", v, err)
	}
	get := func(k string) string {
		if k == "UZUME_TURN_EXTERNAL_2_SECRET" {
			return " env-secret-0123 "
		}
		return ""
	}
	if v, err := c.ExternalTURNSecret(sp, 2, get); err != nil || v != "env-secret-0123" {
		t.Fatalf("env wins: %q %v", v, err)
	}
	st := ExternalTURNSpec{Name: "p", Mode: ExternalModeStatic}
	if _, err := c.ExternalTURNSecret(st, 1, func(string) string { return "" }); err == nil || !strings.Contains(err.Error(), "UZUME_TURN_EXTERNAL_1_PASSWORD") {
		t.Fatalf("%v", err)
	}
	os.WriteFile(file, []byte("\n"), 0o600)
	if _, err := c.ExternalTURNSecret(sp, 1, func(string) string { return "" }); err == nil {
		t.Fatal("empty file accepted")
	}
}

func TestTrustedProxy(t *testing.T) {
	c := parse(t, map[string]string{"UZUME_TRUSTED_PROXY": "10.0.0.0/8"})
	if c.TrustedProxy != "10.0.0.0/8" {
		t.Fatalf("env: %q", c.TrustedProxy)
	}
	c = parse(t, map[string]string{"UZUME_TRUSTED_PROXY": "10.0.0.0/8"}, "-trusted-proxy", "127.0.0.1,172.16.0.0/12")
	if c.TrustedProxy != "127.0.0.1,172.16.0.0/12" {
		t.Fatalf("flag wins: %q", c.TrustedProxy)
	}
	c.DataDir = t.TempDir()
	if err := c.Validate(); err != nil {
		t.Fatal(err)
	}
	c.TrustedProxy = "10.0.0.0/8,not-an-ip"
	if err := c.Validate(); err == nil || !strings.Contains(err.Error(), "trusted-proxy") {
		t.Fatalf("invalid list must fail: %v", err)
	}
}

func TestChildFlag(t *testing.T) {
	if parse(t, nil).Child {
		t.Fatal("child must default to off")
	}
	if !parse(t, map[string]string{"UZUME_CHILD": "1"}).Child || !parse(t, nil, "-child").Child {
		t.Fatal("UZUME_CHILD / -child not read")
	}
	if parse(t, map[string]string{"UZUME_CHILD": "1"}, "-child=false").Child {
		t.Fatal("flag must override the environment")
	}
}

func TestToolsSourceConfig(t *testing.T) {
	if c := parse(t, nil); c.ToolsSourceURL != DefaultToolsSourceURL || c.ToolsSource() != DefaultToolsSourceURL || c.Validate() != nil {
		t.Fatalf("default: %+v", c)
	}
	for _, c := range []*Config{
		parse(t, map[string]string{"UZUME_TOOLS_SOURCE_URL": "none"}),
		parse(t, nil, "-tools-source-url", "NONE"),
	} {
		if c.ToolsSource() != "" || c.Validate() != nil {
			t.Fatalf("none: %+v", c)
		}
	}
	c := parse(t, map[string]string{"UZUME_TOOLS_SOURCE_URL": "https://env.example/releases"}, "-tools-source-url", "https://flag.example/releases")
	if c.ToolsSource() != "https://flag.example/releases" || c.Validate() != nil {
		t.Fatalf("flag: %+v", c)
	}
	if c = parse(t, nil, "-tools-source-url", "http://insecure.example/x"); c.Validate() == nil {
		t.Fatal("http accepted")
	}
}
