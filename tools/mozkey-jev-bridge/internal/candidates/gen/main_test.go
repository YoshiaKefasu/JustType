package main

import (
	"testing"
)

func TestParseIntField(t *testing.T) {
	cases := []struct {
		text    string
		want    int
		wantErr bool
	}{
		{"0", 0, false},
		{"4457", 4457, false},
		// A dictionary revision that ships negative costs must not lose its rows:
		// a skipped row changes which surfaces the index keeps.
		{"-120", -120, false},
		{"+8", 8, false},
		{"", 0, true},
		{"-", 0, true},
		{"+", 0, true},
		{"12a3", 0, true},
		{"1.5", 0, true},
		{" 12", 0, true},
	}

	for _, tc := range cases {
		got, err := parseIntField(tc.text)
		if tc.wantErr {
			if err == nil {
				t.Errorf("parseIntField(%q) = %d, want an error", tc.text, got)
			}
			continue
		}
		if err != nil {
			t.Errorf("parseIntField(%q): %v", tc.text, err)
			continue
		}
		if got != tc.want {
			t.Errorf("parseIntField(%q) = %d, want %d", tc.text, got, tc.want)
		}
	}
}

// The generator silently skips malformed rows, so parseIntField must report
// every failure rather than guessing.
func TestParseIntFieldNeverSilentlyAccepts(t *testing.T) {
	for _, text := range []string{"abc", "12 34", "١٢", "0x10"} {
		if _, err := parseIntField(text); err == nil {
			t.Errorf("parseIntField(%q) should have failed", text)
		}
	}
}

func TestExpandGlobsRejectsEmptyMatch(t *testing.T) {
	if _, err := expandGlobs([]string{"definitely-not-a-real-file-*.txt"}); err == nil {
		t.Fatal("expected an error for a glob that matched nothing")
	}
	if _, err := expandGlobs([]string{""}); err != nil {
		t.Errorf("an empty glob should be skipped, got %v", err)
	}
}
