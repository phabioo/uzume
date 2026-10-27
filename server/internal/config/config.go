// Package config reads the hub configuration from flags and UZUME_* environment variables.
package config

import (
	"crypto/ed25519"
	"errors"
	"flag"
	"fmt"
	"net"
	"net/url"
	"os"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"

	"github.com/phabioo/uzume/server/internal/clientip"
	"github.com/phabioo/uzume/server/internal/corepkg"
	"github.com/phabioo/uzume/server/internal/updates"
)

// Config is the runtime configuration of the Uzume server.
type Config struct {
	goos          string // platform the defaults were chosen for
	requestDirSet bool   // -update-request-dir was given
	DataDir       string
	Listen        string
	// Name is the display name on first start; afterwards the name stored in the database applies.
	Name    string
	TLSCert string
	TLSKey  string
	// Dev allows HTTP instead of HTTPS (development and tests only).
	Dev bool
	// ICEServers are stun: URLs delivered to apps for Sessions. nil = not configured: DefaultSTUNServer applies;
	// an empty non-nil list ("none") = explicitly no external STUN. Copy with CloneICE to keep that difference.
	ICEServers []string
	// CloudflareTURNKeyID selects the optional Cloudflare Realtime TURN key (relay provider next to the embedded
	// TURN server). The API token is never part of the Config: CloudflareTURNToken reads it from the environment
	// or from CloudflareTURNTokenFile.
	CloudflareTURNKeyID     string
	CloudflareTURNTokenFile string
	// CloudflareTURNBaseURL overrides the API base (tests only, no flag).
	CloudflareTURNBaseURL string
	// TURNExternal are the raw -turn-external values (external TURN servers); see ExternalTURNSpecs. They hold no
	// secret: the shared secret or password comes from the environment or a file (ExternalTURNSecret).
	TURNExternal []string
	// CoreBuildbotURL is the libretro buildbot nightly base URL (ADR 0020 D1), https only.
	CoreBuildbotURL string
	// CoreInfoURL is the libretro core info archive (info.zip), https only.
	CoreInfoURL string
	// CoreIndexURL is deprecated and ignored (the signed core index was retired by ADR 0020); a non-empty value
	// only produces a log warning.
	CoreIndexURL string
	// CoreTrustKeys are additional trusted signing keys (base64 Ed25519 public keys) besides the compiled-in ones.
	// They apply to the updates index only (cores are no longer signed).
	CoreTrustKeys []string
	// UpdateIndexURL is the signed updates index (default updates.DefaultIndexURL; https or file://).
	UpdateIndexURL string
	// PluginIndexURL is the signed plugin index (default updates.DefaultPluginIndexURL; https or file://); "none" or
	// empty = no index (offline servers). Use PluginIndex for the effective value.
	PluginIndexURL string
	// ToolsSourceURL is the release list of the tools catalog (default hub.DefaultToolsSourceURL, the GitHub releases
	// API of the randomizer engine); "none" or empty = off. Use ToolsSource for the effective value.
	ToolsSourceURL string
	// UpdateRequestDir is where the update request file is created (default /run/uzume; Windows <data-dir>\update-request).
	UpdateRequestDir string
	// SaveKeepRecent, SaveKeepDaily, SaveKeepWeekly are the save history retention rules (ADR 0012 D7):
	// newest versions kept, days and weeks of which the newest version is kept. 0 = unlimited for that rule.
	SaveKeepRecent, SaveKeepDaily, SaveKeepWeekly int
	// LibraryImportDir is the folder "Rescan folder" imports ROMs from (default <data-dir>/library-import; files are only read).
	LibraryImportDir string
	// NetworkSharing lets other devices reach the server. Off: the server listens on loopback only (127.0.0.1 and ::1) and
	// the embedded TURN server stays off. Initial value only: once stored (Settings), the stored value wins.
	NetworkSharing bool
	// TURN switches on the embedded STUN/TURN server (ADR 0012 D2); it needs PublicHost.
	TURN bool
	// PublicHost is the DNS name (or IPv4) of the server's public address, optionally with an external port
	// ("hub.example.org:8443", IPv6 "[::1]:8443"); see SplitPublicHost, PublicHostName, PublicHostPort.
	PublicHost string
	// TURNPort is the UDP and TCP port of the TURN server (default 3478).
	TURNPort int
	// TURNRelayPorts is the UDP relay range "min-max" (default 49160-49199).
	TURNRelayPorts string
	// TURNRelayIP is an optional fixed public IPv4 for relayed addresses.
	TURNRelayIP string
	// TrustedProxy is a comma or space separated list of CIDRs or IPs of reverse proxies in front of the server. Only
	// requests whose TCP peer is inside it are believed about client IP and scheme (X-Forwarded-For/-Proto, Forwarded).
	TrustedProxy string
	// Child runs the server as the app's child process (ADR 0030 D6): loopback only whatever the stored Network sharing
	// setting says, no TURN, no self-update, ends when stdin closes, restarts in-process.
	Child bool
}

