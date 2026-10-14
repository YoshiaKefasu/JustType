// Command mozkey-jev-bridge relays Mozkey's ZNZ1 named pipe protocol to the
// OpenCode Zen System One endpoint (model jev-1.13-free) and records every
// exchange as a JSONL training dataset.
//
// Usage:
//
//	mozkey-jev-bridge [flags]
//
// The API key is read from -zen-key or the ZEN_API_KEY environment variable and
// is never logged.  The jev-1.13-free model also answers without a key, so an
// unset key simply omits the Authorization header.
package main

import (
	"errors"
	"flag"
	"log"
	"os"
	"os/signal"
	"path/filepath"
	"syscall"
	"time"

	"golang.org/x/sys/windows"

	"mozkey-jev-bridge/internal/bridge"
	"mozkey-jev-bridge/internal/candidates"
	"mozkey-jev-bridge/internal/dataset"
	"mozkey-jev-bridge/internal/jev"
	"mozkey-jev-bridge/internal/pipe"
	"mozkey-jev-bridge/internal/znz"
)

const (
	defaultEndpoint = "https://opencode.ai/zen/v1/systemone"
	defaultModel    = "jev-1.13-free"
	defaultSession  = "ses_019a00000000AbCdEfGhIjKlMn"
	defaultBudget   = 2500 * time.Millisecond
)

// singleInstanceMutex is held for the lifetime of the process so a second
// bridge cannot bind the same pipe name.
var singleInstanceMutex windows.Handle

func main() {
	log.SetFlags(log.LstdFlags | log.Lmsgprefix)
	log.SetPrefix("mozkey-jev-bridge: ")

	executableDir := executableDir()

	pipeName := flag.String("pipe", pipe.DefaultPipeName, "named pipe path to serve")
	endpoint := flag.String("zen-endpoint", defaultEndpoint, "System One endpoint URL")
	model := flag.String("zen-model", defaultModel, "System One model id")
	zenKey := flag.String("zen-key", os.Getenv("ZEN_API_KEY"),
		"Zen API key; falls back to $ZEN_API_KEY. Empty omits the header (the free model needs none)")
	sessionID := flag.String("session-id", defaultSession, "OpenCode session id sent as the session headers")
	budget := flag.Duration("http-timeout", defaultBudget,
		"ceiling for one Jev call; Mozkey gives up at ~3.5 s regardless")
	logPath := flag.String("log-path", filepath.Join(executableDir, "jev_dataset.jsonl"),
		"JSONL dataset output path")
	candidatesPath := flag.String("candidates", filepath.Join(executableDir, "data", "candidates.tsv.gz"),
		"reading -> candidate index generated from the Mozc dictionary")
	maxOptions := flag.Int("max-options", 6, "maximum candidates offered to Jev")
	dryRun := flag.Bool("dry-run", false,
		"answer with the dictionary ranking without calling Jev; use this to verify the pipe wiring")
	verbose := flag.Bool("verbose", false, "log every request")
	flag.Parse()

	index, err := candidates.Load(*candidatesPath)
	if err != nil {
		log.Fatalf("load candidate index: %v", err)
	}

	logger, err := dataset.Open(*logPath)
	if err != nil {
		log.Fatalf("open dataset log: %v", err)
	}
	defer logger.Close()

	// Flush the dataset on Ctrl-C and exit instead of staying parked in
	// ConnectNamedPipe until the next client shows up.
	shutdown := make(chan os.Signal, 1)
	signal.Notify(shutdown, os.Interrupt, syscall.SIGTERM)
	go func() {
		<-shutdown
		if err := logger.Close(); err != nil {
			log.Printf("close dataset log: %v", err)
		}
		os.Exit(0)
	}()

	scorer := jev.New(*endpoint, *model, *zenKey, *sessionID)

	handler := &bridge.Handler{
		Scorer:       scorer,
		Index:        index,
		Log:          logger,
		MaxOptions:   *maxOptions,
		Budget:       *budget,
		Instructions: bridge.DefaultInstructions,
		DryRun:       *dryRun,
	}

	// Hold the single-instance guard before announcing anything: a second
	// bridge on the same pipe name would silently split requests between two
	// backends.  A package-level handle is never collected, and the OS releases
	// the mutex on exit, which is exactly the lifetime we want.
	singleInstanceMutex, err = pipe.AcquireSingleInstance("")
	switch {
	case errors.Is(err, pipe.ErrAlreadyRunning):
		logger.Close()
		log.Printf("another bridge instance already owns %s; exiting", *pipeName)
		os.Exit(0)
	case err != nil:
		logger.Close()
		log.Fatalf("acquire single-instance guard: %v", err)
	}

	log.Printf("serving %s", *pipeName)
	log.Printf("index: %d readings (%s)", index.Len(), *candidatesPath)
	log.Printf("dataset: %s", *logPath)
	if *dryRun {
		log.Printf("dry run: Jev will not be called")
	} else {
		log.Printf("model: %s via %s", *model, *endpoint)
	}

	if err := pipe.Serve(pipe.Config{PipeName: *pipeName}, func(
		header znz.RequestHeader, promptBytes []byte) pipe.Outcome {
		result := handler.Handle(header, promptBytes)

		if *verbose {
			log.Printf("gen=%d reading=%s options=%d choice=%q confidence=%.2f status=%d debug=%s latency=%dms asked=%v",
				result.Record.Generation, result.Record.Reading, len(result.Record.Options),
				result.Record.Choice, result.Record.Confidence, result.Status, result.Debug,
				result.Record.LatencyMS, result.Asked)
		}
		if result.Error != nil && result.Status != znz.StatusTimeout {
			log.Printf("gen=%d reading=%s: %v", result.Record.Generation,
				result.Record.Reading, result.Error)
		}

		return pipe.Outcome{Value: result.Value, Status: result.Status, Debug: result.Debug}
	}); err != nil {
		log.Fatalf("serve: %v", err)
	}
}

// executableDir returns the directory holding this binary so the dataset and the
// candidate index default to sitting next to it.
func executableDir() string {
	path, err := os.Executable()
	if err != nil {
		return "."
	}
	return filepath.Dir(path)
}
