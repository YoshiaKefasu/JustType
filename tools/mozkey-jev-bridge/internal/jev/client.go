// Package jev talks to the OpenCode Zen System One endpoint that hosts Jev.
//
// Jev does not generate text: it reads a state string and answers typed
// questions.  The bridge uses a single "choice" question whose criteria are the
// Mozc candidate surfaces, and takes the chosen key as the conversion result.
//
// Verified against the live endpoint (https://opencode.ai/zen/v1/systemone,
// model jev-1.13-free):
//
//	request  {"model":...,"state":...,"questions":{"choice":{"type":"choice",
//	          "instructions":...,"criteria":{"<surface>":"<hint>",...}}}}
//	response {"model":...,"answers":{"choice":{"type":"choice",
//	          "choice":"<surface>","confidence":0.78,
//	          "probabilities":{"<surface>":0.81,...}}}}
package jev

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"time"
	"unicode"
)

// ErrNoChoice means the endpoint answered but without a usable choice, which is
// a protocol surprise rather than a transport failure.
var ErrNoChoice = errors.New("jev: response carried no choice")

// Question is one choice question over a shared state.
type Question struct {
	Instructions string
	Options      []string // ordered, best first
}

// Answer is the parsed decision for a choice question.
type Answer struct {
	Choice        string
	Confidence    float64
	Probabilities map[string]float64
}

// Client is a System One client.
//
// The zero value is not usable; construct it with New or fill the exported
// fields.  HTTP is injected so tests can point it at an httptest server.
type Client struct {
	Endpoint  string
	Model     string
	APIKey    string
	SessionID string
	UserAgent string

	HTTPClient *http.Client
}

// New builds a client with sane defaults for the OpenCode Zen gateway.
func New(endpoint, model, apiKey, sessionID string) *Client {
	return &Client{
		Endpoint:  endpoint,
		Model:     model,
		APIKey:    apiKey,
		SessionID: sessionID,
		UserAgent: "opencode/1.18.31",
		HTTPClient: &http.Client{
			// The transport timeout is a backstop; callers pass a context with
			// the real budget so a request can be cancelled mid-flight.
			Timeout: 30 * time.Second,
		},
	}
}

// requestPayload is the JSON body.  Options become a criteria map because the
// endpoint documents choice criteria as key -> description, and the answer
// echoes the chosen key.
type requestPayload struct {
	Model     string                 `json:"model"`
	State     string                 `json:"state"`
	Questions map[string]questionDef `json:"questions"`
}

type questionDef struct {
	Type         string            `json:"type"`
	Instructions string            `json:"instructions"`
	Criteria     map[string]string `json:"criteria"`
}

type responsePayload struct {
	Model   string                `json:"model"`
	Answers map[string]answerBody `json:"answers"`
	Error   string                `json:"error"`
	Message string                `json:"message"`
}

type answerBody struct {
	Type          string             `json:"type"`
	Choice        string             `json:"choice"`
	Confidence    float64            `json:"confidence"`
	Probabilities map[string]float64 `json:"probabilities"`
}

