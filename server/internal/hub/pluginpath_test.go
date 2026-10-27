package hub

import (
	"path/filepath"
	"testing"
)

func TestPluginPathRefusesCraftedRefs(t *testing.T) {
	s := &Service{dataDir: t.TempDir()}
	if p, err := s.pluginPath("org.example.demo", "1.2.3"); err != nil || filepath.Base(p) != "1.2.3.uzplugin" {
		t.Fatalf("valid ref: %q %v", p, err)
	}
	for _, c := range [][2]string{{"../x", "1.0.0"}, {"org.example.demo", "../x"}, {"..", "1.0.0"}, {"a/b", "1.0.0"}, {"", ""}} {
		if p, err := s.pluginPath(c[0], c[1]); err == nil {
			t.Errorf("pluginPath(%q, %q) = %q, want error", c[0], c[1], p)
		}
	}
}
