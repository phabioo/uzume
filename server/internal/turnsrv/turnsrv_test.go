package turnsrv

import (
	"context"
	"errors"
	"math/rand/v2"
	"net"
	"net/netip"
	"os"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/pion/turn/v4"
)

var secret = []byte("0123456789abcdef0123456789abcdef")

func TestCredentialsValidate(t *testing.T) {
	now := time.Unix(1_700_000_000, 0)
	u, p, exp := Issue(secret, "dev-1", now)
	if exp.Sub(now) != 12*time.Hour || !strings.HasSuffix(u, ":dev-1") {
		t.Fatalf("%s %v", u, exp)
	}
	if id, ok := Validate(secret, u, p, now.Add(time.Hour)); !ok || id != "dev-1" {
		t.Fatal("valid credentials rejected")
	}
	if _, ok := Validate(secret, u, p, exp); ok {
		t.Fatal("expired accepted")
	}
	if _, ok := Validate(secret, u, p+"x", now); ok {
		t.Fatal("bad password accepted")
	}
	if _, ok := Validate([]byte("another-secret-another-secret-00"), u, p, now); ok {
		t.Fatal("other secret accepted")
	}
	if _, ok := Validate(secret, "garbage", p, now); ok {
		t.Fatal("garbage accepted")
	}
}

func TestPeerAllowed(t *testing.T) {
	pub := net.ParseIP("203.0.113.5")
	lan := netip.MustParseAddr("192.168.1.20")
	lanOK := func(a netip.Addr) bool { return a == lan || a == netip.MustParseAddr("127.0.0.1") }
	for _, c := range []struct {
		peer string
		ok   bool
	}{{"127.0.0.1", false}, {"::1", false}, {"0.0.0.0", false}, {"224.0.0.1", false}, {"169.254.1.1", false},
		{"fe80::1", false}, {"ff02::1", false}, {"192.0.2.10", true}, {"198.51.100.7", true}, {"2001:db8::1", true},
		{"10.1.2.3", false}, {"172.16.0.1", false}, {"172.31.255.255", false}, {"172.32.0.1", true}, {"192.168.0.7", false},
		{"100.64.0.1", false}, {"100.127.255.255", false}, {"100.128.0.1", true}, {"0.1.2.3", false}, {"192.0.0.8", false},
		{"198.18.0.1", false}, {"198.19.255.255", false}, {"198.20.0.1", true}, {"240.0.0.1", false}, {"255.255.255.255", false},
		{"fc00::1", false}, {"fd12:3456::1", false}, {"::ffff:10.1.2.3", false}, {"::ffff:192.168.0.7", false},
		{"::ffff:100.64.0.1", false}, {"::ffff:192.0.2.10", true},
		{"192.168.1.20", true}, {"::ffff:192.168.1.20", true}} {
		if got := PeerAllowed(pub, net.ParseIP(c.peer), lanOK); got != c.ok {
			t.Errorf("%s: %v", c.peer, got)
		}
	}
	if PeerAllowed(pub, net.ParseIP("192.168.1.20"), nil) {
		t.Error("nil LANPeerOK must refuse internal peers")
	}
	if PeerAllowed(pub, net.ParseIP("127.0.0.1"), lanOK) {
		t.Error("allowlisted loopback must stay refused")
	}
	if !PeerAllowed(net.ParseIP("127.0.0.1"), net.ParseIP("127.0.0.1"), nil) {
		t.Error("loopback relay must allow loopback peers")
	}
}

