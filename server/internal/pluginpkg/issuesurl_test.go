package pluginpkg

import "testing"

func TestManifestIssuesURL(t *testing.T) {
	const base = `{"api":1,"id":"net.example.demo","name":"Demo","version":"1.0.0","entry":"main.lua"`
	for _, c := range []struct {
		name, extra, want string
		warn              bool
	}{
		{"absent", ``, "", false},
		{"null", `,"issues_url":null`, "", false},
		{"valid", `,"issues_url":"https://github.com/example-org/demo.plugin"`, "https://github.com/example-org/demo.plugin", false},
		{"http", `,"issues_url":"http://github.com/example-org/demo"`, "", true},
		{"other host", `,"issues_url":"https://gitlab.com/example-org/demo"`, "", true},
		{"issue url", `,"issues_url":"https://github.com/example-org/demo/issues/3"`, "", true},
		{"trailing slash", `,"issues_url":"https://github.com/example-org/demo/"`, "", true},
		{"not a string", `,"issues_url":5`, "", true},
	} {
		t.Run(c.name, func(t *testing.T) {
			m, err := ParseManifest([]byte(base + c.extra + `}`))
			if err != nil {
				t.Fatalf("the plugin must still load: %v", err)
			}
			if m.IssuesURL != c.want || (len(m.Warnings) > 0) != c.warn {
				t.Fatalf("IssuesURL %q warnings %v", m.IssuesURL, m.Warnings)
			}
		})
	}
}
