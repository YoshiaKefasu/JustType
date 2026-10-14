// Package bridge turns one ZNZ1 request into one ZNZ1 response.
//
// The flow per request is:
//
//	parse the Zenz prompt -> resolve candidates from the Mozc index
//	  -> ask Jev to pick one -> return it to Mozkey and log the exchange
//
// Three paths never touch the network:
//
//	no reading in the prompt      -> status Error, empty value
//	exactly one dictionary hit    -> return it directly
//	reading absent from the index -> return the kana reading
//
// The first two are unambiguous, so asking Jev would only add latency and cost.
// The third is the honest answer: with no candidate set there is nothing to
// choose between, and silently inventing a surface would put a wrong
// conversion in front of the user.
package bridge

import (
	"context"
	"errors"
	"log"
	"net"
	"time"

	"mozkey-jev-bridge/internal/candidates"
	"mozkey-jev-bridge/internal/dataset"
	"mozkey-jev-bridge/internal/jev"
	"mozkey-jev-bridge/internal/prompt"
	"mozkey-jev-bridge/internal/znz"
)

// DefaultInstructions is the prompt Jev sees for the choice question.
const DefaultInstructions = "文脈に最も合致する正しい日本語表記を選んでください"

// Debug tokens written into the ZNZ1 response debug field.  They follow the
// short-token convention mozc_zenz_scorer already uses.
const (
	DebugOK              = "jev_ok"
	DebugNoReading       = "no_reading"
	DebugNoCandidates    = "no_candidates"
	DebugSingleCandidate = "single_candidate"
	DebugTimeout         = "jev_timeout"
	DebugError           = "jev_error"
	DebugDryRun          = "dry_run"
)

// Scorer is the subset of jev.Client the bridge needs, so tests can substitute
// a fake.
type Scorer interface {
	Ask(ctx context.Context, state string, question jev.Question) (jev.Answer, error)
}

// Handler resolves requests.  The zero value is not usable.
type Handler struct {
	Scorer       Scorer
	Index        *candidates.Index
	Log          *dataset.Logger
	MaxOptions   int
	Budget       time.Duration
	Instructions string
	DryRun       bool
}

// Result is the outcome of one request.
type Result struct {
	Value  string
	Status uint32
	Debug  string

	// Asked reports whether Jev was actually consulted.  Dry runs and the
	// short-circuit paths leave it false.
	Asked bool
	// Error is the underlying failure when Status is not OK, for logging.
	Error error
	// Record is ready to append to the dataset; Callers append it even on
	// failure so the dataset records what the bridge saw.
	Record dataset.Record
}

// Handle processes one request.  It never panics on malformed input; every
// failure is expressed as a status code.
func (h *Handler) Handle(header znz.RequestHeader, promptBytes []byte) Result {
	started := time.Now()
	parsed := prompt.Parse(string(promptBytes))

	record := dataset.Record{
		Generation:   header.Generation,
		LeftContext:  parsed.LeftContext,
		Reading:      parsed.Hiragana(),
		RightContext: parsed.RightContext,
	}

	finish := func(status uint32, value, debug string, asked bool, err error) Result {
		record.Choice = value
		record.LatencyMS = time.Since(started).Milliseconds()
		h.append(record)
		return Result{
			Value:  value,
			Status: status,
			Debug:  debug,
			Asked:  asked,
			Error:  err,
			Record: record,
		}
	}

	if parsed.Reading == "" {
		return finish(znz.StatusError, "", DebugNoReading, false, errors.New("prompt carried no reading"))
	}

	surfaces := h.Index.Surfaces(record.Reading)

	// One candidate means the reading is unambiguous; the dictionary answer is
	// also what Mozc would show, so skip the round trip.
	if len(surfaces) == 1 {
		record.Options = surfaces
		value := znz.TruncateRunes(surfaces[0], header.OutputCharLimit())
		return finish(znz.StatusOK, value, DebugSingleCandidate, false, nil)
	}

	// No candidates means the reading is not in the dictionary as a word.
	// Return the hiragana the user typed: the prompt carries katakana because
	// ZenzPromptBuilder converted it, and echoing that back would display テンキ
	// where the user typed てんき.
	if len(surfaces) == 0 {
		record.Options = nil
		value := znz.TruncateRunes(record.Reading, header.OutputCharLimit())
		return finish(znz.StatusOK, value, DebugNoCandidates, false, nil)
	}

	options := surfaces
	if h.MaxOptions > 0 && len(options) > h.MaxOptions {
		options = options[:h.MaxOptions]
	}
	record.Options = options

	if h.DryRun {
		// Dry run answers with the dictionary ranking so the pipe wiring can be
		// verified without spending API calls.
		value := znz.TruncateRunes(options[0], header.OutputCharLimit())
		return finish(znz.StatusOK, value, DebugDryRun, false, nil)
	}

	budget := h.budgetFor(header)
	ctx, cancel := context.WithTimeout(context.Background(), budget)
	defer cancel()

	answer, err := h.Scorer.Ask(ctx, parsed.State(), jev.Question{
		Instructions: h.instruction(),
		Options:      options,
	})
	if err != nil {
		if errors.Is(err, context.DeadlineExceeded) || isTimeout(err) {
			return finish(znz.StatusTimeout, "", DebugTimeout, true, err)
		}
		return finish(znz.StatusError, "", DebugError, true, err)
	}

	record.Choice = answer.Choice
	record.Confidence = answer.Confidence

	value := znz.TruncateRunes(answer.Choice, header.OutputCharLimit())
	return finish(znz.StatusOK, value, DebugOK, true, nil)
}

func (h *Handler) instruction() string {
	if h.Instructions != "" {
		return h.Instructions
	}
	return DefaultInstructions
}

// budgetFor picks the HTTP budget for this request.
//
// Mozkey's named pipe client gives up at about 3.5 s regardless of the timeout
// the request carries (ZenzNamedPipeClient::kDefaultTransportSafetyTimeoutMsec),
// so the bridge never plans to use more than the configured ceiling even when
// the request asks for 5 s.
func (h *Handler) budgetFor(header znz.RequestHeader) time.Duration {
	budget := h.Budget
	if budget <= 0 {
		budget = 2500 * time.Millisecond
	}

	requested := time.Duration(header.ClampedTimeoutMsec()) * time.Millisecond
	if requested < budget {
		budget = requested
	}
	// Leave room to write the response frame after the HTTP call returns.
	if budget > 200*time.Millisecond {
		budget -= 100 * time.Millisecond
	}
	return budget
}

func (h *Handler) append(record dataset.Record) {
	if h.Log == nil {
		return
	}
	// A logging failure must not fail the conversion: the user is typing.
	if err := h.Log.Append(record); err != nil {
		log.Printf("dataset append failed: %v", err)
	}
}

// isTimeout recognises a deadline expiry without matching on error text.
//
// net/http surfaces a context deadline either as context.DeadlineExceeded or as
// an *url.Error wrapping an error whose Timeout() reports true, so both are
// checked.  String matching is deliberately avoided: an upstream message change
// would silently turn a timeout into a hard error and show the user an empty
// conversion instead of falling back.
func isTimeout(err error) bool {
	if err == nil {
		return false
	}
	if errors.Is(err, context.DeadlineExceeded) {
		return true
	}

	var netErr net.Error
	return errors.As(err, &netErr) && netErr.Timeout()
}
