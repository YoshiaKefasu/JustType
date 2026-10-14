package jev

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

const (
	choiceResponse = `{"model":"jev-1.13-free","answers":{"choice":{"type":"choice",` +
		`"choice":"天気","confidence":0.78,"probabilities":{"天気":0.81,"転記":0,` +
		`"てんき":0.19}}},"usage":{"input_tokens":462,"output_tokens":77}}`
)

func newTestClient(t *testing.T, handler http.HandlerFunc) *Client {
	t.Helper()

	server := httptest.NewServer(handler)
	t.Cleanup(server.Close)

	client := New(server.URL, "jev-1.13-free", "", "")
	client.HTTPClient = server.Client()
	return client
}

func TestAskParsesChoice(t *testing.T) {
	client := newTestClient(t, func(w http.ResponseWriter, r *http.Request) {
		io.WriteString(w, choiceResponse)
	})

	answer, err := client.Ask(context.Background(), "左文脈: きょう\n読み: てんき", Question{
		Instructions: "文脈に最も合致する正しい日本語表記を選んでください",
		Options:      []string{"天気", "転記", "てんき"},
	})
	if err != nil {
		t.Fatalf("Ask: %v", err)
	}

	if answer.Choice != "天気" {
		t.Errorf("Choice = %q, want 天気", answer.Choice)
	}
	if answer.Confidence != 0.78 {
		t.Errorf("Confidence = %v, want 0.78", answer.Confidence)
	}
	if got := answer.Probabilities["天気"]; got != 0.81 {
		t.Errorf("Probabilities[天気] = %v, want 0.81", got)
	}
}

// The endpoint documents choice criteria as key -> description and echoes the
// chosen key, so the request must carry one criteria entry per surface.
func TestAskRequestShape(t *testing.T) {
	var body struct {
		Model     string `json:"model"`
		State     string `json:"state"`
		Questions map[string]struct {
			Type         string            `json:"type"`
			Instructions string            `json:"instructions"`
			Criteria     map[string]string `json:"criteria"`
		} `json:"questions"`
	}

	client := newTestClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			t.Errorf("method = %s, want POST", r.Method)
		}
		if got := r.Header.Get("Content-Type"); got != "application/json" {
			t.Errorf("Content-Type = %q", got)
		}
		if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
			t.Errorf("decode request: %v", err)
		}
		io.WriteString(w, choiceResponse)
	})

	state := "左文脈: きょう\n読み: てんき"
	_, err := client.Ask(context.Background(), state, Question{
		Instructions: "選んでください",
		Options:      []string{"天気", "てんき"},
	})
	if err != nil {
		t.Fatalf("Ask: %v", err)
	}

	if body.Model != "jev-1.13-free" {
		t.Errorf("model = %q", body.Model)
	}
	if body.State != state {
		t.Errorf("state = %q, want %q", body.State, state)
	}

	question, ok := body.Questions["choice"]
	if !ok {
		t.Fatalf("questions = %v, want a choice question", body.Questions)
	}
	if question.Type != "choice" {
		t.Errorf("type = %q, want choice", question.Type)
	}
	if len(question.Criteria) != 2 {
		t.Fatalf("criteria = %v, want one entry per surface", question.Criteria)
	}
	for _, surface := range []string{"天気", "てんき"} {
		description, present := question.Criteria[surface]
		if !present {
			t.Errorf("criteria is missing surface %q", surface)
		}
		if description == "" {
			t.Errorf("criteria[%q] description is empty", surface)
		}
	}
}

// The free model works keyless, so an unset key must omit the header entirely
// rather than sending "Bearer ".
func TestAskOmitsAuthorizationWhenKeyIsEmpty(t *testing.T) {
	client := newTestClient(t, func(w http.ResponseWriter, r *http.Request) {
		if _, present := r.Header["Authorization"]; present {
			t.Error("Authorization header must be absent when no key is set")
		}
		io.WriteString(w, choiceResponse)
	})

	if _, err := client.Ask(context.Background(), "s", Question{Options: []string{"天気"}}); err != nil {
		t.Fatalf("Ask: %v", err)
	}
}

func TestAskSendsConfiguredHeaders(t *testing.T) {
	client := newTestClient(t, func(w http.ResponseWriter, r *http.Request) {
		if got := r.Header.Get("Authorization"); got != "Bearer sk-test" {
			t.Errorf("Authorization = %q", got)
		}
		if got := r.Header.Get("User-Agent"); got != "opencode/1.18.31" {
			t.Errorf("User-Agent = %q", got)
		}
		for _, name := range []string{"x-session-id", "x-session-affinity", "X-Opencode-Session"} {
			if got := r.Header.Get(name); got != "ses_test" {
				t.Errorf("%s = %q, want ses_test", name, got)
			}
		}
		io.WriteString(w, choiceResponse)
	})
	client.APIKey = "sk-test"
	client.SessionID = "ses_test"

	if _, err := client.Ask(context.Background(), "s", Question{Options: []string{"天気"}}); err != nil {
		t.Fatalf("Ask: %v", err)
	}
}