func TestHostFor(t *testing.T) {
	for _, c := range []struct{ req, pub, want string }{
		{"hub.example.org:8443", "x", "hub.example.org"},
		{"hub.example.org", "x", "hub.example.org"},
		{"192.0.2.1:8443", "x", "192.0.2.1"},
		{"[2001:db8::1]:8443", "x", "[2001:db8::1]"},
		{"[2001:db8::1]", "x", "[2001:db8::1]"},
		{"", "hub.example.org", "hub.example.org"},
		{"", "2001:db8::2", "[2001:db8::2]"},
	} {
		if got := HostFor(c.req, c.pub); got != c.want {
			t.Errorf("%q: %q want %q", c.req, got, c.want)
		}
	}
	u := URLs("[2001:db8::1]", 3478)
	if u[0] != "turn:[2001:db8::1]:3478?transport=udp" || u[1] != "turn:[2001:db8::1]:3478?transport=tcp" {
		t.Fatal(u)
	}
	if STUNURL("hub.example.org", 3478) != "stun:hub.example.org:3478" {
		t.Fatal()
	}
}

func TestResolveUpdatesAndWarnsOnce(t *testing.T) {
	ips := []net.IP{net.ParseIP("203.0.113.1")}
	var rerr error
	rmin, rmax := relayRange(t)
	s, err := Start(context.Background(), Config{PublicHost: "hub.example.org", RelayMin: rmin, RelayMax: rmax, Secret: secret,
		ListenHost: "127.0.0.1", DeviceOK: func(context.Context, string) bool { return true },
		Resolver: func(context.Context, string) ([]net.IP, error) { return ips, rerr }, ResolveInterval: time.Hour})
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	if s.Status().RelayIP != "203.0.113.1" {
		t.Fatalf("%+v", s.Status())
	}
	ips, rerr = nil, errors.New("nxdomain")
	s.Resolve(context.Background())
	if s.Status().RelayIP != "" {
		t.Fatal("relay not dropped")
	}
	ips, rerr = []net.IP{net.ParseIP("2001:db8::1"), net.ParseIP("203.0.113.9")}, nil
	s.Resolve(context.Background())
	if s.Status().RelayIP != "203.0.113.9" {
		t.Fatalf("%+v", s.Status())
	}
}

// relayRange returns a 40-port UDP relay range that is currently free and lies below every OS ephemeral range
// (Windows 49152-65535, Linux 32768-60999), so concurrently running tests that bind port 0 cannot take its ports.
func relayRange(t *testing.T) (int, int) {
	t.Helper()
	const span = 40
	for try := 0; try < 50; try++ {
		lo := 20000 + rand.IntN(9000)
		var held []net.PacketConn
		free := true
		for p := lo; p < lo+span; p++ {
			c, err := net.ListenPacket("udp4", net.JoinHostPort("127.0.0.1", itoa(p)))
			if err != nil {
				free = false
				break
			}
			held = append(held, c)
		}
		for _, c := range held {
			c.Close()
		}
		if free {
			return lo, lo + span - 1
		}
	}
	t.Skip("no free UDP relay range found")
	return 0, 0
}

func startLoopback(t *testing.T, deviceOK func(string) bool, relayIP net.IP, now func() time.Time) *Server {
	t.Helper()
	rmin, rmax := relayRange(t)
	s, err := Start(context.Background(), Config{PublicHost: "127.0.0.1", RelayIP: relayIP, RelayMin: rmin, RelayMax: rmax,
		Secret: secret, ListenHost: "127.0.0.1", Now: now,
		DeviceOK: func(_ context.Context, id string) bool { return deviceOK(id) }})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { s.Close() })
	return s
}

func allocate(t *testing.T, s *Server, user, cred string) error {
	t.Helper()
	conn, err := net.ListenPacket("udp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	c, err := turn.NewClient(&turn.ClientConfig{STUNServerAddr: net.JoinHostPort("127.0.0.1", itoa(s.Port())),
		TURNServerAddr: net.JoinHostPort("127.0.0.1", itoa(s.Port())), Username: user, Password: cred, Realm: Realm, Conn: conn})
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	if err := c.Listen(); err != nil {
		t.Fatal(err)
	}
	rc, err := c.Allocate()
	if err != nil {
		return err
	}
	defer rc.Close()
	if a := rc.LocalAddr().(*net.UDPAddr); a.Port < s.cfg.RelayMin || a.Port > s.cfg.RelayMax || !a.IP.Equal(net.ParseIP("127.0.0.1")) {
		t.Errorf("relay addr %v", a)
	}
	return nil
}

