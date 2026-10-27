// Command uzume-sign creates and signs the core index, the updates index and the plugin index of Uzume and
// signs .uzplugin files.
//
//	uzume-sign keygen -out <seed-file>
//	uzume-sign add -index <cores-index.json> -package <package.json>
//	uzume-sign release-add -index <updates-index.json> -release <release.json> [-keep 5]
//	uzume-sign plugin-sign -in <x.uzplugin> -out <y.uzplugin>      (adds/replaces SIGNATURE.json; seed from env UZUME_SIGNING_KEY)
//	uzume-sign plugin-add -index <plugin-index.json> -plugin <signed.uzplugin> -url <https url> [-replace]   (refuses a different file for an existing id+version unless -replace)
//	uzume-sign sign -index <cores-index.json|updates-index.json|plugin-index.json> [-out <file>.sig] [-bundle <file>.signed]   (seed from env UZUME_SIGNING_KEY)
//	uzume-sign index-refresh -index <updates-index.json|plugin-index.json>   (new generated_at/expires_at, content unchanged)
//	uzume-sign verify -index <file> (-sig <file> | -bundle <file>) -pub <base64 public key>
//	uzume-sign pubkey                                             (seed from env UZUME_SIGNING_KEY)
//
// The private key (seed) is never read from a flag and never printed.
package main

import (
	"bytes"
	"crypto/ed25519"
	"crypto/rand"
	"encoding/base64"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"time"

	"github.com/phabioo/uzume/server/internal/corepkg"
	"github.com/phabioo/uzume/server/internal/pluginpkg"
	"github.com/phabioo/uzume/server/internal/updates"
)

// warnOut receives non-fatal warnings (a variable for tests).
var warnOut io.Writer = os.Stderr

const signingKeyEnv = "UZUME_SIGNING_KEY"

func main() {
	if err := run(os.Args[1:], os.Getenv, os.Stdout, time.Now); err != nil {
		fmt.Fprintln(os.Stderr, "Error:", err)
		os.Exit(1)
	}
}

func run(args []string, getenv func(string) string, out io.Writer, now func() time.Time) error {
	if len(args) == 0 {
		return errors.New("usage: uzume-sign keygen|add|release-add|plugin-sign|plugin-add|sign|index-refresh|verify|pubkey [flags]")
	}
	fs := flag.NewFlagSet(args[0], flag.ContinueOnError)
	switch args[0] {
	case "keygen":
		o := fs.String("out", "", "file for the base64 seed (created 0600, never overwritten)")
		if err := fs.Parse(args[1:]); err != nil {
			return err
		}
		return keygen(*o, out)
	case "add":
		idx := fs.String("index", "", "cores-index.json (created when missing)")
		pkg := fs.String("package", "", "package JSON (same shape as an index entry)")
		if err := fs.Parse(args[1:]); err != nil {
			return err
		}
		return add(*idx, *pkg, now)
	case "release-add":
		idx := fs.String("index", "", "updates-index.json (created when missing)")
		rel := fs.String("release", "", "release JSON (one object, same shape as an index entry)")
		keep := fs.Int("keep", 5, "releases to keep per product and channel (newest by SemVer)")
		allowFile := fs.Bool("allow-file-urls", false, "accept file:// artifact URLs (tests only; default https only)")
		if err := fs.Parse(args[1:]); err != nil {
			return err
		}
		return releaseAdd(*idx, *rel, *keep, *allowFile, now)
	case "plugin-sign":
		in := fs.String("in", "", "input .uzplugin")
		o := fs.String("out", "", "output .uzplugin (may equal -in)")
		if err := fs.Parse(args[1:]); err != nil {
			return err
		}
		return pluginSign(*in, *o, getenv)
	case "plugin-add":
		idx := fs.String("index", "", "plugin-index.json (created when missing)")
		pl := fs.String("plugin", "", "signed .uzplugin")
		u := fs.String("url", "", "https URL where the file will be published")
		allowFile := fs.Bool("allow-file-urls", false, "accept a file:// -url (tests only; default https only)")
		replace := fs.Bool("replace", false, "replace an existing id+version whose sha256 differs (dangerous, see docs/guides/plugin-api.md)")
		if err := fs.Parse(args[1:]); err != nil {
			return err
		}
		return pluginAdd(*idx, *pl, *u, *allowFile, *replace, getenv, now)
	case "sign":
		idx := fs.String("index", "", "cores-index.json, updates-index.json or plugin-index.json (detected by content)")
		o := fs.String("out", "", "legacy signature file over the raw bytes (default <index>.sig; read by servers and apps before the ADR 0016 amendment 2026-10-19)")
		bundle := fs.String("bundle", "", "also write the single-file bundle (domain-separated signature line + index bytes), e.g. <index>.signed")
		allowFile := fs.Bool("allow-file-urls", false, "accept file:// artifact URLs in an updates index (tests only)")
		if err := fs.Parse(args[1:]); err != nil {
			return err
		}
		return sign(*idx, *o, *bundle, *allowFile, getenv)
	case "index-refresh":
		idx := fs.String("index", "", "updates-index.json or plugin-index.json: set generated_at and expires_at to now, nothing else")
		if err := fs.Parse(args[1:]); err != nil {
			return err
		}
		return indexRefresh(*idx, now)
	case "verify":
		idx := fs.String("index", "", "cores-index.json")
		sig := fs.String("sig", "", "legacy signature file")
		bundle := fs.String("bundle", "", "signed bundle (domain-separated; must carry exactly the bytes of -index)")
		pub := fs.String("pub", "", "base64 public key")
		if err := fs.Parse(args[1:]); err != nil {
			return err
		}
		return verify(*idx, *sig, *bundle, *pub, out)
	case "pubkey":
		if err := fs.Parse(args[1:]); err != nil {
			return err
		}
		seed, err := seedFromEnv(getenv)
		if err != nil {
			return err
		}
		return printKey(seed, out)
	}
	return fmt.Errorf("unknown command %q", args[0])
}

