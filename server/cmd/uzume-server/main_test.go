package main

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/phabioo/uzume/server/internal/hub/hubtest"
	"github.com/phabioo/uzume/server/internal/tlsutil"
)

func TestSetupAdminTwice(t *testing.T) {
	dir := t.TempDir()
	var out bytes.Buffer
	if err := runSetupAdmin([]string{"-data-dir", dir, "-username", "fabio"}, strings.NewReader("secret-12345\n"), &out); err != nil {
		t.Fatal(err)
	}
	if strings.Contains(out.String(), "secret") {
		t.Fatal("password in output")
	}
	err := runSetupAdmin([]string{"-data-dir", dir, "-username", "second"}, strings.NewReader("secret-12345\n"), &out)
	if err == nil {
		t.Fatal("second admin must be refused")
	}
	if err := runSetupAdmin([]string{"-data-dir", dir}, strings.NewReader("x\n"), &out); err == nil {
		t.Fatal("missing -username must fail")
	}
}

func TestRenewCert(t *testing.T) {
	dir := t.TempDir()
	var out strings.Builder
	if err := runRenewCert([]string{"-data-dir", dir}, &out); err == nil {
		t.Fatal("renew without an existing certificate must fail")
	}
	if _, err := tlsutil.EnsureSelfSigned(dir); err != nil {
		t.Fatal(err)
	}
	if err := runRenewCert([]string{"-data-dir", dir}, &out); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(out.String(), "New SHA-256 fingerprint: ") {
		t.Fatalf("output: %q", out.String())
	}
	if _, err := os.Stat(filepath.Join(dir, "tls", "cert.pem.prev")); err != nil {
		t.Fatal("backup missing")
	}
}

func TestRenewCertRefusesOwnCert(t *testing.T) {
	dir, own := t.TempDir(), t.TempDir()
	certFile, keyFile := filepath.Join(own, "c.pem"), filepath.Join(own, "k.pem")
	if err := os.WriteFile(certFile, []byte("own-cert"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(keyFile, []byte("own-key"), 0o600); err != nil {
		t.Fatal(err)
	}
	var out strings.Builder
	if err := runRenewCert([]string{"-data-dir", dir, "-tls-cert", certFile, "-tls-key", keyFile}, &out); err == nil {
		t.Fatal("must refuse with own cert/key")
	}
	if b, _ := os.ReadFile(certFile); string(b) != "own-cert" {
		t.Fatal("own cert modified")
	}
	if b, _ := os.ReadFile(keyFile); string(b) != "own-key" {
		t.Fatal("own key modified")
	}
	if _, err := os.Stat(filepath.Join(dir, "tls")); err == nil {
		t.Fatal("tls dir created")
	}
}

func TestImportCores(t *testing.T) {
	bb := hubtest.NewBuildbot(t)
	bb.AddCore(t, hubtest.BuildbotCore{ID: "desmume", SystemID: "nds", Lib: bytes.Repeat([]byte{7}, 64)})
	bb.AddCore(t, hubtest.BuildbotCore{ID: "azahar", SystemID: "gba"})
	in, data := t.TempDir(), t.TempDir()
	bb.WriteImportDir(t, in)

	var out strings.Builder
	// A core of a system the server does not support is reported and makes the command fail, the others are imported.
	// The directory may come before or after the flags.
	err := runImportCores([]string{in, "-data-dir", data}, &out)
	if err == nil || !strings.Contains(out.String(), "1 installed, 0 updated, 0 unchanged, 1 problem(s)") || !strings.Contains(out.String(), "azahar") {
		t.Fatalf("err %v summary %q", err, out.String())
	}
	if _, err := os.Stat(filepath.Join(data, "cores", "desmume", "2026.10.09", "linux-x64", "desmume_libretro.so")); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(data, "cores", "desmume", "2026.10.09", "windows-x64", "desmume_libretro.dll")); err != nil {
		t.Fatal(err)
	}
	// Again: nothing changes. Without the 3ds core the command succeeds.
	for _, p := range []string{"linux-x64", "windows-x64"} {
		suffix := map[string]string{"linux-x64": ".so", "windows-x64": ".dll"}[p]
		os.Remove(filepath.Join(in, p, "azahar_libretro"+suffix+".zip"))
	}
	out.Reset()
	if err := runImportCores([]string{"-data-dir", data, in}, &out); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(out.String(), "0 installed, 0 updated, 1 unchanged, 0 problem(s)") {
		t.Fatalf("second import: %q", out.String())
	}
	// A corrupt zip is rejected with its reason.
	bad := t.TempDir()
	os.MkdirAll(filepath.Join(bad, "linux-x64"), 0o755)
	os.WriteFile(filepath.Join(bad, "info.zip"), bb.InfoZip(t), 0o644)
	os.WriteFile(filepath.Join(bad, "linux-x64", "desmume_libretro.so.zip"), hubtest.MakeZip(t, map[string][]byte{"../desmume_libretro.so": {1}}), 0o644)
	out.Reset()
	if err := runImportCores([]string{bad, "-data-dir", t.TempDir()}, &out); err == nil || !strings.Contains(out.String(), "unsafe path") {
		t.Fatalf("err %v out %q", err, out.String())
	}
	if err := runImportCores([]string{"-data-dir", data}, &out); err == nil {
		t.Fatal("missing directory accepted")
	}
	if err := runImportCores([]string{in, "-data-dir", data, "-core-buildbot-url", "http://x"}, &out); err == nil {
		t.Fatal("http source accepted")
	}
}

func TestImportTools(t *testing.T) {
	in, data := t.TempDir(), t.TempDir()
	good := filepath.Join(in, "UPR_FVX-v1_6_2-Linux_x86.zip")
	if err := os.WriteFile(good, hubtest.ToolZip(t, "linux-x64", true, "x"), 0o600); err != nil {
		t.Fatal(err)
	}
	win := filepath.Join(in, "UPR_FVX-v1_6_2-Windows.zip")
	if err := os.WriteFile(win, hubtest.ToolZip(t, "windows-x64", false, "x"), 0o600); err != nil {
		t.Fatal(err)
	}
	var out strings.Builder
	// The files may come before or after the flags.
	if err := runImportTools([]string{good, "-data-dir", data, win}, &out); err != nil {
		t.Fatalf("%v %q", err, out.String())
	}
	if strings.Count(out.String(), "Added") != 2 || !strings.Contains(out.String(), "randomizer 1.6.2 linux-x64") || !strings.Contains(out.String(), "randomizer 1.6.2 windows-x64") {
		t.Fatalf("%q", out.String())
	}
	if _, err := os.Stat(filepath.Join(data, "tools", "randomizer", "1.6.2", "linux-x64.zip")); err != nil {
		t.Fatal(err)
	}
	out.Reset()
	if err := runImportTools([]string{"-data-dir", data, good}, &out); err != nil || !strings.Contains(out.String(), "Unchanged") {
		t.Fatalf("again: %v %q", err, out.String())
	}
	// A wrong name or content is rejected with its reason and fails the command.
	bad := filepath.Join(in, "engine.zip")
	os.WriteFile(bad, []byte("x"), 0o600)
	out.Reset()
	if err := runImportTools([]string{"-data-dir", data, bad, good}, &out); err == nil || !strings.Contains(out.String(), "Rejected") || !strings.Contains(out.String(), "file name") {
		t.Fatalf("%v %q", err, out.String())
	}
	if err := runImportTools([]string{"-data-dir", data}, &out); err == nil {
		t.Fatal("no files accepted")
	}
}
