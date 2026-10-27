package hub_test

import (
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"testing"

	"github.com/phabioo/uzume/server/internal/hub"
)

// Plugin user store (0.20 package 3, feature plugin_store_v1).

const storePlugin = "net.example.tracker"

func TestPluginStoreRevisions(t *testing.T) {
	svc, _, admin := newAdmin(t)
	anna, err := svc.CreateUser(ctx, "anna", "Anna")
	if err != nil {
		t.Fatal(err)
	}
	// A missing key is not found; the list starts empty with the cap.
	if _, err := svc.GetStore(ctx, anna.ID, storePlugin, "lists"); !errors.Is(err, hub.ErrNotFound) {
		t.Fatalf("%v", err)
	}
	l, err := svc.ListStore(ctx, anna.ID, storePlugin)
	if err != nil || len(l.Entries) != 0 || l.Entries == nil || l.UsedBytes != 0 || l.QuotaBytes != hub.MaxStorePluginBytes {
		t.Fatalf("%+v %v", l, err)
	}

	// Revision 1 needs base 0; later writes count up; the value is stored compactly.
	e, err := svc.PutStore(ctx, anna.ID, storePlugin, "lists", json.RawMessage(`{ "a": [1, 2] }`), 0)
	if err != nil || e.Revision != 1 || e.SizeBytes != int64(len(`{"a":[1,2]}`)) {
		t.Fatalf("%+v %v", e, err)
	}
	if e, err = svc.PutStore(ctx, anna.ID, storePlugin, "lists", json.RawMessage(`[1]`), 1); err != nil || e.Revision != 2 {
		t.Fatalf("%+v %v", e, err)
	}
	got, err := svc.GetStore(ctx, anna.ID, storePlugin, "lists")
	if err != nil || got.Revision != 2 || string(got.Value) != `[1]` {
		t.Fatalf("%+v %v", got, err)
	}

	// A stale or wrong base is a conflict that carries the server's value and revision, and changes nothing.
	for _, base := range []int64{0, 1, 3} {
		_, err = svc.PutStore(ctx, anna.ID, storePlugin, "lists", json.RawMessage(`[2]`), base)
		var ce *hub.StoreConflictError
		if !errors.As(err, &ce) || !ce.Current.Exists || ce.Current.Entry.Revision != 2 || string(ce.Current.Entry.Value) != `[1]` {
			t.Fatalf("base %d: %v", base, err)
		}
	}
	// A base for a key that does not exist is a conflict with Exists false.
	_, err = svc.PutStore(ctx, anna.ID, storePlugin, "other", json.RawMessage(`1`), 4)
	var ce *hub.StoreConflictError
	if !errors.As(err, &ce) || ce.Current.Exists {
		t.Fatalf("%v", err)
	}

	// Scalars and null are JSON values too.
	for i, v := range []string{`"text"`, `42`, `true`, `null`} {
		if _, err := svc.PutStore(ctx, anna.ID, storePlugin, fmt.Sprintf("k%d", i), json.RawMessage(v), 0); err != nil {
			t.Fatalf("%s: %v", v, err)
		}
	}

	// Per user and per plugin: nobody else sees the key, not even an admin.
	for _, who := range []string{admin.ID} {
		if _, err := svc.GetStore(ctx, who, storePlugin, "lists"); !errors.Is(err, hub.ErrNotFound) {
			t.Fatalf("%v", err)
		}
	}
	if _, err := svc.GetStore(ctx, anna.ID, "net.example.other", "lists"); !errors.Is(err, hub.ErrNotFound) {
		t.Fatalf("%v", err)
	}
	l, _ = svc.ListStore(ctx, anna.ID, storePlugin)
	if len(l.Entries) != 5 || l.Entries[0].Key != "k0" || l.Entries[0].Value != nil {
		t.Fatalf("%+v", l.Entries)
	}

	// Delete: conditional with a base, unconditional with -1; a stale base is a conflict.
	if err := svc.DeleteStore(ctx, anna.ID, storePlugin, "lists", 1); !errors.As(err, &ce) || !ce.Current.Exists || ce.Current.Entry.Revision != 2 {
		t.Fatalf("%v", err)
	}
	if err := svc.DeleteStore(ctx, anna.ID, storePlugin, "lists", 2); err != nil {
		t.Fatal(err)
	}
	if err := svc.DeleteStore(ctx, anna.ID, storePlugin, "k0", -1); err != nil {
		t.Fatal(err)
	}
	if err := svc.DeleteStore(ctx, anna.ID, storePlugin, "k0", -1); !errors.Is(err, hub.ErrNotFound) {
		t.Fatalf("%v", err)
	}
	// A recreated key starts again at revision 1.
	if e, err = svc.PutStore(ctx, anna.ID, storePlugin, "lists", json.RawMessage(`0`), 0); err != nil || e.Revision != 1 {
		t.Fatalf("%+v %v", e, err)
	}
}