// Defaults of the core source (ADR 0020 D1).
const (
	DefaultCoreBuildbotURL = "https://buildbot.libretro.com/nightly"
	DefaultCoreInfoURL     = "https://buildbot.libretro.com/assets/frontend/info.zip"
)

// DefaultSTUNServer is the STUN URL used when no external STUN server is configured.
const DefaultSTUNServer = "stun:stun.cloudflare.com:3478"

// CloneICE copies an ICE server list and keeps nil (default) distinct from empty (none).
func CloneICE(l []string) []string {
	if l == nil {
		return nil
	}
	return append([]string{}, l...)
}

// EffectiveICE resolves a configured list: nil becomes the default STUN server, empty stays empty.
func EffectiveICE(l []string) []string {
	if l == nil {
		return []string{DefaultSTUNServer}
	}
	return append([]string{}, l...)
}

// parseICE reads a comma-separated list; empty = not configured (nil), "none" = explicitly empty.
func parseICE(s string) []string {
	if strings.EqualFold(strings.TrimSpace(s), "none") {
		return []string{}
	}
	return splitCSV(s)
}

// CloudflareTURNToken returns the Cloudflare TURN API token: the environment variable
// UZUME_CLOUDFLARE_TURN_API_TOKEN wins over the token file. Errors never contain the token.
func (c *Config) CloudflareTURNToken(getenv func(string) string) (string, error) {
	if v := strings.TrimSpace(getenv("UZUME_CLOUDFLARE_TURN_API_TOKEN")); v != "" {
		return v, nil
	}
	if c.CloudflareTURNTokenFile == "" {
		return "", errors.New("cloudflare TURN needs the API token in UZUME_CLOUDFLARE_TURN_API_TOKEN or -cloudflare-turn-token-file")
	}
	b, err := os.ReadFile(c.CloudflareTURNTokenFile)
	if err != nil {
		return "", fmt.Errorf("cloudflare-turn-token-file: %w", err)
	}
	if t := strings.TrimSpace(string(b)); t != "" {
		return t, nil
	}
	return "", errors.New("cloudflare-turn-token-file is empty")
}

// Defaults of the TURN options.
const (
	DefaultTURNPort       = 3478
	DefaultTURNRelayPorts = "49160-49199"
)

// TURNRelayRange parses TURNRelayPorts.
func (c *Config) TURNRelayRange() (min, max int, err error) {
	a, b, ok := strings.Cut(strings.TrimSpace(c.TURNRelayPorts), "-")
	if ok {
		min, err = strconv.Atoi(strings.TrimSpace(a))
		if err == nil {
			max, err = strconv.Atoi(strings.TrimSpace(b))
		}
	}
	if !ok || err != nil || min < 1 || max > 65535 || min > max {
		return 0, 0, fmt.Errorf("turn-relay-ports: %q is not a range like 49160-49199", c.TURNRelayPorts)
	}
	return min, max, nil
}

// repeatList is a repeatable flag.Value: the first use replaces the environment default, further uses append.
type repeatList struct {
	v       *[]string
	touched *bool
}

func (l repeatList) String() string {
	if l.v == nil {
		return ""
	}
	return strings.Join(*l.v, ",")
}

func (l repeatList) Set(s string) error {
	if !*l.touched {
		*l.v, *l.touched = nil, true
	}
	if s = strings.TrimSpace(s); s != "" {
		*l.v = append(*l.v, s)
	}
	return nil
}

// csvList is a flag.Value for comma-separated lists.
type csvList struct{ v *[]string }

