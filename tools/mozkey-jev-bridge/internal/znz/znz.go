// Package znz implements the ZNZ1 named pipe wire protocol shared by the
// Mozkey client and mozkey-jev-bridge.
//
// The layout mirrors src/zenz/zenz_wire_protocol.h and must stay byte
// compatible with it.  Both headers are packed and little-endian; the static
// asserts in the C++ header are the contract, so the Go encoders below are
// written field by field rather than through unsafe struct casting.
package znz

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
)

// Wire constants.  Keep in sync with src/zenz/zenz_wire_protocol.h.
const (
	Magic   uint32 = 0x315A4E5A // "ZNZ1"
	Version uint16 = 1

	KindRequest  uint16 = 1
	KindResponse uint16 = 2

	StatusOK      uint32 = 0
	StatusError   uint32 = 1
	StatusTimeout uint32 = 2
)

// Sizes and limits.  The size limits mirror src/session/zenz_named_pipe_client.cc
// so the bridge never asks the client to read more than it is willing to.
const (
	RequestHeaderSize  = 24
	ResponseHeaderSize = 28

	MaxPromptBytes = 8192
	MaxValueBytes  = 1024 * 1024
	MaxDebugBytes  = 64 * 1024

	// Mozkey clamps the model timeout it sends into this range; the bridge
	// clamps the same way so a hostile or stale value cannot widen the budget.
	MinRequestTimeoutMsec = 50
	MaxRequestTimeoutMsec = 5000

	MinOutputChars = 1
	MaxOutputChars = 256
)

// Protocol violations reported by ReadRequest.
//
// ReadRequest wraps these in a ProtocolError so the server can echo the
// generation back in an error response, the way HandleClient in
// src/zenz_scorer/main.cc does.
var (
	ErrShortHeader    = errors.New("short_header")
	ErrBadMagic       = errors.New("bad_magic")
	ErrBadVersion     = errors.New("bad_version")
	ErrBadKind        = errors.New("bad_kind")
	ErrEmptyPrompt    = errors.New("empty_prompt")
	ErrPromptTooLarge = errors.New("prompt_too_large")
)

// ProtocolError is a client protocol violation that carries enough context to
// answer the client instead of dropping it.
//
// Generation is whatever the header decoded to, which for a bad magic or
// version is not meaningful; mozc_zenz_scorer echoes it anyway, and a wrong
// generation is harmless because the client discards a frame it cannot match.
type ProtocolError struct {
	// Err is one of the sentinel errors above.
	Err error
	// Generation is the generation field decoded from the request header.
	Generation uint32
}

func (e *ProtocolError) Error() string { return "znz: " + e.Err.Error() }

// Unwrap lets errors.Is match the sentinel errors.
func (e *ProtocolError) Unwrap() error { return e.Err }

// Token is the short string placed in the response debug field.
func (e *ProtocolError) Token() string { return e.Err.Error() }

// RequestHeader is the 24 byte request preamble.
type RequestHeader struct {
	Generation     uint32
	TimeoutMsec    uint32
	MaxOutputChars uint32
	PromptSize     uint32
}

// ResponseHeader is the 28 byte response preamble.
type ResponseHeader struct {
	Generation  uint32
	Status      uint32
	LatencyMsec uint32
	ValueSize   uint32
	DebugSize   uint32
}

// Request is a validated request header plus its prompt bytes.
type Request struct {
	Header RequestHeader
	Prompt []byte
}

// EncodeRequestHeader renders the header in wire order.
func EncodeRequestHeader(h RequestHeader) []byte {
	buf := make([]byte, RequestHeaderSize)
	binary.LittleEndian.PutUint32(buf[0:4], Magic)
	binary.LittleEndian.PutUint16(buf[4:6], Version)
	binary.LittleEndian.PutUint16(buf[6:8], KindRequest)
	binary.LittleEndian.PutUint32(buf[8:12], h.Generation)
	binary.LittleEndian.PutUint32(buf[12:16], h.TimeoutMsec)
	binary.LittleEndian.PutUint32(buf[16:20], h.MaxOutputChars)
	binary.LittleEndian.PutUint32(buf[20:24], h.PromptSize)
	return buf
}

// EncodeResponseHeader renders the header in wire order.
func EncodeResponseHeader(h ResponseHeader) []byte {
	buf := make([]byte, ResponseHeaderSize)
	binary.LittleEndian.PutUint32(buf[0:4], Magic)
	binary.LittleEndian.PutUint16(buf[4:6], Version)
	binary.LittleEndian.PutUint16(buf[6:8], KindResponse)
	binary.LittleEndian.PutUint32(buf[8:12], h.Generation)
	binary.LittleEndian.PutUint32(buf[12:16], h.Status)
	binary.LittleEndian.PutUint32(buf[16:20], h.LatencyMsec)
	binary.LittleEndian.PutUint32(buf[20:24], h.ValueSize)
	binary.LittleEndian.PutUint32(buf[24:28], h.DebugSize)
	return buf
}

