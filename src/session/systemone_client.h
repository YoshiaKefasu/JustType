// Copyright 2026 Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer in
// the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#ifndef MOZC_SESSION_SYSTEMONE_CLIENT_H_
#define MOZC_SESSION_SYSTEMONE_CLIENT_H_

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace mozc {
namespace session {

// Default settings for the OpenCode Zen gateway that hosts Jev.  These mirror
// the values in config.proto (systemone_*) so a caller that passes an
// untouched Config gets the same behavior.
inline constexpr char kSystemOneDefaultEndpointUrl[] =
    "https://opencode.ai/zen/v1/systemone";
inline constexpr char kSystemOneDefaultModelName[] = "jev-1.13-free";
inline constexpr char kSystemOneDefaultSessionId[] =
    "ses_019a00000000AbCdEfGhIjKlMn";
inline constexpr uint32_t kSystemOneDefaultTimeoutMsec = 2500;

// Instruction text the endpoint documents for the choice question.  Kept in
// sync with tools/mozkey-jev-bridge/internal/bridge.DefaultInstructions so
// the native client and the Go bridge ask the model the same question.
inline constexpr char kSystemOneChoiceInstructions[] =
    "文脈に最も合致する正しい日本語表記を選んでください";

// One decision request.  SystemOne (Jev / Laya) does not generate text: it
// reads `state` and answers which of `options` best fits it, so this replaces
// the generative Zenz pipeline rather than extending it.
struct SystemOneRequest {
  // Full URL including scheme, host, and path, e.g.
  // "https://opencode.ai/zen/v1/systemone" or
  // "http://127.0.0.1:8888/v1/systemone".
  std::string endpoint_url = kSystemOneDefaultEndpointUrl;

  // Sent as "Authorization: Bearer <key>" only when non-empty; the free
  // model answers keyless.
  std::string api_key;

  std::string model_name = kSystemOneDefaultModelName;

  // Sent as x-session-id / x-session-affinity / X-Opencode-Session so the
  // gateway can pin follow-up requests to the same backend.  Only sent when
  // non-empty.
  std::string session_id = kSystemOneDefaultSessionId;

  // Per-request budget in milliseconds, applied to resolve/connect/send/
  // receive alike.  A request that misses this deadline is abandoned and the
  // already-computed Mozc result is kept, so this is a latency ceiling rather
  // than a retry window.
  uint32_t timeout_msec = kSystemOneDefaultTimeoutMsec;

  // The context string the model judges, e.g. left context and reading.
  std::string state;

  // Candidate surfaces, ordered best first.  The chosen surface is echoed
  // back and must be one of these; anything else is rejected.
  std::vector<std::string> options;
};

// Result of one exchange.  ok is false whenever the caller must keep the
// already-computed Mozc result: validation failure, transport failure,
// non-2xx status, malformed response, or a choice that was not offered.
struct SystemOneResponse {
  bool ok = false;

  // The chosen surface, valid only when ok is true.
  std::string choice;

  // Endpoint-reported confidence in [0, 1]; 0.0 when the field is absent.
  double confidence = 0.0;

  // Round-trip time measured by the transport layer.
  uint32_t latency_msec = 0;

  // Human-readable failure reason, empty when ok is true.
  std::string error_message;

  // HTTP status code, 0 when the request never completed.
  int http_status = 0;
};

// A URL split into the pieces WinHTTP needs.  Produced by
// SystemOneClient::ParseEndpointUrl.  This header intentionally stays free of
// windows.h: `port` is a plain uint16_t so the struct can be built and
// tested without pulling in the SDK.
struct ParsedEndpoint {
  bool secure = false;  // true for https
  std::wstring host;    // UTF-16, as WinHTTP wants it
  uint16_t port = 0;    // 0 means the URL carried no explicit port
  std::wstring path;    // "/v1/systemone"; empty means "/"
  bool ok = false;
  std::string error_message;
};

// Raw transport result.  The client converts this into a SystemOneResponse
// after status and JSON checks; the split keeps the network layer mockable.
struct HttpResult {
  int http_status = 0;          // 0 when the request never completed
  std::string body;             // response body, size-capped by the transport
  bool transport_ok = false;    // false on any WinHTTP-level failure
  std::string transport_error;  // e.g. "winhttp_connect_failed_12029"
  uint32_t latency_msec = 0;
};

// Native SystemOne client.  Talks to Jev (OpenCode Zen) or a local Laya
// directly over WinHTTP, with no external proxy in between.
//
// The transport is injectable: the default is WinHttpExchange, and tests pass
// their own ExchangeFn so URL parsing, request building, and response
// handling can be exercised deterministically.  The zero value is usable.
class SystemOneClient {
 public:
  // Sends the JSON body with the given header block to the endpoint and
  // returns the raw status/body.  Must never throw and must always fill
  // latency_msec.
  using ExchangeFn = std::function<HttpResult(
      const ParsedEndpoint& endpoint, const std::wstring& headers,
      const std::string& body, uint32_t timeout_msec)>;

  SystemOneClient() = default;
  explicit SystemOneClient(ExchangeFn exchange);

  // Splits an http(s) URL.  Returns nullopt (with error_message filled) for
  // malformed URLs and non-http(s) schemes.
  static std::optional<ParsedEndpoint> ParseEndpointUrl(const std::string& url);

  // Builds the request JSON: one criteria entry per option, valued with a
  // script hint (漢字表記 / カタカナ表記 / ひらがな表記 / その他の表記) so
  // the endpoint's criteria descriptions carry signal instead of echoing the
  // surface.  Matches the Go reference client in
  // tools/mozkey-jev-bridge/internal/jev/client.go.
  static std::string BuildRequestJson(const SystemOneRequest& request);

  // Builds the "\r\n"-separated header block.  Authorization is present
  // only when api_key is non-empty; the three session headers only when
  // session_id is non-empty.  Returns false when api_key or session_id
  // contains CR/LF, which would otherwise allow header injection.
  static bool BuildHeaders(const SystemOneRequest& request,
                           std::wstring* headers);

  // Parses the answer JSON into choice/confidence.  Returns false when the
  // body is malformed, carries no choice, or chose a surface that was not
  // offered — the caller must never forward an unvetted string into the IME.
  // A renamed question id falls back to the first choice-shaped answer, like
  // the Go client.
  static bool ParseResponseJson(const std::string& body,
                                const std::vector<std::string>& options,
                                std::string* choice, double* confidence);

  // Runs one exchange end to end: validate, parse URL, build request, send,
  // parse answer.  Never throws; every failure lands in error_message.
  SystemOneResponse Ask(const SystemOneRequest& request);

 private:
  // Empty means "use the default WinHTTP transport".
  ExchangeFn exchange_;
};

// The default WinHTTP transport.  Public so the default ExchangeFn can be
// constructed from it and so callers/tests can invoke it directly.
HttpResult WinHttpExchange(const ParsedEndpoint& endpoint,
                           const std::wstring& headers, const std::string& body,
                           uint32_t timeout_msec);

}  // namespace session
}  // namespace mozc

#endif  // MOZC_SESSION_SYSTEMONE_CLIENT_H_