func (l csvList) String() string {
	if l.v == nil {
		return ""
	}
	return strings.Join(*l.v, ",")
}

func (l csvList) Set(s string) error { *l.v = parseICE(s); return nil }

func splitCSV(s string) []string {
	var out []string
	for _, p := range strings.Split(s, ",") {
		if p = strings.TrimSpace(p); p != "" {
			out = append(out, p)
		}
	}
	return out
}

// DefaultDataDir is the built-in data directory: /data in the container image (UZUME_SERVER_CONTAINER=1, the image sets
// no data-dir variable, so an explicit UZUME_DATA_DIR still wins), else /var/lib/uzume, on
// Windows %ProgramData%\Uzume\Server.
func DefaultDataDir(goos string, getenv func(string) string) string {
	if getenv("UZUME_SERVER_CONTAINER") == "1" {
		return "/data"
	}
	if goos != "windows" {
		return "/var/lib/uzume"
	}
	pd := getenv("ProgramData")
	if pd == "" {
		pd = `C:\ProgramData`
	}
	return filepath.Join(pd, "Uzume", "Server")
}

// DefaultUpdateRequestDir is the built-in update request directory: /run/uzume (systemd RuntimeDirectory), on
// Windows the directory <data>\update-request that the installer creates (writable for the server service account,
// readable for the updater service).
func DefaultUpdateRequestDir(goos, dataDir string) string {
	if goos == "windows" {
		return filepath.Join(dataDir, "update-request")
	}
	return updates.DefaultRequestDir
}

// trackedString is a string flag that remembers whether it was set explicitly.
type trackedString struct {
	p   *string
	set *bool
}

func (t trackedString) String() string {
	if t.p == nil {
		return ""
	}
	return *t.p
}

func (t trackedString) Set(s string) error { *t.p, *t.set = s, true; return nil }

// Register registers the configuration flags on fs. Defaults come from the environment (UZUME_*), otherwise built-in defaults.
// An explicitly set flag overrides the environment.
func Register(fs *flag.FlagSet, getenv func(string) string) *Config {
	return register(fs, getenv, runtime.GOOS)
}

