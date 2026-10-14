package candidates

import (
	"compress/gzip"
	"os"
	"path/filepath"
	"testing"
)

// writeIndex writes a gzip TSV in the format internal/candidates/gen emits.
func writeIndex(t *testing.T, lines []string) string {
	t.Helper()

	path := filepath.Join(t.TempDir(), "candidates.tsv.gz")
	file, err := os.Create(path)
	if err != nil {
		t.Fatalf("create index: %v", err)
	}
	defer file.Close()

	writer := gzip.NewWriter(file)
	for _, line := range lines {
		if _, err := writer.Write([]byte(line + "\n")); err != nil {
			t.Fatalf("write index line: %v", err)
		}
	}
	if err := writer.Close(); err != nil {
		t.Fatalf("close gzip writer: %v", err)
	}
	return path
}

func TestLoadAndLookup(t *testing.T) {
	index, err := Load(writeIndex(t, []string{
		"てんき\t天気\t天鬼\t転記",
		"かんがえる\t考える",
	}))
	if err != nil {
		t.Fatalf("Load: %v", err)
	}

	if index.Len() != 2 {
		t.Errorf("Len = %d, want 2", index.Len())
	}

	got := index.Surfaces("てんき")
	want := []string{"天気", "天鬼", "転記"}
	if len(got) != len(want) {
		t.Fatalf("Surfaces = %v, want %v", got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Errorf("Surfaces[%d] = %q, want %q", i, got[i], want[i])
		}
	}
}

// A single-surface reading is the unambiguous case the bridge short-circuits,
// so it must survive loading as a one-element slice rather than collapsing to
// nil.
func TestLoadSingleSurfaceReading(t *testing.T) {
	index, err := Load(writeIndex(t, []string{"かんがえる\t考える"}))
	if err != nil {
		t.Fatalf("Load: %v", err)
	}

	got := index.Surfaces("かんがえる")
	if len(got) != 1 || got[0] != "考える" {
		t.Errorf("Surfaces = %v, want [考える]", got)
	}
}

// Absent and present-but-empty must stay distinguishable: the bridge treats
// them differently.
func TestSurfacesAbsentReading(t *testing.T) {
	index, err := Load(writeIndex(t, []string{"てんき\t天気"}))
	if err != nil {
		t.Fatalf("Load: %v", err)
	}

	if got := index.Surfaces("存在しない読み"); got != nil {
		t.Errorf("Surfaces = %v, want nil", got)
	}
	if got := index.Surfaces(""); got != nil {
		t.Errorf("Surfaces(\"\") = %v, want nil", got)
	}
}

func TestLoadEmptyIndex(t *testing.T) {
	index, err := Load(writeIndex(t, nil))
	if err != nil {
		t.Fatalf("Load: %v", err)
	}
	if index.Len() != 0 {
		t.Errorf("Len = %d, want 0", index.Len())
	}
}

func TestLoadRejectsMalformedLine(t *testing.T) {
	_, err := Load(writeIndex(t, []string{"てんき"}))
	if err == nil {
		t.Fatal("expected an error for a line with no surfaces")
	}
}

func TestLoadMissingFile(t *testing.T) {
	if _, err := Load(filepath.Join(t.TempDir(), "absent.tsv.gz")); err == nil {
		t.Fatal("expected an error for a missing index")
	}
}

func TestLoadNotGzip(t *testing.T) {
	path := filepath.Join(t.TempDir(), "plain.tsv.gz")
	if err := os.WriteFile(path, []byte("てんき\t天気\n"), 0o644); err != nil {
		t.Fatalf("write file: %v", err)
	}
	if _, err := Load(path); err == nil {
		t.Fatal("expected an error for a file that is not gzip")
	}
}

// A CRLF-terminated line must not leave \r glued to the last surface.
func TestLoadStripsCarriageReturn(t *testing.T) {
	index, err := Load(writeIndex(t, []string{"てんき\t天気\r"}))
	if err != nil {
		t.Fatalf("Load: %v", err)
	}
	got := index.Surfaces("てんき")
	if len(got) != 1 || got[0] != "天気" {
		t.Errorf("Surfaces = %q, want [天気]", got)
	}
}

func TestNilIndexSafe(t *testing.T) {
	var index *Index
	if index.Len() != 0 {
		t.Error("nil index Len must be 0")
	}
	if index.Surfaces("てんき") != nil {
		t.Error("nil index Surfaces must be nil")
	}
}