func itoa(n int) string { return strconv.Itoa(n) }

func TestAllocationWithIssuedCredentials(t *testing.T) {
	revoked := map[string]bool{"revoked": true}
	s := startLoopback(t, func(id string) bool { return id == "dev-ok" || id == "revoked" && !revoked[id] }, net.ParseIP("127.0.0.1"), nil)
	c := s.Credentials("127.0.0.1:8443", "dev-ok")
	if err := allocate(t, s, c.Username, c.Credential); err != nil {
		t.Fatalf("valid: %v", err)
	}
	if err := allocate(t, s, c.Username, "wrong"); err == nil {
		t.Fatal("wrong password allocated")
	}
	r := s.Credentials("", "revoked")
	if err := allocate(t, s, r.Username, r.Credential); err == nil {
		t.Fatal("revoked device allocated")
	}
	u := s.Credentials("", "unknown")
	if err := allocate(t, s, u.Username, u.Credential); err == nil {
		t.Fatal("unknown device allocated")
	}
}

func TestAllocationExpiredAndNoRelayIP(t *testing.T) {
	past := time.Now().Add(-13 * time.Hour)
	s := startLoopback(t, func(string) bool { return true }, net.ParseIP("127.0.0.1"), nil)
	u, p, _ := Issue(secret, "dev", past)
	if err := allocate(t, s, u, p); err == nil {
		t.Fatal("expired allocated")
	}
	// No resolved IPv4: allocations fail.
	rmin, rmax := relayRange(t)
	s2, err := Start(context.Background(), Config{PublicHost: "hub.example.org", RelayMin: rmin, RelayMax: rmax, Secret: secret,
		ListenHost: "127.0.0.1", DeviceOK: func(context.Context, string) bool { return true },
		Resolver: func(context.Context, string) ([]net.IP, error) { return nil, errors.New("down") }})
	if err != nil {
		t.Fatal(err)
	}
	defer s2.Close()
	c := s2.Credentials("", "dev")
	if err := allocate(t, s2, c.Username, c.Credential); err == nil {
		t.Fatal("allocated without relay IP")
	}
}

func TestAuthRejectsFarFutureUsername(t *testing.T) {
	s := startLoopback(t, func(string) bool { return true }, net.ParseIP("127.0.0.1"), nil)
	u, p, _ := Issue(secret, "dev", time.Now().Add(48*time.Hour))
	if err := allocate(t, s, u, p); err == nil {
		t.Fatal("far-future username allocated")
	}
	u, p, _ = Issue(secret, "dev", time.Now())
	if err := allocate(t, s, u, p); err != nil {
		t.Fatalf("regular username: %v", err)
	}
}

func TestAllocationQuotaPerDevice(t *testing.T) {
	s := startLoopback(t, func(string) bool { return true }, net.ParseIP("127.0.0.1"), nil)
	open := func(user, cred string) error {
		conn, err := net.ListenPacket("udp4", "127.0.0.1:0")
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { conn.Close() })
		c, err := turn.NewClient(&turn.ClientConfig{STUNServerAddr: net.JoinHostPort("127.0.0.1", itoa(s.Port())),
			TURNServerAddr: net.JoinHostPort("127.0.0.1", itoa(s.Port())), Username: user, Password: cred, Realm: Realm, Conn: conn})
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(c.Close)
		if err := c.Listen(); err != nil {
			t.Fatal(err)
		}
		rc, err := c.Allocate()
		if err != nil {
			return err
		}
		t.Cleanup(func() { rc.Close() })
		return nil
	}
	a := s.Credentials("", "dev-a")
	for i := 0; i < MaxAllocationsPerDevice; i++ {
		if err := open(a.Username, a.Credential); err != nil {
			t.Fatalf("allocation %d: %v", i+1, err)
		}
	}
	if err := open(a.Username, a.Credential); err == nil {
		t.Fatal("allocation over the per-device quota succeeded")
	}
	b := s.Credentials("", "dev-b")
	if err := open(b.Username, b.Credential); err != nil {
		t.Fatalf("other device: %v", err)
	}
}

