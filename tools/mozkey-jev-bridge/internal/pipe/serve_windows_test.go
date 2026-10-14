//go:build windows

package pipe_test

import (
	"compress/gzip"
	"context"
	"encoding/binary"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"

	"mozkey-jev-bridge/internal/bridge"
	"mozkey-jev-bridge/internal/candidates"
	"mozkey-jev-bridge/internal/dataset"
	"mozkey-jev-bridge/internal/jev"
	"mozkey-jev-bridge/internal/pipe"
	"mozkey-jev-bridge/internal/znz"
)

// fakeScorer stands in for the remote model so the pipe test exercises the whole
// path except the HTTP call.
type fakeScorer struct {
	calls int
	state string
}

func (f *fakeScorer) Ask(ctx context.Context, state string, question jev.Question) (jev.Answer, error) {
	f.calls++
	f.state = state
	return jev.Answer{Choice: "天気", Confidence: 0.83}, nil
}

func writeIndex(t *testing.T) string {
	t.Helper()

	path := filepath.Join(t.TempDir(), "candidates.tsv.gz")
	file, err := os.Create(path)
	if err != nil {
		t.Fatalf("create index: %v", err)
	}
	defer file.Close()

	writer := gzip.NewWriter(file)
	if _, err := writer.Write([]byte("てんき\t天気\t天鬼\t転記\n")); err != nil {
		t.Fatalf("write index: %v", err)
	}
	if err := writer.Close(); err != nil {
		t.Fatalf("close gzip writer: %v", err)
	}
	return path
}

func buildHandler(t *testing.T, scorer bridge.Scorer) (*bridge.Handler, string) {
	t.Helper()

	index, err := candidates.Load(writeIndex(t))
	if err != nil {
		t.Fatalf("load index: %v", err)
	}

	logPath := filepath.Join(t.TempDir(), "jev_dataset.jsonl")
	logger, err := dataset.Open(logPath)
	if err != nil {
		t.Fatalf("open dataset: %v", err)
	}
	t.Cleanup(func() { logger.Close() })

	return &bridge.Handler{
		Scorer:     scorer,
		Index:      index,
		Log:        logger,
		MaxOptions: 6,
		Budget:     5 * time.Second,
	}, logPath
}

func handlerFunc(handler *bridge.Handler) pipe.HandleFunc {
	return func(header znz.RequestHeader, promptBytes []byte) pipe.Outcome {
		result := handler.Handle(header, promptBytes)
		return pipe.Outcome{Value: result.Value, Status: result.Status, Debug: result.Debug}
	}
}

// testPipeName keeps each test off the production pipe and off each other.
func testPipeName(tag string) string {
	return `\\.\pipe\mozkey_jev_bridge_test_` + tag + "_" +
		strconv.FormatInt(time.Now().UnixNano(), 10)
}

