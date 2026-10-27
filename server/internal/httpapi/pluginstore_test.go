package httpapi

import (
	"encoding/json"
	"strings"
	"testing"
)

// Plugin user store over REST (0.20 package 3, feature plugin_store_v1). Requests and responses are validated against
// the OpenAPI spec by s.do.

type storeEntryT struct {
	Key       string `json:"key"`
	Revision  int64  `json:"revision"`
	SizeBytes int64  `json:"size_bytes"`
	UpdatedAt string `json:"updated_at"`
}

type storeListT struct {
	Keys       []storeEntryT `json:"keys"`
	UsedBytes  int64         `json:"used_bytes"`
	QuotaBytes int64         `json:"quota_bytes"`
}

type storeValueT struct {
	Key      string          `json:"key"`
	Revision int64           `json:"revision"`
	Value    json.RawMessage `json:"value"`
}

type storeConflictT struct {
	Error struct {
		Code string `json:"code"`
	} `json:"error"`
	Revision *int64          `json:"revision"`
	Value    json.RawMessage `json:"value"`
}

const storeBase = "/api/v1/plugins/net.example.tracker/store"

func TestPluginStoreAPI(t *testing.T) {
	s := newSessEnv(t, nil)
	anna, carol := s.device(s.anna.ID, "Annas PC"), s.device(s.anna.ID, "Annas Laptop")
	bob := s.device(s.bob.ID, "Bobs PC")

	// Auth on every route.
	wantStatus(t, s.do("GET", storeBase, nil, opt{}), 401, "")
	wantStatus(t, s.do("GET", storeBase+"/k", nil, opt{}), 401, "")
	wantStatus(t, s.do("PUT", storeBase+"/k", map[string]any{"base_revision": 0, "value": 1}, opt{}), 401, "")
	wantStatus(t, s.do("DELETE", storeBase+"/k", nil, opt{}), 401, "")

	// Empty list with the cap; a missing key is 404.
	l := decode[storeListT](t, s.api(anna, "GET", storeBase, nil))
	if len(l.Keys) != 0 || l.UsedBytes != 0 || l.QuotaBytes != 16<<20 {
		t.Fatalf("%+v", l)
	}
	wantStatus(t, s.api(anna, "GET", storeBase+"/lists", nil), 404, "not_found")

	// Write, read from another device of the same person, replace.
	rec := s.api(anna, "PUT", storeBase+"/lists", map[string]any{"base_revision": 0, "value": map[string]any{"a": []int{1, 2}, "big": 12345678901234567}})
	wantStatus(t, rec, 200, "")
	e := decode[storeEntryT](t, rec)
	if e.Key != "lists" || e.Revision != 1 || e.UpdatedAt == "" || e.SizeBytes == 0 {
		t.Fatalf("%+v", e)
	}
	rec = s.api(carol, "GET", storeBase+"/lists", nil)
	wantStatus(t, rec, 200, "")
	v := decode[storeValueT](t, rec)
	if v.Revision != 1 || string(v.Value) != `{"a":[1,2],"big":12345678901234567}` { // numbers are not rounded
		t.Fatalf("%+v %s", v, v.Value)
	}
	rec = s.api(carol, "PUT", storeBase+"/lists", map[string]any{"base_revision": 1, "value": "text"})
	wantStatus(t, rec, 200, "")
	if e = decode[storeEntryT](t, rec); e.Revision != 2 {
		t.Fatalf("%+v", e)
	}

	// A stale base is 409 store_conflict with the server's revision and value; nothing changed.
	rec = s.api(anna, "PUT", storeBase+"/lists", map[string]any{"base_revision": 1, "value": "mine"})
	wantStatus(t, rec, 409, "store_conflict")
	c := decode[storeConflictT](t, rec)
	if c.Revision == nil || *c.Revision != 2 || string(c.Value) != `"text"` {
		t.Fatalf("%+v %s", c, c.Value)
	}
	// Base for a key that is not there: conflict with revision 0 and no value.
	rec = s.api(anna, "PUT", storeBase+"/nokey", map[string]any{"base_revision": 3, "value": 1})
	wantStatus(t, rec, 409, "store_conflict")
	if c = decode[storeConflictT](t, rec); c.Revision == nil || *c.Revision != 0 || len(c.Value) != 0 {
		t.Fatalf("%+v", c)
	}

	// The list shows keys without values.
	wantStatus(t, s.api(anna, "PUT", storeBase+"/second", map[string]any{"base_revision": 0, "value": nil}), 200, "")
	l = decode[storeListT](t, s.api(anna, "GET", storeBase, nil))
	if len(l.Keys) != 2 || l.Keys[0].Key != "lists" || l.Keys[0].Revision != 2 || l.Keys[1].Key != "second" || l.UsedBytes != l.Keys[0].SizeBytes+l.Keys[1].SizeBytes {
		t.Fatalf("%+v", l)
	}

	// Private per person and per plugin.
	if l = decode[storeListT](t, s.api(bob, "GET", storeBase, nil)); len(l.Keys) != 0 {
		t.Fatalf("%+v", l)
	}
	wantStatus(t, s.api(bob, "GET", storeBase+"/lists", nil), 404, "not_found")
	wantStatus(t, s.api(bob, "DELETE", storeBase+"/lists", nil), 404, "not_found")
	wantStatus(t, s.api(anna, "GET", "/api/v1/plugins/net.example.other/store/lists", nil), 404, "not_found")
	adminDev := s.device(s.admin.ID, "Admin PC")
	wantStatus(t, s.api(adminDev, "GET", storeBase+"/lists", nil), 404, "not_found")

	// Errors: bad key, bad body, a value above 256 KiB.
	wantStatus(t, s.do("GET", storeBase+"/Bad_Key", nil, opt{token: anna.tok, raw: true}), 400, "bad_request")
	wantStatus(t, s.do("PUT", storeBase+"/Bad_Key", map[string]any{"base_revision": 0, "value": 1}, opt{token: anna.tok, raw: true}), 400, "bad_request")
	wantStatus(t, s.do("PUT", storeBase+"/k", map[string]any{"base_revision": -1, "value": 1}, opt{token: anna.tok, raw: true}), 400, "bad_request")
	wantStatus(t, s.do("PUT", storeBase+"/k", nil, opt{token: anna.tok, data: []byte(`{"base_revision":0}`), ctype: "application/json", raw: true}), 400, "bad_request")
	wantStatus(t, s.do("PUT", storeBase+"/k", nil, opt{token: anna.tok, data: []byte(`not json`), ctype: "application/json", raw: true}), 400, "bad_request")
	big := map[string]any{"base_revision": 0, "value": strings.Repeat("x", 256<<10)}
	wantStatus(t, s.do("PUT", storeBase+"/big", big, opt{token: anna.tok, raw: true}), 413, "payload_too_large")
	wantStatus(t, s.api(anna, "GET", storeBase+"/big", nil), 404, "not_found")

	// Delete: stale base 409, right base 204, without base always; a second delete is 404.
	wantStatus(t, s.api(anna, "DELETE", storeBase+"/lists?base_revision=1", nil), 409, "store_conflict")
	wantStatus(t, s.api(anna, "DELETE", storeBase+"/lists?base_revision=2", nil), 204, "")
	wantStatus(t, s.api(anna, "DELETE", storeBase+"/second", nil), 204, "")
	wantStatus(t, s.api(anna, "DELETE", storeBase+"/second", nil), 404, "not_found")
	if l = decode[storeListT](t, s.api(anna, "GET", storeBase, nil)); len(l.Keys) != 0 || l.UsedBytes != 0 {
		t.Fatalf("%+v", l)
	}
}