func register(fs *flag.FlagSet, getenv func(string) string, goos string) *Config {
	c := &Config{goos: goos}
	env := func(key, def string) string {
		if v := getenv("UZUME_" + key); v != "" {
			return v
		}
		return def
	}
	dev, _ := strconv.ParseBool(getenv("UZUME_DEV"))
	fs.StringVar(&c.DataDir, "data-dir", env("DATA_DIR", DefaultDataDir(goos, getenv)), "data directory (UZUME_DATA_DIR)")
	fs.StringVar(&c.LibraryImportDir, "library-import-dir", env("LIBRARY_IMPORT_DIR", ""), "folder the Library's \"Rescan folder\" imports ROMs from, default <data-dir>/library-import (UZUME_LIBRARY_IMPORT_DIR)")
	fs.StringVar(&c.Listen, "listen", env("LISTEN", ":8443"), "listen address (UZUME_LISTEN)")
	fs.StringVar(&c.Name, "name", env("NAME", ""), "hub name on first start (UZUME_NAME)")
	fs.StringVar(&c.TLSCert, "tls-cert", env("TLS_CERT", ""), "TLS certificate (PEM) (UZUME_TLS_CERT)")
	fs.StringVar(&c.TLSKey, "tls-key", env("TLS_KEY", ""), "TLS key (PEM) (UZUME_TLS_KEY)")
	fs.BoolVar(&c.Dev, "dev", dev, "development mode: HTTP instead of HTTPS (UZUME_DEV)")
	c.ICEServers = parseICE(getenv("UZUME_ICE_SERVERS"))
	fs.Var(csvList{&c.ICEServers}, "ice-servers", "comma-separated stun: URLs for Sessions, default "+DefaultSTUNServer+", \"none\" = no external STUN (UZUME_ICE_SERVERS)")
	fs.StringVar(&c.CloudflareTURNKeyID, "cloudflare-turn-key-id", env("CLOUDFLARE_TURN_KEY_ID", ""), "Cloudflare Realtime TURN key ID, optional relay next to -turn; the API token comes from UZUME_CLOUDFLARE_TURN_API_TOKEN or -cloudflare-turn-token-file (UZUME_CLOUDFLARE_TURN_KEY_ID)")
	fs.StringVar(&c.CloudflareTURNTokenFile, "cloudflare-turn-token-file", env("CLOUDFLARE_TURN_TOKEN_FILE", ""), "file holding the Cloudflare TURN API token (UZUME_CLOUDFLARE_TURN_TOKEN_FILE)")
	c.TURNExternal = splitLines(getenv("UZUME_TURN_EXTERNAL"))
	fs.Var(&externalList{v: &c.TURNExternal}, "turn-external", "external TURN server, repeatable: mode=secret|static;urls=turn:turn.example.org:3478?transport=udp,...;secret-file=<path> (static: username=<user>;password-file=<path>); secrets also via UZUME_TURN_EXTERNAL_<n>_SECRET/_PASSWORD (UZUME_TURN_EXTERNAL, one server per line)")
	fs.StringVar(&c.CoreBuildbotURL, "core-buildbot-url", func() string {
		if v := getenv("UZUME_SERVER_CORE_BUILDBOT_URL"); v != "" {
			return v
		}
		return DefaultCoreBuildbotURL
	}(), "base URL of the libretro buildbot nightly builds, https only (UZUME_SERVER_CORE_BUILDBOT_URL)")
	fs.StringVar(&c.CoreInfoURL, "core-info-url", func() string {
		if v := getenv("UZUME_SERVER_CORE_INFO_URL"); v != "" {
			return v
		}
		return DefaultCoreInfoURL
	}(), "URL of the libretro core info archive info.zip, https only (UZUME_SERVER_CORE_INFO_URL)")
	fs.StringVar(&c.CoreIndexURL, "core-index-url", getenv("UZUME_SERVER_CORE_INDEX_URL"),
		"deprecated and ignored: the signed core index was retired (ADR 0020), see -core-buildbot-url (UZUME_SERVER_CORE_INDEX_URL)")
	c.CoreTrustKeys = splitCSV(getenv("UZUME_SERVER_CORE_TRUST_KEYS"))
	fs.Var(repeatList{&c.CoreTrustKeys, new(bool)}, "core-trust-key",
		"additional trusted signing key for the updates index (cores are not signed), base64 Ed25519 public key; repeatable (UZUME_SERVER_CORE_TRUST_KEYS, comma-separated)")
	fs.StringVar(&c.UpdateIndexURL, "update-index-url", func() string {
		if v := getenv("UZUME_SERVER_UPDATE_INDEX_URL"); v != "" {
			return v
		}
		return updates.DefaultIndexURL
	}(), "URL of the signed updates index, https or file:// (UZUME_SERVER_UPDATE_INDEX_URL)")
	fs.StringVar(&c.PluginIndexURL, "plugin-index-url", func() string {
		if v := getenv("UZUME_PLUGIN_INDEX_URL"); v != "" {
			return v
		}
		if v := getenv("UZUME_SERVER_PLUGIN_INDEX_URL"); v != "" {
			return v
		}
		return updates.DefaultPluginIndexURL
	}(), "URL of the signed plugin index, https or file://; none = no index, for offline servers (UZUME_PLUGIN_INDEX_URL)")
	fs.StringVar(&c.ToolsSourceURL, "tools-source-url", func() string {
		if v := getenv("UZUME_TOOLS_SOURCE_URL"); v != "" {
			return v
		}
		return DefaultToolsSourceURL
	}(), "URL of the release list of the tools catalog (GitHub releases API), https; none = off, for offline servers (UZUME_TOOLS_SOURCE_URL)")
	intEnv := func(key string, def int) int {
		if n, err := strconv.Atoi(strings.TrimSpace(getenv("UZUME_" + key))); err == nil {
			return n
		}
		return def
	}
	fs.IntVar(&c.SaveKeepRecent, "save-keep-recent", intEnv("SAVE_KEEP_RECENT", 20), "save history: newest versions per slot to keep, 0 = unlimited (UZUME_SAVE_KEEP_RECENT)")
	fs.IntVar(&c.SaveKeepDaily, "save-keep-daily", intEnv("SAVE_KEEP_DAILY", 30), "save history: days of which the newest version is kept, 0 = unlimited (UZUME_SAVE_KEEP_DAILY)")
	fs.IntVar(&c.SaveKeepWeekly, "save-keep-weekly", intEnv("SAVE_KEEP_WEEKLY", 26), "save history: weeks of which the newest version is kept, 0 = unlimited (UZUME_SAVE_KEEP_WEEKLY)")
	c.UpdateRequestDir = func() string {
		if v := getenv("UZUME_SERVER_UPDATE_REQUEST_DIR"); v != "" {
			return v
		}
		if goos == "windows" {
			return "" // follows -data-dir, resolved in Validate unless set explicitly
		}
		return updates.DefaultRequestDir
	}()
	fs.Var(trackedString{&c.UpdateRequestDir, &c.requestDirSet}, "update-request-dir", "directory for the update request file read by the root helper (UZUME_SERVER_UPDATE_REQUEST_DIR)")
	sharing := true
	if b, err := strconv.ParseBool(getenv("UZUME_NETWORK_SHARING")); err == nil {
		sharing = b
	}
	fs.BoolVar(&c.NetworkSharing, "network-sharing", sharing, "other devices may reach the server; false = loopback only, no TURN; initial value, the saved setting wins (UZUME_NETWORK_SHARING)")
	turn, _ := strconv.ParseBool(getenv("UZUME_TURN"))
	fs.BoolVar(&c.TURN, "turn", turn, "embedded STUN/TURN server for Sessions over the internet, needs -public-host (UZUME_TURN)")
	fs.StringVar(&c.PublicHost, "public-host", env("PUBLIC_HOST", ""), "DNS name or IPv4 of the server's public address, optionally host:port when the router forwards a different external port (IPv6 in brackets); required with -turn (UZUME_PUBLIC_HOST)")
	turnPort := DefaultTURNPort
	if v, err := strconv.Atoi(getenv("UZUME_TURN_PORT")); err == nil {
		turnPort = v
	}
	fs.IntVar(&c.TURNPort, "turn-port", turnPort, "TURN UDP and TCP port (UZUME_TURN_PORT)")
	fs.StringVar(&c.TURNRelayPorts, "turn-relay-ports", env("TURN_RELAY_PORTS", DefaultTURNRelayPorts), "TURN UDP relay port range min-max (UZUME_TURN_RELAY_PORTS)")
	fs.StringVar(&c.TURNRelayIP, "turn-relay-ip", env("TURN_RELAY_IP", ""), "fixed public IPv4 for relayed addresses instead of resolving -public-host (UZUME_TURN_RELAY_IP)")
	fs.StringVar(&c.TrustedProxy, "trusted-proxy", env("TRUSTED_PROXY", ""), "CIDRs/IPs of reverse proxies in front of the server whose X-Forwarded-For/-Proto or Forwarded headers are believed; such requests are never local (UZUME_TRUSTED_PROXY)")
	child, _ := strconv.ParseBool(getenv("UZUME_CHILD"))
	fs.BoolVar(&c.Child, "child", child, "run as the app's child process: loopback only (the stored Network sharing setting is kept but not used), no self-update, exits when stdin closes, prints UZUME_READY <port> (UZUME_CHILD)")
	return c
}