// connect opens the pipe the way Mozkey does, retrying until the server has
// created its first instance.
func connect(t *testing.T, name string) *os.File {
	t.Helper()

	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		file, err := os.OpenFile(name, os.O_RDWR, 0)
		if err == nil {
			return file
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatalf("could not connect to %s in time", name)
	return nil
}

func zenzPrompt(left, reading string) []byte {
	return []byte("\uee02" + left + "\uee00" + reading + "\uee01")
}

// request builds a complete ZNZ1 request frame.
func request(t *testing.T, generation uint32, prompt []byte) []byte {
	t.Helper()

	frame := znz.EncodeRequestHeader(znz.RequestHeader{
		Generation:     generation,
		TimeoutMsec:    1000,
		MaxOutputChars: 256,
		PromptSize:     uint32(len(prompt)),
	})
	return append(frame, prompt...)
}

// readResponseFrame reads a whole response frame and returns its fields.
type response struct {
	generation uint32
	status     uint32
	value      string
	debug      string
}

func readResponseFrame(t *testing.T, conn io.Reader) response {
	t.Helper()

	header := make([]byte, znz.ResponseHeaderSize)
	if _, err := io.ReadFull(conn, header); err != nil {
		t.Fatalf("read response header: %v", err)
	}

	if got := binary.LittleEndian.Uint32(header[0:4]); got != znz.Magic {
		t.Fatalf("magic = %#x, want %#x", got, znz.Magic)
	}
	if got := binary.LittleEndian.Uint16(header[4:6]); got != znz.Version {
		t.Errorf("version = %d, want %d", got, znz.Version)
	}
	if got := binary.LittleEndian.Uint16(header[6:8]); got != znz.KindResponse {
		t.Errorf("kind = %d, want %d", got, znz.KindResponse)
	}

	valueSize := binary.LittleEndian.Uint32(header[20:24])
	debugSize := binary.LittleEndian.Uint32(header[24:28])

	value := make([]byte, valueSize)
	if _, err := io.ReadFull(conn, value); err != nil {
		t.Fatalf("read value: %v", err)
	}
	debug := make([]byte, debugSize)
	if _, err := io.ReadFull(conn, debug); err != nil {
		t.Fatalf("read debug: %v", err)
	}

	return response{
		generation: binary.LittleEndian.Uint32(header[8:12]),
		status:     binary.LittleEndian.Uint32(header[12:16]),
		value:      string(value),
		debug:      string(debug),
	}
}

// TestServeRoundTrip drives the real Windows named pipe end to end: request
// frame in, response frame out, through the bridge, the fake scorer and the
// dataset logger.
func TestServeRoundTrip(t *testing.T) {
	scorer := &fakeScorer{}
	handler, logPath := buildHandler(t, scorer)

	name := testPipeName("roundtrip")
	go pipe.Serve(pipe.Config{PipeName: name}, handlerFunc(handler))

	conn := connect(t, name)
	defer conn.Close()

	prompt := zenzPrompt("きょうは いい", "てんき")
	if _, err := conn.Write(request(t, 7, prompt)); err != nil {
		t.Fatalf("write request: %v", err)
	}

	got := readResponseFrame(t, conn)
	if got.generation != 7 {
		t.Errorf("generation = %d, want 7", got.generation)
	}
	if got.status != znz.StatusOK {
		t.Errorf("status = %d, want OK", got.status)
	}
	if got.value != "天気" {
		t.Errorf("value = %q, want 天気", got.value)
	}
	if got.debug != bridge.DebugOK {
		t.Errorf("debug = %q, want %q", got.debug, bridge.DebugOK)
	}
	if scorer.calls != 1 {
		t.Errorf("scorer calls = %d, want 1", scorer.calls)
	}
	wantState := "左文脈: きょうは いい\n読み: てんき"
	if scorer.state != wantState {
		t.Errorf("state = %q, want %q", scorer.state, wantState)
	}

	raw, err := os.ReadFile(logPath)
	if err != nil {
		t.Fatalf("read dataset: %v", err)
	}
	if !strings.Contains(string(raw), "天気") {
		t.Errorf("dataset = %q, want the recorded choice", raw)
	}
}

// The accept loop must survive many connections in a row, including one that
// disappears without writing a frame.
func TestServeHandlesConsecutiveConnections(t *testing.T) {
	scorer := &fakeScorer{}
	handler, _ := buildHandler(t, scorer)

	name := testPipeName("consecutive")
	go pipe.Serve(pipe.Config{PipeName: name}, handlerFunc(handler))

	for i := 0; i < 3; i++ {
		conn := connect(t, name)

		generation := uint32(100 + i)
		if _, err := conn.Write(request(t, generation, zenzPrompt("きょうは", "てんき"))); err != nil {
			t.Fatalf("connection %d: write request: %v", i, err)
		}

		got := readResponseFrame(t, conn)
		if got.generation != generation {
			t.Errorf("connection %d: generation = %d, want %d", i, got.generation, generation)
		}
		if got.value != "天気" {
			t.Errorf("connection %d: value = %q, want 天気", i, got.value)
		}
		conn.Close()
	}

	// A client that connects and vanishes must not wedge the loop.
	victim := connect(t, name)
	if err := victim.Close(); err != nil {
		t.Fatalf("close vanished client: %v", err)
	}

	final := connect(t, name)
	defer final.Close()
	if _, err := final.Write(request(t, 999, zenzPrompt("あしたは", "てんき"))); err != nil {
		t.Fatalf("write final request: %v", err)
	}
	got := readResponseFrame(t, final)
	if got.generation != 999 {
		t.Errorf("final generation = %d, want 999", got.generation)
	}
}

// A malformed frame must be answered with status Error rather than tearing down
// the server.
func TestServeAnswersMalformedRequest(t *testing.T) {
	scorer := &fakeScorer{}
	handler, _ := buildHandler(t, scorer)

	name := testPipeName("malformed")
	go pipe.Serve(pipe.Config{PipeName: name}, handlerFunc(handler))

	conn := connect(t, name)
	defer conn.Close()

	// A header whose prompt_size promises 5000 bytes but never sends them.
	frame := znz.EncodeRequestHeader(znz.RequestHeader{PromptSize: 5000, MaxOutputChars: 256})
	if _, err := conn.Write(frame); err != nil {
		t.Fatalf("write request: %v", err)
	}

	// The bridge cannot tell a truncated frame from a vanished client, so it
	// closes the connection.  Either way it must still serve the next client.
	conn.Close()

	next := connect(t, name)
	defer next.Close()
	if _, err := next.Write(request(t, 11, zenzPrompt("きょうは", "てんき"))); err != nil {
		t.Fatalf("write follow-up request: %v", err)
	}
	got := readResponseFrame(t, next)
	if got.status != znz.StatusOK || got.value != "天気" {
		t.Errorf("follow-up: status = %d value = %q, want OK / 天気", got.status, got.value)
	}
}

// The pipe name defaults to the path Mozkey expects, so a missing config must
// not silently serve somewhere else.
func TestDefaultPipeName(t *testing.T) {
	if pipe.DefaultPipeName != `\\.\pipe\mozc_zenz_scorer` {
		t.Errorf("DefaultPipeName = %q", pipe.DefaultPipeName)
	}
}

// A client that violates the protocol is alive and talking, so it gets an error
// response with the generation echoed back, the way HandleClient answers
// bad_request_header.  Dropping it silently would leave Mozkey waiting for a
// frame until its own failsafe fires.
func TestServeAnswersProtocolViolation(t *testing.T) {
	scorer := &fakeScorer{}
	handler, _ := buildHandler(t, scorer)

	name := testPipeName("violation")
	go pipe.Serve(pipe.Config{PipeName: name}, handlerFunc(handler))

	conn := connect(t, name)
	defer conn.Close()

	// A frame with a valid generation but a corrupted magic.
	frame := znz.EncodeRequestHeader(znz.RequestHeader{
		Generation:     0xBEEF,
		PromptSize:     1,
		MaxOutputChars: 256,
	})
	frame[0] = 0x00
	if _, err := conn.Write(frame); err != nil {
		t.Fatalf("write request: %v", err)
	}

	got := readResponseFrame(t, conn)
	if got.status != znz.StatusError {
		t.Errorf("status = %d, want Error", got.status)
	}
	if got.generation != 0xBEEF {
		t.Errorf("generation = %#x, want 0xbeef echoed back", got.generation)
	}
	if got.value != "" {
		t.Errorf("value = %q, want empty on a protocol violation", got.value)
	}
	if got.debug != "bad_magic" {
		t.Errorf("debug = %q, want bad_magic", got.debug)
	}
	if scorer.calls != 0 {
		t.Errorf("scorer calls = %d, want 0 for a malformed frame", scorer.calls)
	}
}

// An oversized prompt is the other violation Mozc reports explicitly.
func TestServeAnswersOversizedPrompt(t *testing.T) {
	scorer := &fakeScorer{}
	handler, _ := buildHandler(t, scorer)

	name := testPipeName("oversized")
	go pipe.Serve(pipe.Config{PipeName: name}, handlerFunc(handler))

	conn := connect(t, name)
	defer conn.Close()

	frame := znz.EncodeRequestHeader(znz.RequestHeader{
		Generation:     77,
		PromptSize:     znz.MaxPromptBytes + 1,
		MaxOutputChars: 256,
	})
	if _, err := conn.Write(frame); err != nil {
		t.Fatalf("write request: %v", err)
	}

	got := readResponseFrame(t, conn)
	if got.status != znz.StatusError {
		t.Errorf("status = %d, want Error", got.status)
	}
	if got.debug != "prompt_too_large" {
		t.Errorf("debug = %q, want prompt_too_large", got.debug)
	}

	// The server must still be alive afterwards.
	next := connect(t, name)
	defer next.Close()
	if _, err := next.Write(request(t, 88, zenzPrompt("きょうは", "てんき"))); err != nil {
		t.Fatalf("write follow-up request: %v", err)
	}
	if got := readResponseFrame(t, next); got.status != znz.StatusOK {
		t.Errorf("follow-up status = %d, want OK", got.status)
	}
}