// DecodeRequestHeader parses and validates a request header.
//
// The decoded header is returned even on a validation failure, because
// ReadRequest needs its generation to answer a protocol violation the way
// HandleClient does.
func DecodeRequestHeader(buf []byte) (RequestHeader, error) {
	if len(buf) < RequestHeaderSize {
		return RequestHeader{}, ErrShortHeader
	}

	header := RequestHeader{
		Generation:     binary.LittleEndian.Uint32(buf[8:12]),
		TimeoutMsec:    binary.LittleEndian.Uint32(buf[12:16]),
		MaxOutputChars: binary.LittleEndian.Uint32(buf[16:20]),
		PromptSize:     binary.LittleEndian.Uint32(buf[20:24]),
	}

	switch {
	case binary.LittleEndian.Uint32(buf[0:4]) != Magic:
		return header, ErrBadMagic
	case binary.LittleEndian.Uint16(buf[4:6]) != Version:
		return header, ErrBadVersion
	case binary.LittleEndian.Uint16(buf[6:8]) != KindRequest:
		return header, ErrBadKind
	}
	return header, nil
}

// ReadRequest reads one request from r.
//
// Protocol violations come back as *ProtocolError, which carries the decoded
// generation so the caller can answer with status=Error.  Transport failures
// come back as wrapped io errors, which mean the client is gone and there is
// nothing to answer.
func ReadRequest(r io.Reader) (Request, error) {
	headerBuf := make([]byte, RequestHeaderSize)
	if _, err := io.ReadFull(r, headerBuf); err != nil {
		return Request{}, fmt.Errorf("znz: read header: %w", err)
	}

	header, err := DecodeRequestHeader(headerBuf)
	if err != nil {
		return Request{Header: header}, &ProtocolError{Err: err, Generation: header.Generation}
	}
	if header.PromptSize == 0 {
		return Request{Header: header}, &ProtocolError{Err: ErrEmptyPrompt, Generation: header.Generation}
	}
	if header.PromptSize > MaxPromptBytes {
		return Request{Header: header}, &ProtocolError{Err: ErrPromptTooLarge, Generation: header.Generation}
	}

	prompt := make([]byte, header.PromptSize)
	if _, err := io.ReadFull(r, prompt); err != nil {
		return Request{}, fmt.Errorf("znz: read prompt: %w", err)
	}
	return Request{Header: header, Prompt: prompt}, nil
}

// WriteResponse writes a complete response frame: header, then value, then
// debug.  An error is returned if any part of the frame cannot be written, so
// the caller can distinguish a broken connection from a handled request.
func WriteResponse(w io.Writer, generation, status, latencyMsec uint32,
	value, debug string) error {
	if len(value) > MaxValueBytes {
		value = value[:MaxValueBytes]
	}
	if len(debug) > MaxDebugBytes {
		debug = debug[:MaxDebugBytes]
	}

	frame := make([]byte, 0, ResponseHeaderSize+len(value)+len(debug))
	frame = append(frame, EncodeResponseHeader(ResponseHeader{
		Generation:  generation,
		Status:      status,
		LatencyMsec: latencyMsec,
		ValueSize:   uint32(len(value)),
		DebugSize:   uint32(len(debug)),
	})...)
	frame = append(frame, value...)
	frame = append(frame, debug...)

	if _, err := w.Write(frame); err != nil {
		return fmt.Errorf("znz: write response: %w", err)
	}
	return nil
}

// ClampedTimeoutMsec applies the protocol clamp to the requested model budget.
func (h RequestHeader) ClampedTimeoutMsec() uint32 {
	requested := h.TimeoutMsec
	if requested == 0 {
		return MaxRequestTimeoutMsec
	}
	if requested < MinRequestTimeoutMsec {
		return MinRequestTimeoutMsec
	}
	if requested > MaxRequestTimeoutMsec {
		return MaxRequestTimeoutMsec
	}
	return requested
}

// OutputCharLimit applies the protocol clamp to the requested output size.
func (h RequestHeader) OutputCharLimit() int {
	requested := h.MaxOutputChars
	if requested == 0 {
		return MaxOutputChars
	}
	if requested < MinOutputChars {
		return MinOutputChars
	}
	if requested > MaxOutputChars {
		return MaxOutputChars
	}
	return int(requested)
}

// TruncateRunes cuts s to at most limit runes.
//
// max_output_chars counts characters, and a Japanese conversion result is
// mostly multi-byte runes, so cutting on bytes would corrupt the surface and
// hand Mozkey invalid UTF-8.
func TruncateRunes(s string, limit int) string {
	if limit <= 0 {
		return ""
	}
	count := 0
	for i := range s {
		if count == limit {
			return s[:i]
		}
		count++
	}
	return s
}