// PluginIndex returns the effective plugin index URL: empty when the index is disabled ("none" or empty).
func (c *Config) PluginIndex() string {
	u := strings.TrimSpace(c.PluginIndexURL)
	if strings.EqualFold(u, "none") {
		return ""
	}
	return u
}

// DefaultToolsSourceURL is the GitHub releases API of the randomizer engine (same value as hub.DefaultToolsSourceURL).
const DefaultToolsSourceURL = "https://api.github.com/repos/upr-fvx/universal-pokemon-randomizer-fvx/releases"

// ToolsSource returns the effective tools source URL: empty when the source is disabled ("none" or empty).
func (c *Config) ToolsSource() string {
	u := strings.TrimSpace(c.ToolsSourceURL)
	if strings.EqualFold(u, "none") {
		return ""
	}
	return u
}

// ImportDir returns the library import folder: the configured one, else <data-dir>/library-import.
func (c *Config) ImportDir() string {
	if c.LibraryImportDir != "" {
		return c.LibraryImportDir
	}
	return filepath.Join(c.DataDir, "library-import")
}

// TrustedCoreKeys returns the compiled-in trusted keys plus the configured ones (updates index).
func (c *Config) TrustedCoreKeys() ([]ed25519.PublicKey, error) {
	return corepkg.TrustedKeys(c.CoreTrustKeys)
}

