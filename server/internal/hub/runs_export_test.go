package hub

import "context"

// SetRunLastSeqForTest moves the event counter of a run (to reach the event limit without 50 000 appends).
func SetRunLastSeqForTest(s *Service, runID string, n int64) error {
	_, err := s.db.ExecContext(context.Background(), `UPDATE runs SET last_seq = ? WHERE id = ?`, n, runID)
	return err
}

// RunSeedForTest reads the hidden run seed of a randomizer run.
func RunSeedForTest(s *Service, runID string) (int64, error) {
	var seed int64
	err := s.db.QueryRowContext(context.Background(), `SELECT run_seed FROM run_randomizer WHERE run_id = ?`, runID).Scan(&seed)
	return seed, err
}
