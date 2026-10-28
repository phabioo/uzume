package hub

import (
	"context"
	"fmt"
	"sort"

	"github.com/phabioo/uzume/server/internal/pluginpkg"
	"github.com/phabioo/uzume/server/internal/updates"
)

// runPluginChange is one entry of the run event plugins.updated. From is empty for a plugin that was appended.
type runPluginChange struct {
	PluginID string `json:"plugin_id"`
	From     string `json:"from"`
	To       string `json:"to"`
}

// SetRunPlugins switches the pinned versions of plugins of a run (feature run_plugin_updates_v1). Allowed for the
// creator and for a server admin (admin true); another participant gets ErrForbidden, everybody else ErrNotFound. A
// run that is over is a conflict. Rules, in this order: references valid, each id once, version offered (400);
// upgrade blocked by the new manifest's run_update_from (409 plugin_update_incompatible); downgrade only to the
// plugin's previous_version (409 plugin_update_incompatible); an id the run lacks only when a plugin of the resulting
// set requires it (400); every `requires` of the resulting set met (409 plugin_requires_missing). Unchanged versions
// write nothing. A change writes the run event plugins.updated and pushes run_updated and run_event.
func (s *Service) SetRunPlugins(ctx context.Context, actorID, runID string, refs []RunPluginRef, admin bool) (_ Run, err error) {
	defer s.publishOK(&err, TopicRuns)
	actorName := userDisplayName(ctx, s.db, actorID)
	// Lock order as in CreateRun: runs.mu, then plug.mu. Holding plug.mu from the catalog check to the commit means no
	// prune or removal can delete a version between the check and the commit.
	s.runs.mu.Lock()
	defer s.runs.mu.Unlock()
	s.plug.mu.Lock()
	defer s.plug.mu.Unlock()
	// All reads (run, catalog, manifests) happen before the transaction opens: the database has one connection.
	// runs.mu keeps other run changes out meanwhile.
	run, err := loadRun(ctx, s.db, runID, true)
	if err != nil {
		return Run{}, err
	}
	if !admin {
		if _, ok := run.Participant(actorID); !ok {
			return Run{}, ErrNotFound
		}
		if run.CreatedBy != actorID {
			return Run{}, &Error{Code: CodeForbidden, Message: "Only the creator of the run or an admin can update its plugins"}
		}
	}
	if run.Finished() {
		return Run{}, conflict("The run is over")
	}

	// 400: reference, duplicates, offered.
	if len(refs) < 1 || len(refs) > MaxRunPlugins {
		return Run{}, badRequest("Between 1 and %d plugins can be updated at once", MaxRunPlugins)
	}
	cat := s.pluginCatalog()
	seen := map[string]bool{}
	shas := map[string]string{}
	for _, p := range refs {
		if !runPluginIDRe.MatchString(p.PluginID) || len(p.PluginID) > 128 || p.Version == "" || len(p.Version) > 64 {
			return Run{}, badRequest("Invalid plugin reference")
		}
		if seen[p.PluginID] {
			return Run{}, badRequest("Plugin %s is listed twice", p.PluginID)
		}
		seen[p.PluginID] = true
		if cat == nil {
			return Run{}, badRequest("Plugin %s %s is not offered by this server", p.PluginID, p.Version)
		}
		sha, ok, err := cat.OfferedPlugin(ctx, p.PluginID, p.Version)
		if err != nil {
			return Run{}, err
		}
		if !ok {
			return Run{}, badRequest("Plugin %s %s is not offered by this server", p.PluginID, p.Version)
		}
		shas[p.PluginID] = sha
	}

	// Switch each listed plugin; collect the resulting set.
	cur := map[string]*RunPlugin{}
	result := make([]RunPlugin, len(run.Plugins))
	for i, p := range run.Plugins {
		result[i] = p
		cur[p.PluginID] = &result[i]
	}
	var changes []runPluginChange
	var appended []string
	for _, ref := range refs {
		pinned, has := cur[ref.PluginID]
		if !has {
			appended = append(appended, ref.PluginID)
			continue
		}
		if pinned.Version == ref.Version {
			continue
		}
		newV, errN := updates.ParseSemVer(ref.Version)
		oldV, errO := updates.ParseSemVer(pinned.Version)
		if errN != nil || errO != nil {
			return Run{}, badRequest("Invalid plugin reference")
		}
		if newV.Compare(oldV) < 0 {
			if pinned.PreviousVersion != ref.Version {
				return Run{}, &Error{Code: CodePluginUpdateIncompatible, Message: fmt.Sprintf(
					"%s %s cannot be set: only the previous version of the run's plugin can be restored", ref.PluginID, ref.Version)}
			}
		} else {
			np, err := s.GetPlugin(ctx, ref.PluginID, ref.Version)
			if err != nil {
				return Run{}, err
			}
			if from := np.Manifest.RunUpdateFrom; from != "" {
				if c, err := updates.CompareVersions(pinned.Version, from); err == nil && c < 0 {
					return Run{}, &Error{Code: CodePluginUpdateIncompatible, Message: fmt.Sprintf(
						"%s %s can only take over a running run from %s %s or newer (the run has %s)", ref.PluginID, ref.Version, ref.PluginID, from, pinned.Version)}
				}
			}
		}
		changes = append(changes, runPluginChange{PluginID: ref.PluginID, From: pinned.Version, To: ref.Version})
		pinned.PreviousVersion, pinned.PreviousSHA256 = pinned.Version, pinned.SHA256
		pinned.Version, pinned.SHA256 = ref.Version, shas[ref.PluginID]
	}
	versionOf := map[string]string{}
	for _, p := range refs {
		versionOf[p.PluginID] = p.Version
	}
	for _, id := range appended {
		result = append(result, RunPlugin{PluginID: id, Version: versionOf[id], SHA256: shas[id]})
		changes = append(changes, runPluginChange{PluginID: id, To: versionOf[id]})
	}
	if len(changes) == 0 {
		return viewFor(run, actorID), nil
	}
	if len(result) > MaxRunPlugins {
		return Run{}, badRequest("A run has at most %d plugins", MaxRunPlugins)
	}

	// requires of the resulting set: an appended id must be required by a plugin of the set; every requirement met.
	byID := map[string]string{}
	for _, p := range result {
		byID[p.PluginID] = p.Version
	}
	type readPlugin struct {
		p  RunPlugin
		pl Plugin
		ok bool
	}
	read := make([]readPlugin, len(result))
	required := map[string]bool{}
	for i, p := range result {
		pl, err := s.GetPlugin(ctx, p.PluginID, p.Version)
		read[i] = readPlugin{p, pl, err == nil && pl.Manifest.ID != ""} // unreadable manifest: skip the check
		if read[i].ok {
			for id := range pl.Manifest.Requires {
				required[id] = true
			}
		}
	}
	for _, id := range appended {
		if !required[id] {
			return Run{}, badRequest("Plugin %s is not part of the run and no plugin of the run requires it", id)
		}
	}
	for _, rp := range read {
		if !rp.ok {
			continue
		}
		ids := make([]string, 0, len(rp.pl.Manifest.Requires))
		for id := range rp.pl.Manifest.Requires {
			ids = append(ids, id)
		}
		sort.Strings(ids)
		for _, id := range ids {
			rng := rp.pl.Manifest.Requires[id]
			v, ok := byID[id]
			if !ok || !pluginpkg.RangeMatches(rng, v) {
				have := "it is not in the run"
				if ok {
					have = "the run has " + v
				}
				return Run{}, &Error{Code: CodePluginRequiresMissing, Message: fmt.Sprintf(
					"%s %s requires %s %s, but %s", rp.p.PluginID, rp.p.Version, id, rng, have)}
			}
		}
	}

	now := s.now().UTC()
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return Run{}, internal(err)
	}
	defer tx.Rollback()
	for i, p := range result {
		if i < len(run.Plugins) {
			if p.Version == run.Plugins[i].Version && p.PreviousVersion == run.Plugins[i].PreviousVersion {
				continue
			}
			if _, err := tx.ExecContext(ctx, `UPDATE run_plugins SET version = ?, sha256 = ?, previous_version = ?, previous_sha256 = ? WHERE run_id = ? AND plugin_id = ?`,
				p.Version, p.SHA256, p.PreviousVersion, p.PreviousSHA256, runID, p.PluginID); err != nil {
				return Run{}, internal(err)
			}
			continue
		}
		if _, err := tx.ExecContext(ctx, `INSERT INTO run_plugins(run_id, plugin_id, version, sha256, position) VALUES (?, ?, ?, ?, ?)`,
			runID, p.PluginID, p.Version, p.SHA256, i); err != nil {
			return Run{}, internal(err)
		}
	}
	if _, err := tx.ExecContext(ctx, `UPDATE runs SET updated_at = ? WHERE id = ?`, now.Unix(), runID); err != nil {
		return Run{}, internal(err)
	}
	ev, err := insertEvent(ctx, tx, runID, hubEvent("plugins.updated", map[string]any{
		"changes": changes, "by_user_id": actorID, "by_user_name": actorName}), now)
	if err != nil {
		return Run{}, err
	}
	if err := tx.Commit(); err != nil {
		return Run{}, internal(err)
	}
	run, err = loadRun(ctx, s.db, runID, true)
	if err != nil {
		return Run{}, err
	}
	s.pushRunUpdated(run)
	s.pushRunEvents(ctx, runID, []RunEvent{ev})
	return viewFor(run, actorID), nil
}