func TestPluginStoreQuotaAPI(t *testing.T) {
	s := newSessEnv(t, nil)
	anna := s.device(s.anna.ID, "Annas PC")
	chunk := strings.Repeat("x", 250<<10)
	n := 0
	for ; n < 100; n++ {
		rec := s.do("PUT", storeBase+"/k"+string(rune('a'+n%26))+string(rune('a'+n/26)), map[string]any{"base_revision": 0, "value": chunk}, opt{token: anna.tok})
		if rec.Code == 413 {
			wantStatus(t, rec, 413, "store_quota_exceeded")
			break
		}
		wantStatus(t, rec, 200, "")
	}
	if n < 60 || n >= 100 {
		t.Fatalf("the cap came after %d values", n)
	}
	l := decode[storeListT](t, s.api(anna, "GET", storeBase, nil))
	if l.UsedBytes > l.QuotaBytes || len(l.Keys) != n {
		t.Fatalf("%d keys, %+v", n, l.UsedBytes)
	}
}

// The plugin file route and the store route share a prefix; a key named "file" must not be taken for a download.
func TestPluginStoreRouteNextToPluginFile(t *testing.T) {
	s := newSessEnv(t, nil)
	anna := s.device(s.anna.ID, "Annas PC")
	wantStatus(t, s.api(anna, "PUT", storeBase+"/file", map[string]any{"base_revision": 0, "value": 1}), 200, "")
	wantStatus(t, s.api(anna, "GET", storeBase+"/file", nil), 200, "")
	wantStatus(t, s.api(anna, "GET", "/api/v1/plugins/net.example.tracker/1.0.0/file", nil), 404, "not_found")
	wantStatus(t, s.do("GET", "/api/v1/plugins/net.example.tracker/1.0.0/other", nil, opt{token: anna.tok, raw: true}), 404, "not_found")
}
