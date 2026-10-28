package hub

import "testing"

func TestToolTagVersion(t *testing.T) {
	for tag, want := range map[string]string{
		"vFVX1.6.2": "1.6.2", "FVX1.6.0": "1.6.0", "vfvx1.5.4": "1.5.4", "v1.6.2": "1.6.2", "1.6.2": "1.6.2",
		" vFVX1.5.3 ": "1.5.3", "vFVX1.6": "", "vFVX": "", "nightly": "", "vFVX1.6.2-rc1": "", "vX1.6.2": "",
	} {
		if got := toolTagVersion(tag); got != want {
			t.Errorf("toolTagVersion(%q) = %q, want %q", tag, got, want)
		}
	}
}

func TestParseToolReleasesRealTags(t *testing.T) {
	const j = `[{"tag_name":"vFVX1.6.2","assets":[
	 {"name":"UPR_FVX-v1_6_2-Windows.zip","browser_download_url":"https://x/a","size":1},
	 {"name":"UPR_FVX-v1_6_2-Linux_ARM.zip","browser_download_url":"https://x/b","size":1}]},
	 {"tag_name":"vFVX1.6.1","assets":[{"name":"UPR_FVX-v1_6_1-Linux_x86.zip","browser_download_url":"https://x/c","size":1}]},
	 {"tag_name":"vFVX1.6.3","assets":[{"name":"UPR_FVX-v1_6_2-Linux_x86.zip","browser_download_url":"https://x/d","size":1}]}]`
	rels, err := parseToolReleases([]byte(j))
	if err != nil || len(rels) != 2 {
		t.Fatalf("got %v, %v", rels, err)
	}
	if rels[0].Version != "1.6.2" || rels[1].Version != "1.6.1" || len(rels[0].Assets) != 2 {
		t.Fatalf("unexpected: %+v", rels)
	}
}