// A renamed question id on the gateway must not hard-fail the bridge.
func TestAskFallsBackToFirstChoiceAnswer(t *testing.T) {
	client := newTestClient(t, func(w http.ResponseWriter, r *http.Request) {
		io.WriteString(w, `{"answers":{"renamed":{"type":"choice","choice":"天気","confidence":0.5}}}`)
	})

	answer, err := client.Ask(context.Background(), "s", Question{Options: []string{"天気", "転記"}})
	if err != nil {
		t.Fatalf("Ask: %v", err)
	}
	if answer.Choice != "天気" {
		t.Errorf("Choice = %q, want 天気", answer.Choice)
	}
}

// The chosen surface must be one that was offered.  Forwarding an unvetted
// string into the IME would be worse than reporting an error.
func TestAskRejectsUnofferedChoice(t *testing.T) {
	client := newTestClient(t, func(w http.ResponseWriter, r *http.Request) {
		io.WriteString(w, `{"answers":{"choice":{"type":"choice","choice":"発明","confidence":0.9}}}`)
	})

	_, err := client.Ask(context.Background(), "s", Question{Options: []string{"天気", "転記"}})
	if err == nil {
		t.Fatal("expected an error when the endpoint chose an unoffered surface")
	}
}

func TestAskNoChoice(t *testing.T) {
	client := newTestClient(t, func(w http.ResponseWriter, r *http.Request) {
		io.WriteString(w, `{"answers":{"choice":{"type":"choice","choice":"","confidence":0}}}`)
	})

	_, err := client.Ask(context.Background(), "s", Question{Options: []string{"天気"}})
	if err != ErrNoChoice {
		t.Errorf("error = %v, want ErrNoChoice", err)
	}
}

func TestAskHTTPError(t *testing.T) {
	client := newTestClient(t, func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusTooManyRequests)
		io.WriteString(w, `{"error":"rate limited"}`)
	})

	_, err := client.Ask(context.Background(), "s", Question{Options: []string{"天気"}})
	if err == nil {
		t.Fatal("expected an error for a non-2xx response")
	}
}

func TestAskMalformedJSON(t *testing.T) {
	client := newTestClient(t, func(w http.ResponseWriter, r *http.Request) {
		io.WriteString(w, `{"answers":`)
	})

	if _, err := client.Ask(context.Background(), "s", Question{Options: []string{"天気"}}); err == nil {
		t.Fatal("expected an error for malformed JSON")
	}
}

func TestAskRejectsEmptyOptions(t *testing.T) {
	client := newTestClient(t, func(w http.ResponseWriter, r *http.Request) {
		t.Error("the endpoint must not be called with no options")
	})

	if _, err := client.Ask(context.Background(), "s", Question{}); err == nil {
		t.Fatal("expected an error when there is nothing to choose from")
	}
}

// The bridge budgets the call with a context deadline; the client must honour
// it rather than hanging until its own 30 s backstop.
func TestAskHonoursContextDeadline(t *testing.T) {
	client := newTestClient(t, func(w http.ResponseWriter, r *http.Request) {
		time.Sleep(500 * time.Millisecond)
		io.WriteString(w, choiceResponse)
	})

	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Millisecond)
	defer cancel()

	start := time.Now()
	_, err := client.Ask(ctx, "s", Question{Options: []string{"天気"}})
	if err == nil {
		t.Fatal("expected the context deadline to fail the call")
	}
	if elapsed := time.Since(start); elapsed > 2*time.Second {
		t.Errorf("call took %v, want it to abort at the deadline", elapsed)
	}
}

func TestScriptHint(t *testing.T) {
	cases := []struct {
		surface string
		want    string
	}{
		{"天気", "漢字表記"},
		{"てんき", "ひらがな表記"},
		{"テンキ", "カタカナ表記"},
		{"ABC", "その他の表記"},
	}
	for _, tc := range cases {
		if got := scriptHint(tc.surface); got != tc.want {
			t.Errorf("scriptHint(%q) = %q, want %q", tc.surface, got, tc.want)
		}
	}
}

// A zero-value Client is a legitimate construction, so Ask must not panic on a
// nil transport.
func TestAskWithNilHTTPClient(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		io.WriteString(w, choiceResponse)
	}))
	defer server.Close()

	client := &Client{Endpoint: server.URL, Model: "jev-1.13-free"}
	if client.HTTPClient != nil {
		t.Fatal("precondition: the test needs a nil HTTPClient")
	}

	answer, err := client.Ask(context.Background(), "s", Question{Options: []string{"天気"}})
	if err != nil {
		t.Fatalf("Ask with a nil HTTPClient: %v", err)
	}
	if answer.Choice != "天気" {
		t.Errorf("Choice = %q, want 天気", answer.Choice)
	}
	if client.HTTPClient != nil {
		t.Error("Ask must not mutate the client; a concurrent caller would race")
	}
}
