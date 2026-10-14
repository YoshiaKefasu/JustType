package bridge

import (
	"bufio"
	"compress/gzip"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"mozkey-jev-bridge/internal/candidates"
	"mozkey-jev-bridge/internal/dataset"
	"mozkey-jev-bridge/internal/jev"
	"mozkey-jev-bridge/internal/znz"
)

// fakeScorer records what it was asked and replays a scripted answer.
type fakeScorer struct {
	question jev.Question
	state    string
	calls    int

	answer jev.Answer
	err    error
	delay  time.Duration
}

func (f *fakeScorer) Ask(ctx context.Context, state string, question jev.Question) (jev.Answer, error) {
	f.calls++
	f.state = state
	f.question = question

	if f.delay > 0 {
		select {
		case <-time.After(f.delay):
		case <-ctx.Done():
			return jev.Answer{}, ctx.Err()
		}
	}
	if f.err != nil {
		return jev.Answer{}, f.err
	}
	return f.answer, nil
}

func writeTestIndex(t *testing.T, lines []string) string {
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
			t.Fatalf("write index: %v", err)
		}
	}
	if err := writer.Close(); err != nil {
		t.Fatalf("close gzip writer: %v", err)
	}
	return path
}

func newTestHandler(t *testing.T, scorer Scorer, indexLines []string) (*Handler, string) {
	t.Helper()

	index, err := candidates.Load(writeTestIndex(t, indexLines))
	if err != nil {
		t.Fatalf("load index: %v", err)
	}

	logPath := filepath.Join(t.TempDir(), "jev_dataset.jsonl")
	logger, err := dataset.Open(logPath)
	if err != nil {
		t.Fatalf("open dataset: %v", err)
	}
	t.Cleanup(func() { logger.Close() })

	return &Handler{
		Scorer:       scorer,
		Index:        index,
		Log:          logger,
		MaxOptions:   6,
		Budget:       5 * time.Second,
		Instructions: DefaultInstructions,
	}, logPath
}

func zenzPrompt(left, right, reading string) []byte {
	prompt := "\uee02" + left
	if right != "" {
		prompt += "\uee07" + right
	}
	prompt += "\uee00" + reading + "\uee01"
	return []byte(prompt)
}

func readRecords(t *testing.T, path string) []dataset.Record {
	t.Helper()

	file, err := os.Open(path)
	if err != nil {
		t.Fatalf("open dataset: %v", err)
	}
	defer file.Close()

	var records []dataset.Record
	scanner := bufio.NewScanner(file)
	for scanner.Scan() {
		if scanner.Text() == "" {
			continue
		}
		var record dataset.Record
		if err := json.Unmarshal(scanner.Bytes(), &record); err != nil {
			t.Fatalf("bad JSONL line: %v", err)
		}
		records = append(records, record)
	}
	if err := scanner.Err(); err != nil {
		t.Fatalf("scan dataset: %v", err)
	}
	return records
}