func TestLimitListenerCapsConnections(t *testing.T) {
	inner, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	ln := &limitListener{Listener: inner, sem: make(chan struct{}, 2)}
	defer ln.Close()
	accepted := make(chan net.Conn, 8)
	go func() {
		for {
			c, err := ln.Accept()
			if err != nil {
				return
			}
			accepted <- c
		}
	}()
	dial := func() net.Conn {
		c, err := net.Dial("tcp", inner.Addr().String())
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { c.Close() })
		return c
	}
	c1, c2, c3 := dial(), dial(), dial()
	_ = c1
	_ = c2
	// The third connection is closed by the server right after Accept.
	c3.SetReadDeadline(time.Now().Add(2 * time.Second))
	if _, err := c3.Read(make([]byte, 1)); err == nil || errors.Is(err, os.ErrDeadlineExceeded) {
		t.Fatalf("excess connection not closed: %v", err)
	}
	s1, s2 := <-accepted, <-accepted
	select {
	case <-accepted:
		t.Fatal("third connection handed out")
	default:
	}
	// Closing one frees a slot.
	s1.Close()
	s1.Close() // idempotent
	c4 := dial()
	select {
	case s4 := <-accepted:
		s4.Close()
	case <-time.After(2 * time.Second):
		t.Fatal("slot not released")
	}
	_, _ = s2, c4
}

// Windows keeps separate excluded port ranges for TCP and UDP; the port-0 bind must not get stuck in one of them.
func TestBindPairRotatesStrategies(t *testing.T) {
	refuse := errors.New("bind: forbidden")
	for _, tc := range []string{"udp", "tcp"} {
		t.Run(tc+" refuses ports of the other protocol's port 0", func(t *testing.T) {
			// The refusing protocol rejects every port that the other protocol's port 0 handed out.
			handed := map[int]bool{}
			listen := func(network, address string) (net.Listener, error) {
				ln, err := net.Listen(network, address)
				if err != nil {
					return nil, err
				}
				p := ln.Addr().(*net.TCPAddr).Port
				if tc == "tcp" && handed[p] {
					ln.Close()
					return nil, refuse
				}
				if tc == "udp" {
					handed[p] = true
				}
				return ln, nil
			}
			listenPacket := func(network, address string) (net.PacketConn, error) {
				pc, err := net.ListenPacket(network, address)
				if err != nil {
					return nil, err
				}
				p := pc.LocalAddr().(*net.UDPAddr).Port
				if tc == "udp" && handed[p] {
					pc.Close()
					return nil, refuse
				}
				if tc == "tcp" {
					handed[p] = true
				}
				return pc, nil
			}
			ok := false
			var last error
			for i := 0; i < 20 && !ok; i++ {
				ln, pc, err := bindPair("127.0.0.1", 0, i%3, listen, listenPacket)
				if err != nil {
					// refuse is the simulated exclusion; on a Windows runner a real one ("forbidden by its access permissions")
					// hits the same way, and the server's start loop rotates past both.
					last = err
					continue
				}
				if ln.Addr().(*net.TCPAddr).Port != pc.LocalAddr().(*net.UDPAddr).Port {
					t.Fatal("ports differ")
				}
				ln.Close()
				pc.Close()
				ok = true
			}
			if !ok {
				t.Fatalf("no attempt succeeded (last error: %v)", last)
			}
		})
	}
}
