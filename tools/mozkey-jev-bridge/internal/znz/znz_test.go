package znz

import (
	"bytes"
	"encoding/hex"
	"errors"
	"io"
	"strings"
	"testing"
)

// The C++ header src/zenz/zenz_wire_protocol.h is the contract.  These hex
// literals are a byte-for-byte transcription of what that header produces for a
// known input, so a field-order or endianness regression fails here instead of
// silently confusing Mozkey.
func TestEncodeRequestHeaderMatchesWireLayout(t *testing.T) {
	got := EncodeRequestHeader(RequestHeader{
		Generation:     123,
		TimeoutMsec:    1000,
		MaxOutputChars: 256,
		PromptSize:     5,
	})

	want, err := hex.DecodeString(
		"5a4e5a31" + // magic "ZNZ1"
			"0100" + // version
			"0100" + // kind request
			"7b000000" + // generation 123
			"e8030000" + // timeout 1000
			"00010000" + // max_output_chars 256
			"05000000") // prompt_size 5
	if err != nil {
		t.Fatalf("bad test literal: %v", err)
	}

	if !bytes.Equal(got, want) {
		t.Errorf("request header mismatch\n got %x\nwant %x", got, want)
	}
	if len(got) != RequestHeaderSize {
		t.Errorf("request header size = %d, want %d", len(got), RequestHeaderSize)
	}
}

func TestEncodeResponseHeaderMatchesWireLayout(t *testing.T) {
	got := EncodeResponseHeader(ResponseHeader{
		Generation:  123,
		Status:      StatusOK,
		LatencyMsec: 42,
		ValueSize:   3,
		DebugSize:   2,
	})

	want, err := hex.DecodeString(
		"5a4e5a31" +
			"0100" + // version
			"0200" + // kind response
			"7b000000" + // generation 123
			"00000000" + // status OK
			"2a000000" + // latency 42
			"03000000" + // value_size 3
			"02000000") // debug_size 2
	if err != nil {
		t.Fatalf("bad test literal: %v", err)
	}

	if !bytes.Equal(got, want) {
		t.Errorf("response header mismatch\n got %x\nwant %x", got, want)
	}
	if len(got) != ResponseHeaderSize {
		t.Errorf("response header size = %d, want %d", len(got), ResponseHeaderSize)
	}
}

func TestRequestHeaderRoundTrip(t *testing.T) {
	original := RequestHeader{
		Generation:     0xDEADBEEF,
		TimeoutMsec:    1500,
		MaxOutputChars: 64,
		PromptSize:     128,
	}

	decoded, err := DecodeRequestHeader(EncodeRequestHeader(original))
	if err != nil {
		t.Fatalf("DecodeRequestHeader: %v", err)
	}
	if decoded != original {
		t.Errorf("round trip = %+v, want %+v", decoded, original)
	}
}

func TestDecodeRequestHeaderRejectsBadFrames(t *testing.T) {
	valid := EncodeRequestHeader(RequestHeader{PromptSize: 1})

	cases := []struct {
		name   string
		mutate func([]byte) []byte
		want   error
	}{
		{"short", func(b []byte) []byte { return b[:10] }, ErrShortHeader},
		{"bad magic", func(b []byte) []byte {
			b[0] = 0
			return b
		}, ErrBadMagic},
		{"bad version", func(b []byte) []byte {
			b[4] = 9
			return b
		}, ErrBadVersion},
		{"bad kind", func(b []byte) []byte {
			b[7] = 2
			return b
		}, ErrBadKind},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			_, err := DecodeRequestHeader(tc.mutate(append([]byte(nil), valid...)))
			if err == nil {
				t.Fatal("expected an error, got nil")
			}
			if !isError(err, tc.want) {
				t.Errorf("error = %v, want %v", err, tc.want)
			}
		})
	}
}

func TestReadRequestRejectsOversizedPrompt(t *testing.T) {
	header := EncodeRequestHeader(RequestHeader{PromptSize: MaxPromptBytes + 1})
	if _, err := ReadRequest(bytes.NewReader(header)); !isError(err, ErrPromptTooLarge) {
		t.Errorf("error = %v, want %v", err, ErrPromptTooLarge)
	}
}

func TestReadRequestRejectsEmptyPrompt(t *testing.T) {
	header := EncodeRequestHeader(RequestHeader{PromptSize: 0})
	_, err := ReadRequest(bytes.NewReader(header))
	if !errors.Is(err, ErrEmptyPrompt) {
		t.Errorf("error = %v, want ErrEmptyPrompt", err)
	}
}

func TestReadRequest(t *testing.T) {
	prompt := []byte("\uee02左文脈\uee00てんき\uee01")
	frame := append(EncodeRequestHeader(RequestHeader{
		Generation:  99,
		PromptSize:  uint32(len(prompt)),
		TimeoutMsec: 800,
	}), prompt...)

	request, err := ReadRequest(bytes.NewReader(frame))
	if err != nil {
		t.Fatalf("ReadRequest: %v", err)
	}
	if request.Header.Generation != 99 {
		t.Errorf("generation = %d, want 99", request.Header.Generation)
	}
	if !bytes.Equal(request.Prompt, prompt) {
		t.Errorf("prompt = %q, want %q", request.Prompt, prompt)
	}
}