// Validate checks the configuration for contradictions.
func (c *Config) Validate() error {
	if c.DataDir == "" {
		return errors.New("data-dir must not be empty")
	}
	if (c.TLSCert == "") != (c.TLSKey == "") {
		return errors.New("specify tls-cert and tls-key together or not at all")
	}
	if c.Dev && c.TLSCert != "" {
		return errors.New("-dev (HTTP) and tls-cert/tls-key are mutually exclusive")
	}
	for _, u := range c.ICEServers {
		if !strings.HasPrefix(u, "stun:") || len(u) == len("stun:") {
			return fmt.Errorf("ice-servers: %q is not a stun: URL", u)
		}
	}
	if c.CloudflareTURNKeyID != "" {
		for _, r := range c.CloudflareTURNKeyID {
			if (r < 'a' || r > 'z') && (r < 'A' || r > 'Z') && (r < '0' || r > '9') && r != '-' && r != '_' {
				return errors.New("cloudflare-turn-key-id contains invalid characters")
			}
		}
	} else if c.CloudflareTURNTokenFile != "" {
		return errors.New("-cloudflare-turn-token-file needs -cloudflare-turn-key-id")
	}
	if _, err := c.ExternalTURNSpecs(); err != nil {
		return err
	}
	if _, err := clientip.ParseList(c.TrustedProxy); err != nil {
		return err
	}
	if _, _, err := SplitPublicHost(c.PublicHost); err != nil {
		return errors.New("public-host must be a host name or IP address with an optional port (host[:port], IPv6 in brackets)")
	}
	if c.TURN {
		if c.PublicHost == "" {
			return errors.New("-turn needs -public-host")
		}
		if c.TURNPort < 1 || c.TURNPort > 65535 {
			return errors.New("turn-port must be between 1 and 65535")
		}
		if _, _, err := c.TURNRelayRange(); err != nil {
			return err
		}
		if c.TURNRelayIP != "" {
			if ip := net.ParseIP(c.TURNRelayIP); ip == nil || ip.To4() == nil {
				return fmt.Errorf("turn-relay-ip: %q is not an IPv4 address", c.TURNRelayIP)
			}
		}
	}
	for name, v := range map[string]string{"core-buildbot-url": c.CoreBuildbotURL, "core-info-url": c.CoreInfoURL} {
		if u, err := url.Parse(v); err != nil || u.Scheme != "https" || u.Host == "" {
			return fmt.Errorf("%s must be an https URL", name)
		}
	}
	if err := updates.ValidateIndexURL(c.UpdateIndexURL); err != nil {
		return fmt.Errorf("update-index-url: %w", err)
	}
	if u := c.PluginIndex(); u != "" {
		if err := updates.ValidateIndexURL(u); err != nil {
			return fmt.Errorf("plugin-index-url: %w", err)
		}
	}
	if u := c.ToolsSource(); u != "" {
		if pu, err := url.Parse(u); err != nil || pu.Scheme != "https" || pu.Host == "" || pu.User != nil {
			return errors.New("tools-source-url must be an https URL or none")
		}
	}
	if c.SaveKeepRecent < 0 || c.SaveKeepDaily < 0 || c.SaveKeepWeekly < 0 {
		return errors.New("save-keep-recent, save-keep-daily and save-keep-weekly must not be negative")
	}
	if c.UpdateRequestDir == "" && !c.requestDirSet {
		c.UpdateRequestDir = DefaultUpdateRequestDir(c.goos, c.DataDir)
	}
	if c.UpdateRequestDir == "" {
		return errors.New("update-request-dir must not be empty")
	}
	if _, err := c.TrustedCoreKeys(); err != nil {
		return fmt.Errorf("core-trust-key: %w", err)
	}
	return nil
}

// UseTLS reports whether the hub serves HTTPS. HTTP is available only with -dev.
func (c *Config) UseTLS() bool { return !c.Dev }

// ListenIsLoopback reports whether the listen address binds to loopback only.
func (c *Config) ListenIsLoopback() bool {
	host, _, err := net.SplitHostPort(c.Listen)
	if err != nil || host == "" {
		return false
	}
	if host == "localhost" {
		return true
	}
	ip := net.ParseIP(host)
	return ip != nil && ip.IsLoopback()
}