// Ask evaluates state against a single choice question.
//
// The returned error is non-nil on any transport failure, protocol surprise, or
// when the endpoint picks a surface that was not offered; the caller must never
// forward an unvetted string into the IME.
func (c *Client) Ask(ctx context.Context, state string, question Question) (Answer, error) {
	// Resolve the transport once, locally.  Assigning to the struct field would
	// be a data race if Ask ever ran concurrently, and a zero-value Client is a
	// legitimate construction.
	httpClient := c.HTTPClient
	if httpClient == nil {
		httpClient = http.DefaultClient
	}

	if len(question.Options) == 0 {
		return Answer{}, errors.New("jev: no options to choose from")
	}

	criteria := make(map[string]string, len(question.Options))
	for _, option := range question.Options {
		criteria[option] = scriptHint(option)
	}

	body, err := json.Marshal(requestPayload{
		Model: c.Model,
		State: state,
		Questions: map[string]questionDef{
			"choice": {
				Type:         "choice",
				Instructions: question.Instructions,
				Criteria:     criteria,
			},
		},
	})
	if err != nil {
		return Answer{}, fmt.Errorf("jev: marshal request: %w", err)
	}

	request, err := http.NewRequestWithContext(ctx, http.MethodPost, c.Endpoint,
		bytes.NewReader(body))
	if err != nil {
		return Answer{}, fmt.Errorf("jev: build request: %w", err)
	}
	request.Header.Set("Content-Type", "application/json")
	if c.UserAgent != "" {
		request.Header.Set("User-Agent", c.UserAgent)
	}
	// The free model works keyless; only send the header when a key is
	// configured so an unset key cannot turn into "Bearer ".
	if c.APIKey != "" {
		request.Header.Set("Authorization", "Bearer "+c.APIKey)
	}
	if c.SessionID != "" {
		request.Header.Set("x-session-id", c.SessionID)
		request.Header.Set("x-session-affinity", c.SessionID)
		request.Header.Set("X-Opencode-Session", c.SessionID)
	}

	response, err := httpClient.Do(request)
	if err != nil {
		return Answer{}, fmt.Errorf("jev: post: %w", err)
	}
	defer response.Body.Close()

	// LimitReader caps the read so a runaway endpoint cannot exhaust memory.
	raw, err := io.ReadAll(io.LimitReader(response.Body, 1<<20))
	if err != nil {
		return Answer{}, fmt.Errorf("jev: read response: %w", err)
	}

	if response.StatusCode < 200 || response.StatusCode > 299 {
		return Answer{}, fmt.Errorf("jev: http %d: %s", response.StatusCode,
			truncateForError(raw))
	}

	var parsed responsePayload
	if err := json.Unmarshal(raw, &parsed); err != nil {
		return Answer{}, fmt.Errorf("jev: decode response: %w", err)
	}

	answer, ok := parsed.Answers["choice"]
	if !ok || answer.Type != "" && answer.Type != "choice" {
		// Fall back to the first choice-shaped answer so a renamed question id
		// on the gateway does not hard-fail the bridge.
		answer, ok = firstChoiceAnswer(parsed.Answers)
	}
	if !ok || answer.Choice == "" {
		return Answer{}, ErrNoChoice
	}

	if !contains(question.Options, answer.Choice) {
		return Answer{}, fmt.Errorf("jev: chose %q which was not offered", answer.Choice)
	}

	return Answer{
		Choice:        answer.Choice,
		Confidence:    answer.Confidence,
		Probabilities: answer.Probabilities,
	}, nil
}

func firstChoiceAnswer(answers map[string]answerBody) (answerBody, bool) {
	for _, answer := range answers {
		if answer.Type == "choice" && answer.Choice != "" {
			return answer, true
		}
	}
	return answerBody{}, false
}

// scriptHint labels a surface by script so the criteria descriptions carry
// signal instead of being empty strings.
func scriptHint(surface string) string {
	hasKanji, hasKana, hasKatakana := false, false, false
	for _, r := range surface {
		switch {
		case unicode.In(r, unicode.Hiragana):
			hasKana = true
		case unicode.In(r, unicode.Katakana):
			hasKatakana = true
		case unicode.In(r, unicode.Han):
			hasKanji = true
		}
	}
	switch {
	case hasKanji:
		return "漢字表記"
	case hasKatakana:
		return "カタカナ表記"
	case hasKana:
		return "ひらがな表記"
	default:
		return "その他の表記"
	}
}

func contains(options []string, value string) bool {
	for _, option := range options {
		if option == value {
			return true
		}
	}
	return false
}

func truncateForError(raw []byte) string {
	const limit = 512
	if len(raw) <= limit {
		return string(raw)
	}
	return string(raw[:limit]) + "..."
}
