package httpapi

import (
	"bytes"
	"context"
	"errors"
	"io"
	"log/slog"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
)

// failWriter simulates a client that closes the connection in the middle of a download.
type failWriter struct {
	*httptest.ResponseRecorder
	after int
}

func (w *failWriter) Write(p []byte) (int, error) {
	if w.Body.Len()+len(p) > w.after {
		return 0, errors.New("write tcp: broken pipe")
	}
	return w.ResponseRecorder.Write(p)
}

func newStreamServer(t *testing.T) *Server {
	t.Helper()
	te := newToolEnv(t)
	return &Server{svc: te.svc, log: slog.New(slog.NewTextHandler(io.Discard, nil))}
}

func tmpZip(t *testing.T, n int) *os.File {
	t.Helper()
	p := filepath.Join(t.TempDir(), "x.zip")
	if err := os.WriteFile(p, bytes.Repeat([]byte{'z'}, n), 0o600); err != nil {
		t.Fatal(err)
	}
	f, err := os.Open(p)
	if err != nil {
		t.Fatal(err)
	}
	return f
}

// A client that goes away during the download (broken pipe) is not a server error: the stream ends quietly.
func TestToolPackageClientDisconnectIsNotAnError(t *testing.T) {
	s := newStreamServer(t)
	w := &failWriter{ResponseRecorder: httptest.NewRecorder(), after: 10}
	ts := &toolPackageStream{f: tmpZip(t, 1<<20), etag: `"x"`, size: 1 << 20, srv: s, ctx: context.Background(), path: "randomizer/1/linux-x64"}
	if err := ts.VisitGetToolPackageResponse(w); err != nil {
		t.Fatalf("a broken pipe must not be returned (it would be answered as 500 and recorded): %v", err)
	}
	if w.Code != 200 {
		t.Fatalf("status %d", w.Code)
	}
}

// The zip on disk differs in size from the catalog row (replaced while the row was read): the real size is
// announced, so net/http does not abort with "wrote more than the declared Content-Length".
func TestToolPackageSizeMismatch(t *testing.T) {
	s := newStreamServer(t)
	for _, catalog := range []int64{10, 5000} {
		w := httptest.NewRecorder()
		ts := &toolPackageStream{f: tmpZip(t, 1000), etag: `"x"`, size: catalog, srv: s, ctx: context.Background(), path: "p"}
		if err := ts.VisitGetToolPackageResponse(w); err != nil {
			t.Fatalf("catalog %d: %v", catalog, err)
		}
		if w.Body.Len() != 1000 || w.Header().Get("Content-Length") != "1000" {
			t.Fatalf("catalog %d: body %d, length %s", catalog, w.Body.Len(), w.Header().Get("Content-Length"))
		}
	}
}

// A failing disk read is a real internal error: logged with the cause, no second response body.
func TestToolPackageReadErrorWritesNoSecondResponse(t *testing.T) {
	var logbuf bytes.Buffer
	s := newStreamServer(t)
	s.log = slog.New(slog.NewTextHandler(&logbuf, nil))
	f := tmpZip(t, 10)
	f.Close() // reading a closed file fails
	w := httptest.NewRecorder()
	ts := &toolPackageStream{f: f, etag: `"x"`, size: 10, srv: s, ctx: context.Background(), path: "randomizer/1/linux-x64"}
	if err := ts.VisitGetToolPackageResponse(w); err != nil {
		t.Fatalf("%v", err)
	}
	if !bytes.Contains(logbuf.Bytes(), []byte("tool package read failed")) || bytes.Contains(w.Body.Bytes(), []byte("Internal error")) {
		t.Fatalf("log %q body %q", logbuf.String(), w.Body.String())
	}
}

// The catalog says cached, but the file is gone or was replaced: 404 / a complete answer, never 500.
func TestToolPackageFileMissingOrChangedOnDisk(t *testing.T) {
	te := newToolEnv(t)
	wantStatus(t, te.api(te.adm, "POST", "/api/v1/admin/tools/randomizer/sync", nil), 200, "")
	wantStatus(t, te.api(te.adm, "POST", "/api/v1/admin/tools/randomizer/versions/1.6.2/install", nil), 200, "")
	wantStatus(t, te.api(te.adm, "PUT", "/api/v1/admin/tools/randomizer", map[string]any{"offered": true}), 200, "")
	pkg := "/api/v1/tools/randomizer/packages/1.6.2/linux-x64"
	file := filepath.Join(te.svc.DataDir(), "tools", "randomizer", "1.6.2", "linux-x64.zip")

	if err := os.WriteFile(file, []byte("short"), 0o600); err != nil {
		t.Fatal(err)
	}
	wantStatus(t, te.api(te.annaD, "GET", pkg, nil), 404, "not_found") // size no longer matches the catalog: not cached
	if err := os.Remove(file); err != nil {
		t.Fatal(err)
	}
	wantStatus(t, te.api(te.annaD, "GET", pkg, nil), 404, "not_found")
}
