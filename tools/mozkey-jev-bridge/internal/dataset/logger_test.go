package dataset

import (
	"bufio"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

func TestAppendWritesOneJsonObjectPerLine(t *testing.T) {
	path := filepath.Join(t.TempDir(), "jev_dataset.jsonl")
	logger, err := Open(path)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}

	records := []Record{
		{Generation: 1, Reading: "てんき", Options: []string{"天気", "転記"}, Choice: "天気", Confidence: 0.91, LatencyMS: 420},
		{Generation: 2, Reading: "なにか", Options: nil, Choice: "", Confidence: 0, LatencyMS: 3},
	}
	for _, record := range records {
		if err := logger.Append(record); err != nil {
			t.Fatalf("Append: %v", err)
		}
	}
	if err := logger.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}

	file, err := os.Open(path)
	if err != nil {
		t.Fatalf("open log: %v", err)
	}
	defer file.Close()

	var decoded []Record
	scanner := bufio.NewScanner(file)
	for scanner.Scan() {
		if scanner.Text() == "" {
			continue
		}
		var record Record
		if err := json.Unmarshal(scanner.Bytes(), &record); err != nil {
			t.Fatalf("line is not valid JSON: %v (%q)", err, scanner.Text())
		}
		decoded = append(decoded, record)
	}
	if err := scanner.Err(); err != nil {
		t.Fatalf("scan: %v", err)
	}

	if len(decoded) != len(records) {
		t.Fatalf("read %d records, want %d", len(decoded), len(records))
	}
	if decoded[0].Choice != "天気" || decoded[0].Confidence != 0.91 {
		t.Errorf("record 0 = %+v", decoded[0])
	}
	if decoded[0].Timestamp == "" {
		t.Error("timestamp must be filled in when absent")
	}
}

// An empty option set has to survive as a real JSON array, otherwise the
// dataset records a missing field instead of "the bridge had no candidates".
func TestEmptyOptionsSerialiseAsArray(t *testing.T) {
	path := filepath.Join(t.TempDir(), "jev_dataset.jsonl")
	logger, err := Open(path)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	if err := logger.Append(Record{Generation: 3, Options: nil}); err != nil {
		t.Fatalf("Append: %v", err)
	}
	if err := logger.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}

	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read log: %v", err)
	}

	var fields map[string]json.RawMessage
	if err := json.Unmarshal(raw, &fields); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	if got := string(fields["options"]); got != "[]" {
		t.Errorf(`options = %s, want []`, got)
	}
}

// The field names are the dataset schema; renaming one silently breaks any
// downstream consumer.
func TestFieldNames(t *testing.T) {
	path := filepath.Join(t.TempDir(), "jev_dataset.jsonl")
	logger, err := Open(path)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	if err := logger.Append(Record{}); err != nil {
		t.Fatalf("Append: %v", err)
	}
	if err := logger.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}

	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read log: %v", err)
	}

	var fields map[string]json.RawMessage
	if err := json.Unmarshal(raw, &fields); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}

	want := []string{
		"timestamp", "generation", "left_context", "reading",
		"right_context", "options", "choice", "confidence", "latency_ms",
	}
	for _, name := range want {
		if _, present := fields[name]; !present {
			t.Errorf("field %q is missing from the record", name)
		}
	}
	if len(fields) != len(want) {
		t.Errorf("record has %d fields, want %d", len(fields), len(want))
	}
}

// A crash between Append calls must not lose earlier records, which is why each
// record is flushed rather than buffered until Close.
func TestRecordsSurviveWithoutClose(t *testing.T) {
	path := filepath.Join(t.TempDir(), "jev_dataset.jsonl")
	logger, err := Open(path)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	t.Cleanup(func() { logger.Close() })

	if err := logger.Append(Record{Generation: 7, Choice: "天気"}); err != nil {
		t.Fatalf("Append: %v", err)
	}

	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read log: %v", err)
	}
	if len(raw) == 0 {
		t.Fatal("record was not flushed before Close")
	}
}

func TestAppendCreatesFileWhenMissing(t *testing.T) {
	path := filepath.Join(t.TempDir(), "nested", "jev_dataset.jsonl")

	// Open does not create parent directories; the caller is expected to point
	// at an existing directory, so a missing parent is a real error.
	if _, err := Open(path); err == nil {
		t.Fatal("expected an error when the parent directory is missing")
	}
}

func TestOpenMissingParentDirectory(t *testing.T) {
	path := filepath.Join(t.TempDir(), "jev_dataset.jsonl")
	logger, err := Open(path)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	if err := logger.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}
}

func TestTimestampFormat(t *testing.T) {
	stamp := Timestamp()
	if len(stamp) != len("2006-01-02T15:04:05.000Z") {
		t.Errorf("Timestamp = %q, want millisecond RFC3339 UTC", stamp)
	}
}