func TestHandleAsksJevAndReturnsChoice(t *testing.T) {
	scorer := &fakeScorer{answer: jev.Answer{Choice: "天気", Confidence: 0.81}}
	handler, logPath := newTestHandler(t, scorer, []string{
		"てんき\t天気\t天鬼\t転記\t転帰\tてんき\tテンキ",
	})

	result := handler.Handle(
		znz.RequestHeader{Generation: 42, TimeoutMsec: 1000, MaxOutputChars: 256},
		zenzPrompt("きょうは いい", "ですね", "てんき"),
	)

	if result.Status != znz.StatusOK {
		t.Errorf("status = %d, want OK", result.Status)
	}
	if result.Value != "天気" {
		t.Errorf("value = %q, want 天気", result.Value)
	}
	if result.Debug != DebugOK {
		t.Errorf("debug = %q, want %q", result.Debug, DebugOK)
	}
	if !result.Asked {
		t.Error("Jev should have been consulted")
	}

	if scorer.calls != 1 {
		t.Fatalf("scorer calls = %d, want 1", scorer.calls)
	}
	wantState := "左文脈: きょうは いい\n読み: てんき\n右文脈: ですね"
	if scorer.state != wantState {
		t.Errorf("state = %q, want %q", scorer.state, wantState)
	}
	if len(scorer.question.Options) != 6 {
		t.Errorf("options = %v, want the 6 dictionary surfaces", scorer.question.Options)
	}
	if scorer.question.Options[0] != "天気" {
		t.Errorf("first option = %q, want 天気", scorer.question.Options[0])
	}
	if scorer.question.Instructions != DefaultInstructions {
		t.Errorf("instructions = %q", scorer.question.Instructions)
	}

	records := readRecords(t, logPath)
	if len(records) != 1 {
		t.Fatalf("logged %d records, want 1", len(records))
	}
	record := records[0]
	if record.Generation != 42 {
		t.Errorf("generation = %d, want 42", record.Generation)
	}
	if record.Reading != "てんき" {
		t.Errorf("reading = %q, want てんき", record.Reading)
	}
	if record.LeftContext != "きょうは いい" || record.RightContext != "ですね" {
		t.Errorf("contexts = %q / %q", record.LeftContext, record.RightContext)
	}
	if record.Choice != "天気" || record.Confidence != 0.81 {
		t.Errorf("choice = %q confidence = %v", record.Choice, record.Confidence)
	}
	if len(record.Options) != 6 {
		t.Errorf("logged options = %v, want 6", record.Options)
	}
}

// An unambiguous reading needs no model call: the dictionary answer is what
// Mozc would show anyway, so the round trip is pure latency and cost.
func TestHandleSingleCandidateShortCircuits(t *testing.T) {
	scorer := &fakeScorer{}
	handler, logPath := newTestHandler(t, scorer, []string{"かんがえる\t考える"})

	result := handler.Handle(
		znz.RequestHeader{MaxOutputChars: 256},
		zenzPrompt("よく", "", "かんがえる"),
	)

	if scorer.calls != 0 {
		t.Fatalf("scorer calls = %d, want 0", scorer.calls)
	}
	if result.Value != "考える" {
		t.Errorf("value = %q, want 考える", result.Value)
	}
	if result.Status != znz.StatusOK || result.Debug != DebugSingleCandidate {
		t.Errorf("status = %d debug = %q", result.Status, result.Debug)
	}

	records := readRecords(t, logPath)
	if len(records) != 1 || len(records[0].Options) != 1 {
		t.Fatalf("dataset = %+v, want one record with one option", records)
	}
}

// A reading absent from the dictionary has no candidate set, so the honest
// answer is the kana the user typed.  The prompt carries katakana because the
// prompt builder converted it; echoing that would display テンキ for てんき.
func TestHandleNoCandidatesReturnsHiragana(t *testing.T) {
	scorer := &fakeScorer{}
	handler, logPath := newTestHandler(t, scorer, []string{"てんき\t天気"})

	result := handler.Handle(
		znz.RequestHeader{MaxOutputChars: 256},
		zenzPrompt("なにか", "", "ニホンゴ"),
	)

	if scorer.calls != 0 {
		t.Fatalf("scorer calls = %d, want 0", scorer.calls)
	}
	if result.Value != "にほんご" {
		t.Errorf("value = %q, want にほんご", result.Value)
	}
	if result.Status != znz.StatusOK || result.Debug != DebugNoCandidates {
		t.Errorf("status = %d debug = %q", result.Status, result.Debug)
	}

	records := readRecords(t, logPath)
	if len(records) != 1 {
		t.Fatalf("logged %d records, want 1", len(records))
	}
	if len(records[0].Options) != 0 {
		t.Errorf("options = %v, want empty", records[0].Options)
	}
}

