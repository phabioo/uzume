package httpapi

import (
	"net/http"
	"strings"

	"github.com/phabioo/uzume/server/internal/rollwrite"
)

// isStreamRoute reports whether a GET request serves file content that is streamed to the client (core files,
// save and save history content; the ROM and firmware routes call rollingWrite themselves).
func isStreamRoute(r *http.Request) bool {
	if r.Method != http.MethodGet {
		return false
	}
	p := r.URL.Path
	return (strings.HasPrefix(p, "/api/v1/tools/") && strings.Contains(p, "/packages/")) ||
		(strings.HasPrefix(p, "/api/v1/cores/") && strings.Contains(p, "/files/")) ||
		(strings.HasPrefix(p, "/api/v1/games/") && strings.HasSuffix(p, "/content"))
}

// rollingWrite renews the write deadline of a streamed download with every chunk (see package rollwrite).
func rollingWrite(w http.ResponseWriter) (http.ResponseWriter, func()) { return rollwrite.Wrap(w) }
