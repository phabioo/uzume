package hubtest

import (
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"

	"github.com/phabioo/uzume/server/internal/hub"
)

// ToolAssetName is the release asset name of the randomizer engine for a version and an Uzume platform, for "1.6.2" and
// windows-x64: UPR_FVX-v1_6_2-Windows.zip.
func ToolAssetName(version, platform string) string {
	suffix := map[string]string{"windows-x64": "Windows", "linux-x64": "Linux_x86", "linux-arm64": "Linux_ARM"}[platform]
	return fmt.Sprintf("UPR_FVX-v%s-%s.zip", strings.ReplaceAll(version, ".", "_"), suffix)
}

// ToolZip builds a dummy engine zip (never a real engine): UPR-FVX.jar and the Java launcher, below one top folder
// when nested. content makes the bytes differ between files.
func ToolZip(t *testing.T, platform string, nested bool, content string) []byte {
	t.Helper()
	prefix := ""
	if nested {
		prefix = "UPR_FVX/"
	}
	java := "java/bin/java"
	if platform == "windows-x64" {
		java = "java/bin/java.exe"
	}
	return MakeZip(t, map[string][]byte{
		prefix + "UPR-FVX.jar": []byte("dummy jar " + content),
		prefix + java:          []byte("dummy java " + content),
		prefix + "data/x.txt":  []byte("data"),
	})
}

// ToolSource is a fake GitHub releases API with the release zips: an HTTPS test server.
type ToolSource struct {
	Server *httptest.Server
	// Down makes every request fail with 503.
	Down bool

	mu       sync.Mutex
	releases []toolRelease
	files    map[string][]byte // path -> zip
	Hits     map[string]int
}

type toolRelease struct {
	Tag        string
	Prerelease bool
	Draft      bool
	Assets     []toolAsset
}

type toolAsset struct{ Name, Path string }

// NewToolSource starts the fake source; it stops with the test.
func NewToolSource(t *testing.T) *ToolSource {
	t.Helper()
	s := &ToolSource{files: map[string][]byte{}, Hits: map[string]int{}}
	s.Server = httptest.NewTLSServer(http.HandlerFunc(s.serve))
	t.Cleanup(s.Server.Close)
	return s
}

// Apply points the server options at the fake source.
func (s *ToolSource) Apply(o *hub.Options) {
	o.ToolsSourceURL = s.Server.URL + "/releases"
	o.CoreHTTPClient = s.Server.Client()
}

// AddRelease lists a release with a zip per platform (the zip content is the version and platform unless files has it).
func (s *ToolSource) AddRelease(t *testing.T, version string, prerelease bool, platforms ...string) {
	t.Helper()
	s.mu.Lock()
	defer s.mu.Unlock()
	r := toolRelease{Tag: "v" + version, Prerelease: prerelease}
	for _, p := range platforms {
		name := ToolAssetName(version, p)
		path := "/dl/" + version + "/" + name
		s.files[path] = ToolZip(t, p, p == "linux-x64", version+p) // linux-x64 nests the files under one folder
		r.Assets = append(r.Assets, toolAsset{Name: name, Path: path})
	}
	s.releases = append([]toolRelease{r}, s.releases...)
}

// AddDraft lists a draft release (never used by the sync).
func (s *ToolSource) AddDraft(version string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.releases = append(s.releases, toolRelease{Tag: "v" + version, Draft: true,
		Assets: []toolAsset{{Name: ToolAssetName(version, "linux-x64"), Path: "/dl/draft"}}})
}

// Replace swaps the zip served for a version and platform (a changed file behind the same URL).
func (s *ToolSource) Replace(version, platform string, zip []byte) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.files["/dl/"+version+"/"+ToolAssetName(version, platform)] = zip
}

// Zip returns the zip currently served for a version and platform.
func (s *ToolSource) Zip(version, platform string) []byte {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.files["/dl/"+version+"/"+ToolAssetName(version, platform)]
}

func (s *ToolSource) serve(w http.ResponseWriter, r *http.Request) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.Hits[r.URL.Path]++
	if s.Down {
		http.Error(w, "down", http.StatusServiceUnavailable)
		return
	}
	if r.URL.Path == "/releases" {
		type asset struct {
			Name string `json:"name"`
			URL  string `json:"browser_download_url"`
			Size int    `json:"size"`
		}
		type rel struct {
			TagName    string  `json:"tag_name"`
			Draft      bool    `json:"draft"`
			Prerelease bool    `json:"prerelease"`
			Assets     []asset `json:"assets"`
		}
		var out []rel
		for _, x := range s.releases {
			rr := rel{TagName: x.Tag, Draft: x.Draft, Prerelease: x.Prerelease}
			for _, a := range x.Assets {
				rr.Assets = append(rr.Assets, asset{Name: a.Name, URL: s.Server.URL + a.Path, Size: len(s.files[a.Path])})
			}
			rr.Assets = append(rr.Assets, asset{Name: "source.tar.gz", URL: s.Server.URL + "/dl/other", Size: 1}) // ignored
			out = append(out, rr)
		}
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(out)
		return
	}
	if b, ok := s.files[r.URL.Path]; ok {
		w.Header().Set("Content-Type", "application/zip")
		w.Write(b)
		return
	}
	http.NotFound(w, r)
}