func seedFromEnv(getenv func(string) string) ([]byte, error) {
	v := getenv(signingKeyEnv)
	if v == "" {
		return nil, fmt.Errorf("environment variable %s is not set", signingKeyEnv)
	}
	return corepkg.ParseSeed(v)
}

func printKey(seed []byte, out io.Writer) error {
	pub, err := corepkg.PublicFromSeed(seed)
	if err != nil {
		return err
	}
	fmt.Fprintf(out, "public_key=%s\nkey_id=%s\n", base64.StdEncoding.EncodeToString(pub), corepkg.KeyID(pub))
	return nil
}

func keygen(path string, out io.Writer) error {
	if path == "" {
		return errors.New("-out is missing")
	}
	seed := make([]byte, ed25519.SeedSize)
	if _, err := rand.Read(seed); err != nil {
		return err
	}
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o600)
	if err != nil {
		return err
	}
	_, err = f.WriteString(base64.StdEncoding.EncodeToString(seed) + "\n")
	if cerr := f.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		os.Remove(path)
		return err
	}
	return printKey(seed, out)
}

func add(indexPath, pkgPath string, now func() time.Time) error {
	if indexPath == "" || pkgPath == "" {
		return errors.New("-index and -package are required")
	}
	raw, err := os.ReadFile(pkgPath)
	if err != nil {
		return err
	}
	var p corepkg.Package
	if err := json.Unmarshal(raw, &p); err != nil {
		return fmt.Errorf("package file: %w", err)
	}
	if err := corepkg.ValidatePackage(p); err != nil {
		return fmt.Errorf("package file: %w", err)
	}
	idx := corepkg.Index{Schema: corepkg.Schema}
	switch data, err := os.ReadFile(indexPath); {
	case errors.Is(err, os.ErrNotExist):
	case err != nil:
		return err
	default:
		var errs []error
		if idx, errs = corepkg.ParseIndex(data); len(errs) > 0 {
			return fmt.Errorf("existing index is invalid: %w", errs[0])
		}
	}
	replaced := false
	for i, q := range idx.Packages {
		if q.CoreID == p.CoreID && q.Version == p.Version && q.Platform == p.Platform {
			idx.Packages[i], replaced = p, true
		}
	}
	if !replaced {
		idx.Packages = append(idx.Packages, p)
	}
	corepkg.SortPackages(idx.Packages)
	idx.GeneratedAt = now().UTC().Truncate(time.Second)
	b, err := json.MarshalIndent(idx, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(indexPath, append(b, '\n'), 0o644)
}

// addLegacyCompanions appends, per hub channel, the legacy companion of the newest regular release (see
// updates.LegacyCompanion). rels must not contain companions.
func addLegacyCompanions(rels []updates.Release) []updates.Release {
	newest := map[string]updates.Release{}
	for _, q := range rels {
		if q.Product != updates.ProductHub {
			continue
		}
		if cur, ok := newest[q.Channel]; !ok || updates.MustCompare(q.Version, cur.Version) > 0 {
			newest[q.Channel] = q
		}
	}
	for _, q := range newest {
		if c, ok := updates.LegacyCompanion(q); ok {
			rels = append(rels, c)
		}
	}
	updates.SortReleases(rels)
	return rels
}

// releaseAdd adds or replaces one release in the updates index (created when missing), keeps the newest keep
// releases per (product, channel) by SemVer and writes deterministic JSON.
func releaseAdd(indexPath, relPath string, keep int, allowFile bool, now func() time.Time) error {
	if indexPath == "" || relPath == "" {
		return errors.New("-index and -release are required")
	}
	if keep < 1 {
		return errors.New("-keep must be at least 1")
	}
	raw, err := os.ReadFile(relPath)
	if err != nil {
		return err
	}
	var r updates.Release
	if err := json.Unmarshal(raw, &r); err != nil {
		return fmt.Errorf("release file: %w", err)
	}
	if err := updates.ValidateReleaseOpts(r, allowFile); err != nil {
		return fmt.Errorf("release file: %w", err)
	}
	idx := updates.Index{Schema: updates.Schema}
	switch data, err := os.ReadFile(indexPath); {
	case errors.Is(err, os.ErrNotExist):
	case err != nil:
		return err
	default:
		var errs []error
		idx, errs = updates.ParseIndexOpts(data, allowFile)
		if updates.Fatal(errs) {
			return fmt.Errorf("existing index is invalid: %w", errs[0])
		}
		for _, e := range errs { // entries invalid under the current rules (e.g. the old channel "test") are dropped
			fmt.Fprintln(warnOut, "Warning: dropping release from the existing index:", e)
		}
	}
	rels := idx.Releases[:0:0]
	for _, q := range idx.Releases {
		if q.Product == r.Product && q.Channel == r.Channel && q.Version == r.Version {
			continue // replaced
		}
		if q.Product == updates.ProductHub && updates.IsLegacyCompanion(q.Version) {
			continue // regenerated below, not counted by keep
		}
		rels = append(rels, q)
	}
	idx.Releases = updates.Prune(append(rels, r), keep)
	idx.Releases = addLegacyCompanions(idx.Releases)
	idx.GeneratedAt = now().UTC().Truncate(time.Second)
	idx.ExpiresAt = idx.GeneratedAt.Add(updates.IndexValidity)
	b, err := updates.Marshal(idx)
	if err != nil {
		return err
	}
	return os.WriteFile(indexPath, b, 0o644)
}

func sign(indexPath, outPath, bundlePath string, allowFile bool, getenv func(string) string) error {
	if indexPath == "" {
		return errors.New("-index is required")
	}
	if outPath == "" {
		outPath = indexPath + ".sig"
	}
	seed, err := seedFromEnv(getenv)
	if err != nil {
		return err
	}
	data, err := os.ReadFile(indexPath)
	if err != nil {
		return err
	}
	context, err := indexContext(data, allowFile)
	if err != nil {
		return err
	}
	line, err := corepkg.Sign(data, seed)
	if err != nil {
		return err
	}
	if err := os.WriteFile(outPath, line, 0o644); err != nil {
		return err
	}
	if bundlePath == "" {
		return nil
	}
	ctxLine, err := corepkg.SignContext(context, data, seed)
	if err != nil {
		return err
	}
	return os.WriteFile(bundlePath, corepkg.Bundle(ctxLine, data), 0o644)
}

// indexContext validates the index (type detected by content) and returns its signing context.
func indexContext(data []byte, allowFile bool) (string, error) {
	switch {
	case pluginpkg.LooksLikeIndex(data):
		if _, errs := pluginpkg.ParseIndex(data, allowFile); len(errs) > 0 {
			return "", fmt.Errorf("plugin index is invalid: %w", errs[0])
		}
		return corepkg.ContextPluginIndex, nil
	case updates.LooksLikeUpdatesIndex(data):
		if _, errs := updates.ParseIndexOpts(data, allowFile); len(errs) > 0 {
			return "", fmt.Errorf("updates index is invalid: %w", errs[0])
		}
		return corepkg.ContextUpdatesIndex, nil
	}
	if _, errs := corepkg.ParseIndex(data); len(errs) > 0 {
		return "", fmt.Errorf("index is invalid: %w", errs[0])
	}
	return corepkg.ContextCoresIndex, nil
}

// indexRefresh re-stamps generated_at and expires_at of an updates or plugin index (weekly workflow; the content is unchanged).
func indexRefresh(indexPath string, now func() time.Time) error {
	if indexPath == "" {
		return errors.New("-index is required")
	}
	data, err := os.ReadFile(indexPath)
	if err != nil {
		return err
	}
	stamp := now().UTC().Truncate(time.Second)
	var b []byte
	switch {
	case pluginpkg.LooksLikeIndex(data):
		idx, errs := pluginpkg.ParseIndex(data, false)
		if pluginpkg.Fatal(errs) {
			return fmt.Errorf("plugin index is invalid: %w", errs[0])
		}
		idx.GeneratedAt, idx.ExpiresAt = stamp, stamp.Add(pluginpkg.IndexValidity)
		b, err = pluginpkg.MarshalIndex(idx)
	case updates.LooksLikeUpdatesIndex(data):
		idx, errs := updates.ParseIndexOpts(data, false)
		if updates.Fatal(errs) {
			return fmt.Errorf("updates index is invalid: %w", errs[0])
		}
		idx.GeneratedAt, idx.ExpiresAt = stamp, stamp.Add(updates.IndexValidity)
		b, err = updates.Marshal(idx)
	default:
		return errors.New("index-refresh supports the updates index and the plugin index")
	}
	if err != nil {
		return err
	}
	return os.WriteFile(indexPath, b, 0o644)
}

func verify(indexPath, sigPath, bundlePath, pub string, out io.Writer) error {
	if indexPath == "" || (sigPath == "") == (bundlePath == "") || pub == "" {
		return errors.New("-index, -pub and exactly one of -sig and -bundle are required")
	}
	key, err := corepkg.ParsePublicKey(pub)
	if err != nil {
		return err
	}
	data, err := os.ReadFile(indexPath)
	if err != nil {
		return err
	}
	keys := []ed25519.PublicKey{key}
	if bundlePath != "" {
		b, err := os.ReadFile(bundlePath)
		if err != nil {
			return err
		}
		line, inner, err := corepkg.SplitBundle(b)
		if err != nil {
			return err
		}
		if !bytes.Equal(inner, data) {
			return errors.New("bundle does not contain the bytes of the index")
		}
		context, err := indexContext(data, true)
		if err != nil {
			return err
		}
		if err := corepkg.VerifyContext(context, inner, line, keys, false); err != nil {
			return err
		}
		fmt.Fprintf(out, "OK key_id=%s context=%s\n", corepkg.KeyID(key), context)
		return nil
	}
	sig, err := os.ReadFile(sigPath)
	if err != nil {
		return err
	}
	if err := corepkg.Verify(data, sig, keys); err != nil {
		return err
	}
	fmt.Fprintf(out, "OK key_id=%s\n", corepkg.KeyID(key))
	return nil
}