func TestHandlePromptWithoutReading(t *testing.T) {
	scorer := &fakeScorer{}
	handler, _ := newTestHandler(t, scorer, []string{"てんき\t天気\t転記"})

	result := handler.Handle(
		znz.RequestHeader{MaxOutputChars: 256},
		[]byte("\uee02左文脈のみ\uee01"),
	)

	if result.Status != znz.StatusError {
		t.Errorf("status = %d, want Error", result.Status)
	}
	if result.Debug != DebugNoReading {
		t.Errorf("debug = %q, want %q", result.Debug, DebugNoReading)
	}
	if result.Value != "" {
		t.Errorf("value = %q, want empty", result.Value)
	}
	if scorer.calls != 0 {
		t.Errorf("scorer calls = %d, want 0", scorer.calls)
	}
}

// A failing backend must return an empty value so Mozkey falls back to its own
// conversion instead of showing a guessed surface.
func TestHandleJevError(t *testing.T) {
	scorer := &fakeScorer{err: errors.New("jev: http 500: boom")}
	handler, logPath := newTestHandler(t, scorer, []string{"てんき\t天気\t転記"})

	result := handler.Handle(
		znz.RequestHeader{MaxOutputChars: 256},
		zenzPrompt("", "", "てんき"),
	)

	if result.Status != znz.StatusError {
		t.Errorf("status = %d, want Error", result.Status)
	}
	if result.Debug != DebugError {
		t.Errorf("debug = %q, want %q", result.Debug, DebugError)
	}
	if result.Value != "" {
		t.Errorf("value = %q, want empty", result.Value)
	}

	records := readRecords(t, logPath)
	if len(records) != 1 {
		t.Fatalf("logged %d records, want 1", len(records))
	}
	if records[0].Choice != "" {
		t.Errorf("logged choice = %q, want empty on failure", records[0].Choice)
	}
	if len(records[0].Options) != 2 {
		t.Errorf("logged options = %v, want the 2 offered", records[0].Options)
	}
}

func TestHandleJevTimeout(t *testing.T) {
	scorer := &fakeScorer{delay: 2 * time.Second, err: context.DeadlineExceeded}
	handler, _ := newTestHandler(t, scorer, []string{"てんき\t天気\t転記"})
	handler.Budget = 30 * time.Millisecond

	result := handler.Handle(
		znz.RequestHeader{MaxOutputChars: 256, TimeoutMsec: 1000},
		zenzPrompt("", "", "てんき"),
	)

	if result.Status != znz.StatusTimeout {
		t.Errorf("status = %d, want Timeout", result.Status)
	}
	if result.Debug != DebugTimeout {
		t.Errorf("debug = %q, want %q", result.Debug, DebugTimeout)
	}
}

// The request's own timeout must win when it is tighter than the configured
// ceiling.
func TestHandleRequestTimeoutClampsBudget(t *testing.T) {
	scorer := &fakeScorer{delay: 5 * time.Second}
	handler, _ := newTestHandler(t, scorer, []string{"てんき\t天気\t転記"})
	handler.Budget = time.Hour

	result := handler.Handle(
		// 50 ms is the protocol floor, so the budget lands just under it.
		znz.RequestHeader{MaxOutputChars: 256, TimeoutMsec: 50},
		zenzPrompt("", "", "てんき"),
	)

	if result.Status != znz.StatusTimeout {
		t.Errorf("status = %d, want Timeout", result.Status)
	}
}

func TestHandleMaxOptionsCapsCandidateList(t *testing.T) {
	scorer := &fakeScorer{answer: jev.Answer{Choice: "きょう", Confidence: 0.4}}
	handler, _ := newTestHandler(t, scorer, []string{
		"きょう\t今日\t共\t強\t鏡\t橋\t協\t教\t興\t京",
	})
	handler.MaxOptions = 3

	result := handler.Handle(
		znz.RequestHeader{MaxOutputChars: 256},
		zenzPrompt("", "", "きょう"),
	)

	if len(scorer.question.Options) != 3 {
		t.Errorf("options = %v, want 3", scorer.question.Options)
	}
	if result.Value != "きょう" {
		t.Errorf("value = %q", result.Value)
	}
}