// The server answers a protocol violation instead of dropping the client, so
// the error has to carry the generation and a short debug token.
func TestReadRequestProtocolErrorCarriesGenerationAndToken(t *testing.T) {
	cases := []struct {
		name       string
		promptSize uint32
		mutate     func([]byte)
		wantErr    error
		wantToken  string
	}{
		{"bad magic", 1, func(b []byte) { b[0] = 0 }, ErrBadMagic, "bad_magic"},
		{"bad version", 1, func(b []byte) { b[4] = 9 }, ErrBadVersion, "bad_version"},
		{"bad kind", 1, func(b []byte) { b[7] = 2 }, ErrBadKind, "bad_kind"},
		{"empty prompt", 0, nil, ErrEmptyPrompt, "empty_prompt"},
		{"prompt too large", MaxPromptBytes + 1, nil, ErrPromptTooLarge, "prompt_too_large"},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			frame := EncodeRequestHeader(RequestHeader{
				Generation: 4242,
				PromptSize: tc.promptSize,
			})
			if tc.mutate != nil {
				tc.mutate(frame)
			}

			_, err := ReadRequest(bytes.NewReader(frame))

			var violation *ProtocolError
			if !errors.As(err, &violation) {
				t.Fatalf("error = %v (%T), want *ProtocolError", err, err)
			}
			if !errors.Is(err, tc.wantErr) {
				t.Errorf("errors.Is(%v, %v) = false", err, tc.wantErr)
			}
			if violation.Generation != 4242 {
				t.Errorf("generation = %d, want 4242 so it can be echoed back",
					violation.Generation)
			}
			if violation.Token() != tc.wantToken {
				t.Errorf("token = %q, want %q", violation.Token(), tc.wantToken)
			}
		})
	}
}

// A vanished client is a transport failure, not a protocol violation: there is
// nobody left to send an error response to.
func TestReadRequestTruncatedHeaderIsNotAProtocolError(t *testing.T) {
	_, err := ReadRequest(bytes.NewReader(make([]byte, 10)))

	var violation *ProtocolError
	if errors.As(err, &violation) {
		t.Errorf("error = %v, want a plain transport error", err)
	}
	if !errors.Is(err, io.ErrUnexpectedEOF) {
		t.Errorf("error = %v, want it to wrap io.ErrUnexpectedEOF", err)
	}
}

func TestWriteResponse(t *testing.T) {
	var buffer bytes.Buffer
	if err := WriteResponse(&buffer, 7, StatusOK, 12, "天気", "jev_ok"); err != nil {
		t.Fatalf("WriteResponse: %v", err)
	}

	got := buffer.Bytes()
	want, err := hex.DecodeString(
		"5a4e5a31" +
			"0100" +
			"0200" +
			"07000000" + // generation 7
			"00000000" + // status OK
			"0c000000" + // latency 12
			"06000000" + // value_size: 6 bytes for 天気
			"06000000") // debug_size: 6 bytes for jev_ok
	if err != nil {
		t.Fatalf("bad test literal: %v", err)
	}
	want = append(want, []byte("天気jev_ok")...)

	if !bytes.Equal(got, want) {
		t.Errorf("frame mismatch\n got %x\nwant %x", got, want)
	}
}

func TestClampedTimeoutMsec(t *testing.T) {
	cases := []struct {
		name      string
		requested uint32
		want      uint32
	}{
		{"zero means default", 0, MaxRequestTimeoutMsec},
		{"below floor", 10, MinRequestTimeoutMsec},
		{"inside range", 900, 900},
		{"above ceiling", 99999, MaxRequestTimeoutMsec},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			got := RequestHeader{TimeoutMsec: tc.requested}.ClampedTimeoutMsec()
			if got != tc.want {
				t.Errorf("got %d, want %d", got, tc.want)
			}
		})
	}
}

func TestOutputCharLimit(t *testing.T) {
	cases := []struct {
		requested uint32
		want      int
	}{
		{0, MaxOutputChars},
		{1, 1},
		{64, 64},
		{9999, MaxOutputChars},
	}
	for _, tc := range cases {
		if got := (RequestHeader{MaxOutputChars: tc.requested}).OutputCharLimit(); got != tc.want {
			t.Errorf("MaxOutputChars %d: got %d, want %d", tc.requested, got, tc.want)
		}
	}
}

// max_output_chars counts characters.  A Japanese surface is three bytes per
// rune, so a byte-based cut would corrupt the UTF-8 handed to Mozkey.
func TestTruncateRunesKeepsUtf8Intact(t *testing.T) {
	got := TruncateRunes("日本語です", 3)
	if got != "日本語" {
		t.Errorf("got %q, want %q", got, "日本語")
	}

	whole := TruncateRunes("日本語です", 99)
	if whole != "日本語です" {
		t.Errorf("got %q, want the full string", whole)
	}

	if TruncateRunes("abc", 0) != "" {
		t.Error("limit 0 must produce an empty string")
	}
}

func isError(err, target error) bool {
	return err != nil && target != nil && strings.Contains(err.Error(), target.Error())
}
