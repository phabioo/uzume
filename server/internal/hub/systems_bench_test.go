package hub

import (
	"context"
	"path/filepath"
	"testing"

	"github.com/phabioo/uzume/server/internal/store"
)

func BenchmarkListRegistry(b *testing.B) {
	db, err := store.Open(filepath.Join(b.TempDir(), "uzume.db"))
	if err != nil {
		b.Fatal(err)
	}
	b.Cleanup(func() { db.Close() })
	ctx := context.Background()
	if err := reconcileSystems(ctx, db); err != nil {
		b.Fatal(err)
	}
	s := &Service{db: db}
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		if _, err := s.ListRegistry(ctx); err != nil {
			b.Fatal(err)
		}
	}
}
