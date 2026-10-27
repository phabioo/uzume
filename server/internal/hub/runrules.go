package hub

import (
	"context"
	"encoding/json"
)

// SetRunRules replaces the core rules of a run (the whole object). Allowed for the creator and for a server admin
// (admin true); another participant gets ErrForbidden, everybody else ErrNotFound. A run that is over is a conflict.
// Unchanged rules write nothing. A change writes the run event rules.changed with {before, after} and pushes
// run_updated and run_event to the participants.
func (s *Service) SetRunRules(ctx context.Context, actorID, runID string, rules RunRules, admin bool) (_ Run, err error) {
	defer s.publishOK(&err, TopicRuns)
	if err := rules.Validate(); err != nil {
		return Run{}, err
	}
	actorName := userDisplayName(ctx, s.db, actorID)
	s.runs.mu.Lock()
	defer s.runs.mu.Unlock()
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return Run{}, internal(err)
	}
	defer tx.Rollback()
	run, err := loadRun(ctx, tx, runID, true)
	if err != nil {
		return Run{}, err
	}
	if !admin {
		if _, ok := run.Participant(actorID); !ok {
			return Run{}, ErrNotFound
		}
		if run.CreatedBy != actorID {
			return Run{}, &Error{Code: CodeForbidden, Message: "Only the creator of the run or an admin can change its rules"}
		}
	}
	if run.Finished() {
		return Run{}, conflict("The run is over")
	}
	if run.Rules == rules {
		return viewFor(run, actorID), nil
	}
	rulesJSON, _ := json.Marshal(rules)
	now := s.now().UTC()
	if _, err := tx.ExecContext(ctx, `UPDATE runs SET rules = ?, updated_at = ? WHERE id = ?`, string(rulesJSON), now.Unix(), runID); err != nil {
		return Run{}, internal(err)
	}
	ev, err := insertEvent(ctx, tx, runID, hubEvent("rules.changed", map[string]any{
		"before": run.Rules, "after": rules, "by_user_id": actorID, "by_user_name": actorName}), now)
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