// max_output_chars counts characters, so a Japanese result must not be cut
// mid-rune.
func TestHandleRespectsMaxOutputChars(t *testing.T) {
	scorer := &fakeScorer{answer: jev.Answer{Choice: "日本語変換結果", Confidence: 0.7}}
	handler, _ := newTestHandler(t, scorer, []string{"にほんご\t日本語\tニホンゴ"})

	result := handler.Handle(
		znz.RequestHeader{MaxOutputChars: 3},
		zenzPrompt("", "", "にほんご"),
	)

	if result.Value != "日本語" {
		t.Errorf("value = %q, want 日本語", result.Value)
	}
}

// Dry run answers from the dictionary ranking so the pipe wiring can be
// verified without spending API calls.
func TestHandleDryRun(t *testing.T) {
	scorer := &fakeScorer{}
	handler, logPath := newTestHandler(t, scorer, []string{"てんき\t天気\t転記\tてんき"})
	handler.DryRun = true

	result := handler.Handle(
		znz.RequestHeader{MaxOutputChars: 256},
		zenzPrompt("きょうは", "", "てんき"),
	)

	if scorer.calls != 0 {
		t.Fatalf("scorer calls = %d, want 0 in dry run", scorer.calls)
	}
	if result.Value != "天気" {
		t.Errorf("value = %q, want 天気", result.Value)
	}
	if result.Debug != DebugDryRun {
		t.Errorf("debug = %q, want %q", result.Debug, DebugDryRun)
	}
	if !result.Asked == false {
		t.Error("Asked must be false in dry run")
	}

	records := readRecords(t, logPath)
	if len(records) != 1 {
		t.Fatalf("logged %d records, want 1", len(records))
	}
}

func TestHandleTrimsSurroundingMarkersFromContext(t *testing.T) {
	scorer := &fakeScorer{answer: jev.Answer{Choice: "天気", Confidence: 0.5}}
	handler, _ := newTestHandler(t, scorer, []string{"てんき\t天気\t転記"})

	handler.Handle(
		znz.RequestHeader{MaxOutputChars: 256},
		zenzPrompt("きょう\nは", "", "てんき"),
	)

	// The newline inside the left context is folded to a space so the state
	// string keeps one field per line.
	if strings.Contains(scorer.state, "きょう\nは") {
		t.Errorf("state contains a raw newline inside the context: %q", scorer.state)
	}
	if !strings.Contains(scorer.state, "きょう は") {
		t.Errorf("state = %q, want the newline folded to a space", scorer.state)
	}
}

func TestHandleWithoutLogger(t *testing.T) {
	scorer := &fakeScorer{answer: jev.Answer{Choice: "天気"}}
	handler, _ := newTestHandler(t, scorer, []string{"てんき\t天気\t転記"})
	handler.Log = nil

	// A missing logger must not turn into a panic on the user's keystroke path.
	result := handler.Handle(
		znz.RequestHeader{MaxOutputChars: 256},
		zenzPrompt("", "", "てんき"),
	)
	if result.Value != "天気" {
		t.Errorf("value = %q, want 天気", result.Value)
	}
}

// Timeout must be recognised structurally, not by matching error text: an
// upstream message change would silently turn a fallback into a hard failure.
func TestIsTimeoutUsesNetError(t *testing.T) {
	if !isTimeout(context.DeadlineExceeded) {
		t.Error("context.DeadlineExceeded must count as a timeout")
	}

	// net.Error whose Timeout() reports true, wrapped the way http.Client does.
	wrapped := fmt.Errorf("jev: post: %w", &net.OpError{
		Op:  "Post",
		Err: timeoutError{},
	})
	if !isTimeout(wrapped) {
		t.Error("a wrapped net.Error timeout must count as a timeout")
	}

	if isTimeout(errors.New("jev: http 500: Client.Timeout mentioned in text")) {
		t.Error("matching on error text is not allowed any more")
	}
	if isTimeout(nil) {
		t.Error("nil is not a timeout")
	}
}

// timeoutError is a net.Error that reports Timeout() == true.
type timeoutError struct{}

func (timeoutError) Error() string { return "i/o timeout" }
func (timeoutError) Timeout() bool { return true }
func (timeoutError) Temporary() bool {
	return true
}
