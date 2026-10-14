// Package dataset appends one JSON object per ZNZ1 request so the bridge
// doubles as a collection pipeline for future Laya fine tuning.
//
// The file is opened in append mode and flushed after every record, so records
// survive a process crash.  It is not fsynced: a power loss can still drop the
// tail, which is acceptable for training data and keeps keystroke-rate writes
// cheap.
package dataset

import (
	"bufio"
	"encoding/json"
	"fmt"
	"os"
	"sync"
	"time"
)

// Record is one line of jev_dataset.jsonl.
//
// Options is always a JSON array, never null, so the empty case is a real
// value in the dataset rather than an absent field.
type Record struct {
	Timestamp    string   `json:"timestamp"`
	Generation   uint32   `json:"generation"`
	LeftContext  string   `json:"left_context"`
	Reading      string   `json:"reading"`
	RightContext string   `json:"right_context"`
	Options      []string `json:"options"`
	Choice       string   `json:"choice"`
	Confidence   float64  `json:"confidence"`
	LatencyMS    int64    `json:"latency_ms"`
}

// Logger is safe for concurrent use.
type Logger struct {
	mu      sync.Mutex
	file    *os.File
	writer  *bufio.Writer
	encoder *json.Encoder
}

// Open opens path for appending, creating it if needed.
func Open(path string) (*Logger, error) {
	file, err := os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o644)
	if err != nil {
		return nil, fmt.Errorf("dataset: open %s: %w", path, err)
	}

	writer := bufio.NewWriterSize(file, 1<<16)
	return &Logger{
		file:    file,
		writer:  writer,
		encoder: json.NewEncoder(writer),
	}, nil
}

// Append writes one record and flushes it to the OS.
func (l *Logger) Append(record Record) error {
	if record.Options == nil {
		record.Options = []string{}
	}
	if record.Timestamp == "" {
		record.Timestamp = Timestamp()
	}

	l.mu.Lock()
	defer l.mu.Unlock()

	if err := l.encoder.Encode(record); err != nil {
		return fmt.Errorf("dataset: encode record: %w", err)
	}
	if err := l.writer.Flush(); err != nil {
		return fmt.Errorf("dataset: flush: %w", err)
	}
	return nil
}

// Close flushes and closes the file.  It is safe to call twice.
func (l *Logger) Close() error {
	l.mu.Lock()
	defer l.mu.Unlock()

	var firstErr error
	if err := l.writer.Flush(); err != nil {
		firstErr = err
	}
	if err := l.file.Close(); err != nil && firstErr == nil {
		firstErr = err
	}
	if firstErr != nil {
		return fmt.Errorf("dataset: close: %w", firstErr)
	}
	return nil
}

// Timestamp renders the record timestamp: UTC, RFC3339 with milliseconds.
func Timestamp() string {
	return time.Now().UTC().Format("2006-01-02T15:04:05.000Z")
}
