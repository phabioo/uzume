package main

import (
	"crypto/ed25519"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/phabioo/uzume/server/internal/corepkg"
	"github.com/phabioo/uzume/server/internal/pluginpkg"
)

// pluginSign validates a .uzplugin file and writes it with SIGNATURE.json added or replaced.
func pluginSign(in, out string, getenv func(string) string) error {
	if in == "" || out == "" {
		return errors.New("-in and -out are required")
	}
	seed, err := seedFromEnv(getenv)
	if err != nil {
		return err
	}
	data, err := os.ReadFile(in)
	if err != nil {
		return err
	}
	signed, err := pluginpkg.Sign(data, seed)
	if err != nil {
		return err
	}
	return writeFileAtomic(out, signed)
}

func writeFileAtomic(path string, data []byte) error {
	tmp, err := os.CreateTemp(filepath.Dir(path), ".plugin-sign-*")
	if err != nil {
		return err
	}
	name := tmp.Name()
	_, err = tmp.Write(data)
	if cerr := tmp.Close(); err == nil {
		err = cerr
	}
	if err == nil {
		err = os.Chmod(name, 0o644)
	}
	if err == nil {
		err = os.Rename(name, path)
	}
	if err != nil {
		os.Remove(name)
	}
	return err
}

// pluginAdd adds or replaces the entry of (id, version) in the plugin index (created when missing). The plugin
// must carry a valid signature of a trusted key (the compiled-in keys and, when UZUME_SIGNING_KEY is set, the
// key of that seed). Other entries are kept.
func pluginAdd(indexPath, pluginPath, rawURL string, allowFile, replace bool, getenv func(string) string, now func() time.Time) error {
	if indexPath == "" || pluginPath == "" || rawURL == "" {
		return errors.New("-index, -plugin and -url are required")
	}
	data, err := os.ReadFile(pluginPath)
	if err != nil {
		return err
	}
	p, err := pluginpkg.Open(data)
	if err != nil {
		return err
	}
	keys, err := corepkg.TrustedKeys(nil)
	if err != nil {
		return err
	}
	if getenv(signingKeyEnv) != "" {
		seed, err := seedFromEnv(getenv)
		if err != nil {
			return err
		}
		pub, err := corepkg.PublicFromSeed(seed)
		if err != nil {
			return err
		}
		keys = append(keys, ed25519.PublicKey(pub))
	}
	switch v := p.Verify(keys); v.State {
	case pluginpkg.SigValid:
	case pluginpkg.SigInvalid:
		return fmt.Errorf("plugin signature is invalid: %s", v.Reason)
	default:
		return errors.New("plugin is not signed; run plugin-sign first")
	}
	e := pluginpkg.EntryFor(p, rawURL)
	if err := pluginpkg.ValidateEntry(e, allowFile); err != nil {
		return fmt.Errorf("index entry: %w", err)
	}
	idx := pluginpkg.Index{Format: pluginpkg.IndexFormat}
	switch raw, err := os.ReadFile(indexPath); {
	case errors.Is(err, os.ErrNotExist):
	case err != nil:
		return err
	default:
		var errs []error
		idx, errs = pluginpkg.ParseIndex(raw, allowFile)
		if pluginpkg.Fatal(errs) {
			return fmt.Errorf("existing index is invalid: %w", errs[0])
		}
		for _, e := range errs { // entries invalid under the current rules are dropped
			fmt.Fprintln(warnOut, "Warning: dropping entry from the existing index:", e)
		}
	}
	entries := idx.Plugins[:0:0]
	for _, q := range idx.Plugins {
		if q.ID == e.ID && q.Version == e.Version {
			// A published id+version is immutable: run pins and server copies are keyed by it. The same file
			// (same sha256) only refreshes the entry; a different file needs an explicit -replace.
			if !strings.EqualFold(q.SHA256, e.SHA256) {
				if !replace {
					return fmt.Errorf("%s %s is already in the index with sha256 %s; this file has sha256 %s. A published version is immutable: bump the version, or pass -replace if you really mean to swap the file", e.ID, e.Version, q.SHA256, e.SHA256)
				}
				fmt.Fprintf(warnOut, "Warning: replacing %s %s (sha256 %s -> %s); servers and apps that already hold the old file keep it\n", e.ID, e.Version, q.SHA256, e.SHA256)
			}
			continue // replaced
		}
		entries = append(entries, q)
	}
	idx.Plugins = append(entries, e)
	pluginpkg.SortEntries(idx.Plugins)
	idx.GeneratedAt = now().UTC().Truncate(time.Second)
	idx.ExpiresAt = idx.GeneratedAt.Add(pluginpkg.IndexValidity)
	b, err := pluginpkg.MarshalIndex(idx)
	if err != nil {
		return err
	}
	return os.WriteFile(indexPath, b, 0o644)
}