func TestPluginStoreLimitsAndValidation(t *testing.T) {
	svc, _, _ := newAdmin(t)
	u, _ := svc.CreateUser(ctx, "anna", "Anna")
	one := json.RawMessage(`1`)

	for _, key := range []string{"", "UPPER", "has space", "slash/x", strings.Repeat("a", 65), "ümlaut"} {
		if _, err := svc.PutStore(ctx, u.ID, storePlugin, key, one, 0); err == nil {
			t.Errorf("key %q accepted", key)
		}
	}
	for _, key := range []string{"a", "a.b-c_d", strings.Repeat("a", 64)} {
		if _, err := svc.PutStore(ctx, u.ID, storePlugin, key, one, 0); err != nil {
			t.Errorf("key %q: %v", key, err)
		}
	}
	for _, p := range []string{"", "Bad Plugin", "../x", strings.Repeat("a", 129)} {
		if _, err := svc.PutStore(ctx, u.ID, p, "k", one, 0); err == nil {
			t.Errorf("plugin %q accepted", p)
		}
	}
	for _, v := range []string{"", "not json", `{"a":`, `{} {}`} {
		if _, err := svc.PutStore(ctx, u.ID, storePlugin, "bad", json.RawMessage(v), 0); err == nil {
			t.Errorf("value %q accepted", v)
		}
	}
	if _, err := svc.PutStore(ctx, u.ID, storePlugin, "neg", one, -1); err == nil {
		t.Error("negative base accepted")
	}

	// 256 KiB is the limit of one value.
	big := func(n int) json.RawMessage { return json.RawMessage(`"` + strings.Repeat("x", n-2) + `"`) }
	if _, err := svc.PutStore(ctx, u.ID, storePlugin, "big", big(hub.MaxStoreValueBytes), 0); err != nil {
		t.Fatal(err)
	}
	_, err := svc.PutStore(ctx, u.ID, storePlugin, "bigger", big(hub.MaxStoreValueBytes+1), 0)
	var he *hub.Error
	if !errors.As(err, &he) || he.Code != hub.CodePayloadTooLarge {
		t.Fatalf("%v", err)
	}

	// 16 MiB per plugin: replacing a value counts the old one as freed, a new key beyond the cap is refused, and another
	// plugin has its own budget.
	for i := 0; i < 63; i++ { // 64 values of 256 KiB = 16 MiB with "big"
		if _, err := svc.PutStore(ctx, u.ID, "net.example.fill", fmt.Sprintf("f%d", i), big(hub.MaxStoreValueBytes), 0); err != nil {
			t.Fatalf("fill %d: %v", i, err)
		}
	}
	if _, err := svc.PutStore(ctx, u.ID, "net.example.fill", "f63", big(hub.MaxStoreValueBytes), 0); err != nil {
		t.Fatal(err)
	}
	if _, err := svc.PutStore(ctx, u.ID, "net.example.fill", "more", one, 0); !errors.Is(err, hub.ErrStoreQuotaExceeded) {
		t.Fatalf("over the cap: %v", err)
	}
	if _, err := svc.PutStore(ctx, u.ID, "net.example.fill", "f0", one, 1); err != nil {
		t.Fatalf("a smaller replacement must fit: %v", err)
	}
	if _, err := svc.PutStore(ctx, u.ID, "net.example.fill", "more", one, 0); err != nil {
		t.Fatalf("freed space is usable: %v", err)
	}
	if _, err := svc.PutStore(ctx, u.ID, "net.example.second", "k", big(hub.MaxStoreValueBytes), 0); err != nil {
		t.Fatalf("another plugin has its own budget: %v", err)
	}
	// 1024 keys per plugin.
	for i := 0; i < hub.MaxStorePluginKeys; i++ {
		if _, err := svc.PutStore(ctx, u.ID, "net.example.keys", fmt.Sprintf("k%d", i), one, 0); err != nil {
			t.Fatalf("key %d: %v", i, err)
		}
	}
	if _, err := svc.PutStore(ctx, u.ID, "net.example.keys", "one-too-many", one, 0); !errors.Is(err, hub.ErrStoreQuotaExceeded) {
		t.Fatalf("%v", err)
	}
	if _, err := svc.PutStore(ctx, u.ID, "net.example.keys", "k0", json.RawMessage(`2`), 1); err != nil {
		t.Fatalf("replacing at the key cap: %v", err)
	}
}

func TestPluginStoreGoesWithTheUser(t *testing.T) {
	svc, _, admin := newAdmin(t)
	u, _ := svc.CreateUser(ctx, "anna", "Anna")
	if _, err := svc.PutStore(ctx, u.ID, storePlugin, "lists", json.RawMessage(`1`), 0); err != nil {
		t.Fatal(err)
	}
	if err := svc.DeleteUser(ctx, u.ID, admin.ID); err != nil {
		t.Fatal(err)
	}
	if l, err := svc.ListStore(ctx, u.ID, storePlugin); err != nil || len(l.Entries) != 0 {
		t.Fatalf("%+v %v", l, err)
	}
}
