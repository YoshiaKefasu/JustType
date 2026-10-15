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

// Unit tests for the SystemOne (Jev / Laya) WinHTTP client and logger.
// The HTTP transport is injected, so every code path runs deterministically;
// the optional live check only runs when SYSTEMONE_LIVE_ENDPOINT is set.
//
// Mirrors the Go reference tests in
// tools/mozkey-jev-bridge/internal/jev/client_test.go.

#include "session/systemone_client.h"

#include <algorithm>
#include <cstdlib>
#include <string>

#include "session/systemone_logger.h"
#include "testing/gunit.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace mozc {
namespace session {
namespace {

#if defined(_WIN32)

// Same fixture shape as the Go reference client, including the usage block
// that must be ignored.
constexpr char kChoiceResponse[] =
    R"({"model":"jev-1.13-free","answers":{"choice":{"type":"choice",)"
    R"("choice":"天気","confidence":0.78,"probabilities":{"天気":0.81,)"
    R"("転記":0,"てんき":0.19}}},"usage":{"input_tokens":462,"output_tokens":77}})";

std::wstring Utf8ToWide(const std::string& input) {
  if (input.empty()) {
    return L"";
  }
  const int size = MultiByteToWideChar(
      CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0);
  if (size <= 0) {
    return L"";
  }
  std::wstring output(size, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()),
                      output.data(), size);
  return output;
}

std::string WideToUtf8(const std::wstring& input) {
  if (input.empty()) {
    return "";
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, input.data(),
                                       static_cast<int>(input.size()), nullptr,
                                       0, nullptr, nullptr);
  if (size <= 0) {
    return "";
  }
  std::string output(size, '\0');
  WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()),
                      output.data(), size, nullptr, nullptr);
  return output;
}

bool ContainsWide(const std::wstring& haystack, const std::wstring& needle) {
  return haystack.find(needle) != std::wstring::npos;
}

bool ContainsUtf8(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

// Records what the client handed to the transport and returns a canned
// result.  Mirrors the httptest server in the Go tests.
class MockExchange {
 public:
  HttpResult result;
  std::wstring captured_headers;
  std::string captured_body;
  ParsedEndpoint captured_endpoint;
  uint32_t captured_timeout = 0;
  int call_count = 0;

  SystemOneClient::ExchangeFn Fn() {
    return [this](const ParsedEndpoint& endpoint, const std::wstring& headers,
                  const std::string& body, uint32_t timeout_msec) {
      ++call_count;
      captured_endpoint = endpoint;
      captured_headers = headers;
      captured_body = body;
      captured_timeout = timeout_msec;
      return result;
    };
  }
};

HttpResult OkResult(const std::string& body) {
  HttpResult result;
  result.transport_ok = true;
  result.http_status = 200;
  result.body = body;
  result.latency_msec = 42;
  return result;
}

// ---------------------------------------------------------------------------
// URL parsing
// ---------------------------------------------------------------------------

TEST(SystemOneParseEndpointUrlTest, HttpsDefault) {
  const auto endpoint =
      SystemOneClient::ParseEndpointUrl("https://opencode.ai/zen/v1/systemone");
  ASSERT_TRUE(endpoint.has_value());
  EXPECT_TRUE(endpoint->ok);
  EXPECT_TRUE(endpoint->secure);
  EXPECT_EQ(endpoint->host, L"opencode.ai");
  EXPECT_EQ(endpoint->port, 443);
  EXPECT_EQ(endpoint->path, L"/zen/v1/systemone");
  EXPECT_TRUE(endpoint->error_message.empty());
}

TEST(SystemOneParseEndpointUrlTest, HttpWithExplicitPort) {
  const auto endpoint =
      SystemOneClient::ParseEndpointUrl("http://127.0.0.1:8888/v1/systemone");
  ASSERT_TRUE(endpoint.has_value());
  EXPECT_TRUE(endpoint->ok);
  EXPECT_FALSE(endpoint->secure);
  EXPECT_EQ(endpoint->host, L"127.0.0.1");
  EXPECT_EQ(endpoint->port, 8888);
  EXPECT_EQ(endpoint->path, L"/v1/systemone");
}

TEST(SystemOneParseEndpointUrlTest, HttpDefaultPort) {
  const auto endpoint =
      SystemOneClient::ParseEndpointUrl("http://127.0.0.1/v1/systemone");
  ASSERT_TRUE(endpoint.has_value());
  EXPECT_TRUE(endpoint->ok);
  EXPECT_EQ(endpoint->port, 80);
}

TEST(SystemOneParseEndpointUrlTest, RejectsNonHttpScheme) {
  const auto endpoint = SystemOneClient::ParseEndpointUrl("ftp://host/v1");
  ASSERT_TRUE(endpoint.has_value());
  EXPECT_FALSE(endpoint->ok);
  EXPECT_EQ(endpoint->error_message, "unsupported_url_scheme_3");
}

TEST(SystemOneParseEndpointUrlTest, RejectsMalformedUrl) {
  const auto endpoint = SystemOneClient::ParseEndpointUrl("http://");
  ASSERT_TRUE(endpoint.has_value());
  EXPECT_FALSE(endpoint->ok);
  EXPECT_FALSE(endpoint->error_message.empty());
}

TEST(SystemOneParseEndpointUrlTest, RejectsEmptyUrl) {
  const auto endpoint = SystemOneClient::ParseEndpointUrl("");
  ASSERT_TRUE(endpoint.has_value());
  EXPECT_FALSE(endpoint->ok);
  EXPECT_EQ(endpoint->error_message, "endpoint url is empty");
}

// ---------------------------------------------------------------------------
// Request JSON
// ---------------------------------------------------------------------------

TEST(SystemOneBuildRequestJsonTest, MatchesEndpointShape) {
  SystemOneRequest request;
  request.model_name = "jev-1.13-free";
  request.state = "左文脈: きょう\n読み: てんき";
  request.options = {"天気", "てんき", "テンキ", "ABC"};

  EXPECT_EQ(
      SystemOneClient::BuildRequestJson(request),
      R"({"model":"jev-1.13-free","state":"左文脈: きょう\n読み: てんき",)"
      R"("questions":{"choice":{"type":"choice","instructions":)"
      R"("文脈に最も合致する正しい日本語表記を選んでください",)"
      R"("criteria":{"天気":"漢字表記","てんき":"ひらがな表記",)"
      R"("テンキ":"カタカナ表記","ABC":"その他の表記"}}}})");
}

TEST(SystemOneBuildRequestJsonTest, EscapesJsonSpecialCharacters) {
  SystemOneRequest request;
  request.state = "a\"b\\c\nd\te";
  request.options = {"x"};
  const std::string json = SystemOneClient::BuildRequestJson(request);
  EXPECT_TRUE(ContainsUtf8(json, "a\\\"b\\\\c\\nd\\te"));
}

TEST(SystemOneBuildRequestJsonTest, MixedScriptUsesKanjiFirst) {
  SystemOneRequest request;
  request.options = {"天気てんき"};  // kanji + hiragana mix
  EXPECT_TRUE(ContainsUtf8(SystemOneClient::BuildRequestJson(request),
                           "天気てんき\":\"漢字表記"));
}

// ---------------------------------------------------------------------------
// Headers
// ---------------------------------------------------------------------------

TEST(SystemOneBuildHeadersTest, OmitsAuthorizationWhenKeyIsEmpty) {
  SystemOneRequest request;
  request.api_key = "";
  request.session_id = "ses_test";
  std::wstring headers;
  ASSERT_TRUE(SystemOneClient::BuildHeaders(request, &headers));
  EXPECT_FALSE(ContainsWide(headers, L"Authorization"));
  EXPECT_TRUE(ContainsWide(headers,
                           L"Content-Type: application/json; "
                           L"charset=utf-8\r\n"));
  EXPECT_TRUE(ContainsWide(headers, L"User-Agent: opencode/1.18.31\r\n"));
  EXPECT_TRUE(ContainsWide(headers, L"x-session-id: ses_test\r\n"));
  EXPECT_TRUE(ContainsWide(headers, L"x-session-affinity: ses_test\r\n"));
  EXPECT_TRUE(ContainsWide(headers, L"X-Opencode-Session: ses_test\r\n"));
}

TEST(SystemOneBuildHeadersTest, SendsConfiguredHeaders) {
  SystemOneRequest request;
  request.api_key = "sk-test";
  request.session_id = "ses_test";
  std::wstring headers;
  ASSERT_TRUE(SystemOneClient::BuildHeaders(request, &headers));
  EXPECT_TRUE(ContainsWide(headers, L"Authorization: Bearer sk-test\r\n"));
  EXPECT_TRUE(ContainsWide(headers, L"User-Agent: opencode/1.18.31\r\n"));
  EXPECT_TRUE(ContainsWide(headers, L"x-session-id: ses_test\r\n"));
  EXPECT_TRUE(ContainsWide(headers, L"x-session-affinity: ses_test\r\n"));
  EXPECT_TRUE(ContainsWide(headers, L"X-Opencode-Session: ses_test\r\n"));
}

TEST(SystemOneBuildHeadersTest, OmitsSessionHeadersWhenIdIsEmpty) {
  SystemOneRequest request;
  request.session_id = "";
  std::wstring headers;
  ASSERT_TRUE(SystemOneClient::BuildHeaders(request, &headers));
  EXPECT_FALSE(ContainsWide(headers, L"x-session-id"));
  EXPECT_FALSE(ContainsWide(headers, L"x-session-affinity"));
  EXPECT_FALSE(ContainsWide(headers, L"X-Opencode-Session"));
}

TEST(SystemOneBuildHeadersTest, RejectsCrLfInApiKey) {
  SystemOneRequest request;
  request.api_key = "sk-test\r\nX-Injected: 1";
  std::wstring headers;
  EXPECT_FALSE(SystemOneClient::BuildHeaders(request, &headers));
}

TEST(SystemOneBuildHeadersTest, RejectsCrLfInSessionId) {
  SystemOneRequest request;
  request.session_id = "ses_test\r\nX-Injected: 1";
  std::wstring headers;
  EXPECT_FALSE(SystemOneClient::BuildHeaders(request, &headers));
}

// ---------------------------------------------------------------------------
// Response JSON
// ---------------------------------------------------------------------------

TEST(SystemOneParseResponseJsonTest, ParsesChoice) {
  std::string choice;
  double confidence = -1.0;
  ASSERT_TRUE(SystemOneClient::ParseResponseJson(
      kChoiceResponse, {"天気", "転記", "てんき"}, &choice, &confidence));
  EXPECT_EQ(choice, "天気");
  EXPECT_DOUBLE_EQ(confidence, 0.78);
}

TEST(SystemOneParseResponseJsonTest, ConfidenceIsOptional) {
  const std::string body = R"({"answers":{"choice":{"choice":"天気"}}})";
  std::string choice;
  double confidence = -1.0;
  ASSERT_TRUE(
      SystemOneClient::ParseResponseJson(body, {"天気"}, &choice, &confidence));
  EXPECT_EQ(choice, "天気");
  EXPECT_DOUBLE_EQ(confidence, 0.0);  // untouched fallback
}

TEST(SystemOneParseResponseJsonTest, FallsBackToRenamedQuestionId) {
  const std::string body =
      R"({"answers":{"renamed":{"type":"choice","choice":"天気",)"
      R"("confidence":0.5}}})";
  std::string choice;
  double confidence = -1.0;
  ASSERT_TRUE(SystemOneClient::ParseResponseJson(body, {"天気", "転記"},
                                                 &choice, &confidence));
  EXPECT_EQ(choice, "天気");
  EXPECT_DOUBLE_EQ(confidence, 0.5);
}

TEST(SystemOneParseResponseJsonTest, RejectsUnofferedChoice) {
  const std::string body =
      R"({"answers":{"choice":{"type":"choice","choice":"発明",)"
      R"("confidence":0.9}}})";
  std::string choice;
  double confidence = 0.0;
  EXPECT_FALSE(SystemOneClient::ParseResponseJson(body, {"天気", "転記"},
                                                  &choice, &confidence));
}

TEST(SystemOneParseResponseJsonTest, RejectsEmptyChoice) {
  const std::string body =
      R"({"answers":{"choice":{"type":"choice","choice":"", "confidence":0}}})";
  std::string choice;
  double confidence = 0.0;
  EXPECT_FALSE(
      SystemOneClient::ParseResponseJson(body, {"天気"}, &choice, &confidence));
}

TEST(SystemOneParseResponseJsonTest, RejectsNonChoicePrimaryType) {
  // The gateway may answer with a different question shape under "choice";
  // that must fall through to the first choice-shaped answer.
  const std::string body =
      R"({"answers":{"choice":{"type":"probability","choice":"天気"},)"
      R"("other":{"type":"choice","choice":"転記"}}})";
  std::string choice;
  double confidence = 0.0;
  ASSERT_TRUE(SystemOneClient::ParseResponseJson(body, {"天気", "転記"},
                                                 &choice, &confidence));
  EXPECT_EQ(choice, "転記");
}

TEST(SystemOneParseResponseJsonTest, RejectsMalformedJson) {
  std::string choice;
  double confidence = 0.0;
  EXPECT_FALSE(SystemOneClient::ParseResponseJson("{\"answers\":", {"天気"},
                                                  &choice, &confidence));
  EXPECT_FALSE(
      SystemOneClient::ParseResponseJson("", {"天気"}, &choice, &confidence));
  EXPECT_FALSE(SystemOneClient::ParseResponseJson("not json at all", {"天気"},
                                                  &choice, &confidence));
}

TEST(SystemOneParseResponseJsonTest, RejectsDeeplyNestedJson) {
  // A hostile endpoint could send deeply nested JSON; without the depth cap
  // the recursive skip would smash the stack and crash the server.
  std::string deep = "{\"answers\":{\"x\":";
  for (int i = 0; i < 100; ++i) {
    deep += "{\"n\":";
  }
  deep += "\"leaf\"";
  for (int i = 0; i < 100; ++i) {
    deep += "}";
  }
  deep += "}}";
  std::string choice;
  double confidence = 0.0;
  EXPECT_FALSE(
      SystemOneClient::ParseResponseJson(deep, {"天気"}, &choice, &confidence));
}

TEST(SystemOneParseResponseJsonTest, RejectsMissingAnswers) {
  std::string choice;
  double confidence = 0.0;
  EXPECT_FALSE(SystemOneClient::ParseResponseJson(R"({"model":"x"})", {"天気"},
                                                  &choice, &confidence));
}

TEST(SystemOneParseResponseJsonTest, IgnoresChoiceLookalikeInsideState) {
  // A "choice" key inside a string value must not confuse the parser.
  const std::string body =
      R"({"state":"the word choice appears in the answer text",)"
      R"("answers":{"choice":{"type":"choice","choice":"天気",)"
      R"("confidence":0.9}}})";
  std::string choice;
  double confidence = 0.0;
  ASSERT_TRUE(
      SystemOneClient::ParseResponseJson(body, {"天気"}, &choice, &confidence));
  EXPECT_EQ(choice, "天気");
  EXPECT_DOUBLE_EQ(confidence, 0.9);
}

// ---------------------------------------------------------------------------
// Ask with an injected transport
// ---------------------------------------------------------------------------

SystemOneRequest MakeAskRequest() {
  SystemOneRequest request;
  request.state = "左文脈: きょう\n読み: てんき";
  request.options = {"天気", "転記", "てんき"};
  return request;
}

TEST(SystemOneAskTest, SucceedsWithMockExchange) {
  MockExchange mock;
  mock.result = OkResult(kChoiceResponse);
  SystemOneClient client(mock.Fn());
  const SystemOneResponse response = client.Ask(MakeAskRequest());

  EXPECT_TRUE(response.ok);
  EXPECT_EQ(response.choice, "天気");
  EXPECT_DOUBLE_EQ(response.confidence, 0.78);
  EXPECT_EQ(response.http_status, 200);
  EXPECT_EQ(response.latency_msec, 42);
  EXPECT_TRUE(response.error_message.empty());
  EXPECT_EQ(mock.call_count, 1);
}

TEST(SystemOneAskTest, RejectsCrLfInApiKeyWithoutSending) {
  MockExchange mock;
  mock.result = OkResult(kChoiceResponse);
  SystemOneClient client(mock.Fn());

  SystemOneRequest request = MakeAskRequest();
  request.api_key = "sk-test\r\nX-Injected: 1";
  const SystemOneResponse response = client.Ask(request);

  EXPECT_FALSE(response.ok);
  EXPECT_EQ(response.error_message, "api_key contains CR/LF");
  EXPECT_EQ(mock.call_count, 0);
}

TEST(SystemOneAskTest, RejectsCrLfInSessionIdWithoutSending) {
  MockExchange mock;
  mock.result = OkResult(kChoiceResponse);
  SystemOneClient client(mock.Fn());

  SystemOneRequest request = MakeAskRequest();
  request.session_id = "ses_test\r\nX-Injected: 1";
  const SystemOneResponse response = client.Ask(request);

  EXPECT_FALSE(response.ok);
  EXPECT_EQ(response.error_message, "session_id contains CR/LF");
  EXPECT_EQ(mock.call_count, 0);
}

TEST(SystemOneAskTest, HttpErrorIsReported) {
  MockExchange mock;
  mock.result.transport_ok = true;
  mock.result.http_status = 429;
  mock.result.body = R"({"error":"rate limited"})";
  SystemOneClient client(mock.Fn());
  const SystemOneResponse response = client.Ask(MakeAskRequest());

  EXPECT_FALSE(response.ok);
  EXPECT_EQ(response.http_status, 429);
  EXPECT_TRUE(ContainsUtf8(response.error_message,
                           "http 429: {\"error\":\"rate limited\"}"));
}

TEST(SystemOneAskTest, TransportFailureIsReported) {
  MockExchange mock;
  mock.result.transport_ok = false;
  mock.result.transport_error = "winhttp_receive_response_failed_12002";
  SystemOneClient client(mock.Fn());
  const SystemOneResponse response = client.Ask(MakeAskRequest());

  EXPECT_FALSE(response.ok);
  EXPECT_EQ(response.http_status, 0);
  EXPECT_EQ(response.error_message, "winhttp_receive_response_failed_12002");
}

TEST(SystemOneAskTest, RejectsEmptyOptionsWithoutSending) {
  MockExchange mock;
  mock.result = OkResult(kChoiceResponse);
  SystemOneClient client(mock.Fn());

  SystemOneRequest request;
  request.state = "s";
  const SystemOneResponse response = client.Ask(request);

  EXPECT_FALSE(response.ok);
  EXPECT_EQ(response.error_message, "no options to choose from");
  EXPECT_EQ(mock.call_count, 0);
}

TEST(SystemOneAskTest, RejectsEmptyEndpointUrlWithoutSending) {
  MockExchange mock;
  mock.result = OkResult(kChoiceResponse);
  SystemOneClient client(mock.Fn());

  SystemOneRequest request;
  request.endpoint_url = "";
  request.options = {"天気"};
  const SystemOneResponse response = client.Ask(request);

  EXPECT_FALSE(response.ok);
  EXPECT_EQ(response.error_message, "endpoint url is empty");
  EXPECT_EQ(mock.call_count, 0);
}

TEST(SystemOneAskTest, RejectsUnsupportedSchemeWithoutSending) {
  MockExchange mock;
  mock.result = OkResult(kChoiceResponse);
  SystemOneClient client(mock.Fn());

  SystemOneRequest request;
  request.endpoint_url = "ftp://example.test/v1";
  request.options = {"天気"};
  const SystemOneResponse response = client.Ask(request);

  EXPECT_FALSE(response.ok);
  EXPECT_TRUE(ContainsUtf8(response.error_message, "unsupported_url_scheme"));
  EXPECT_EQ(mock.call_count, 0);
}

TEST(SystemOneAskTest, PassesCorrectArgumentsToExchange) {
  MockExchange mock;
  mock.result = OkResult(kChoiceResponse);
  SystemOneClient client(mock.Fn());

  SystemOneRequest request;
  request.api_key = "sk-test";
  request.session_id = "ses_test";
  request.timeout_msec = 1234;
  request.state = "state text";
  request.options = {"天気"};
  ASSERT_TRUE(client.Ask(request).ok);

  ASSERT_EQ(mock.call_count, 1);
  EXPECT_EQ(mock.captured_timeout, 1234);
  EXPECT_TRUE(mock.captured_endpoint.secure);
  EXPECT_EQ(mock.captured_endpoint.host, L"opencode.ai");
  EXPECT_EQ(mock.captured_endpoint.port, 443);
  EXPECT_EQ(mock.captured_endpoint.path, L"/zen/v1/systemone");
  EXPECT_TRUE(ContainsWide(mock.captured_headers,
                           L"Authorization: Bearer sk-test\r\n"));
  EXPECT_TRUE(ContainsUtf8(mock.captured_body, "\"model\":\"jev-1.13-free\""));
  EXPECT_TRUE(ContainsUtf8(mock.captured_body, "\"state\":\"state text\""));
  EXPECT_TRUE(ContainsUtf8(mock.captured_body, "\"criteria\":{\"天気\""));
}

TEST(SystemOneAskTest, MalformedResponseIsReported) {
  MockExchange mock;
  mock.result = OkResult("{\"answers\":");
  SystemOneClient client(mock.Fn());
  const SystemOneResponse response = client.Ask(MakeAskRequest());

  EXPECT_FALSE(response.ok);
  EXPECT_EQ(response.error_message, "response carried no usable choice");
}

TEST(SystemOneAskTest, LocalLayaEndpointIsAccepted) {
  MockExchange mock;
  mock.result = OkResult(kChoiceResponse);
  SystemOneClient client(mock.Fn());

  SystemOneRequest request;
  request.endpoint_url = "http://127.0.0.1:8888/v1/systemone";
  request.options = {"天気"};
  ASSERT_TRUE(client.Ask(request).ok);

  EXPECT_EQ(mock.call_count, 1);
  EXPECT_FALSE(mock.captured_endpoint.secure);
  EXPECT_EQ(mock.captured_endpoint.host, L"127.0.0.1");
  EXPECT_EQ(mock.captured_endpoint.port, 8888);
  EXPECT_EQ(mock.captured_endpoint.path, L"/v1/systemone");
}

// ---------------------------------------------------------------------------
// Logger
// ---------------------------------------------------------------------------

std::string MakeTempLogPath() {
  wchar_t dir[MAX_PATH] = {};
  ::GetTempPathW(MAX_PATH, dir);
  const std::string name = "systemone_client_test_" +
                           std::to_string(::GetCurrentProcessId()) + "_" +
                           std::to_string(::GetTickCount64()) + ".log";
  return WideToUtf8(std::wstring(dir)) + name;
}

// Reads the UTF-16LE (BOM-prefixed) log file back as UTF-8 for assertions.
std::string ReadLogAsUtf8(const std::string& path) {
  HANDLE file = ::CreateFileW(Utf8ToWide(path).c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return "";
  }
  std::string bytes;
  char buffer[4096] = {};
  DWORD read = 0;
  while (::ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
    bytes.append(buffer, read);
  }
  ::CloseHandle(file);
  if (bytes.size() < 2 || static_cast<unsigned char>(bytes[0]) != 0xFF ||
      static_cast<unsigned char>(bytes[1]) != 0xFE) {
    return "<not utf-16le with BOM>";  // fails the assertions below
  }
  const std::wstring wide(reinterpret_cast<const wchar_t*>(bytes.data() + 2),
                          (bytes.size() - 2) / sizeof(wchar_t));
  return WideToUtf8(wide);
}

SystemOneRequest MakeLoggerRequest() {
  SystemOneRequest request;
  request.endpoint_url = "https://opencode.ai/zen/v1/systemone";
  request.model_name = "jev-1.13-free";
  request.state = "きょうの天気";
  request.options = {"天気", "転記", "てんき"};
  request.api_key = "sk-log-secret";  // Must never appear in the log.
  return request;
}

TEST(SystemOneLoggerTest, LogsSuccessLine) {
  const std::string path = MakeTempLogPath();
  SystemOneLogger logger(true);
  logger.SetLogFilePath(path);

  SystemOneResponse response;
  response.ok = true;
  response.choice = "天気";
  response.confidence = 0.95;
  response.latency_msec = 512;
  logger.LogExchange(MakeLoggerRequest(), response);

  const std::string content = ReadLogAsUtf8(path);
  EXPECT_TRUE(ContainsUtf8(content, "[SystemOne] 20"));
  EXPECT_TRUE(ContainsUtf8(content,

                           "[URL: https://opencode.ai/zen/v1/systemone]"));
  EXPECT_TRUE(ContainsUtf8(content, "[model: jev-1.13-free]"));
  EXPECT_TRUE(ContainsUtf8(content, "[reading: きょうの天気] [options: 3]"));
  EXPECT_TRUE(
      ContainsUtf8(content, "-> choice=\"天気\" (conf=0.95, latency=512ms)"));
  EXPECT_FALSE(ContainsUtf8(content, "sk-log-secret"));

  ::DeleteFileW(Utf8ToWide(path).c_str());
}

TEST(SystemOneLoggerTest, LogsHttpErrorLine) {
  const std::string path = MakeTempLogPath();
  SystemOneLogger logger(true);
  logger.SetLogFilePath(path);

  SystemOneResponse response;
  response.ok = false;
  response.http_status = 401;
  response.error_message = "http 401: unauthorized";
  logger.LogExchange(MakeLoggerRequest(), response);

  const std::string content = ReadLogAsUtf8(path);
  EXPECT_TRUE(ContainsUtf8(content, "[SystemOne ERROR] 20"));
  EXPECT_TRUE(ContainsUtf8(content, "[HTTP 401]"));
  EXPECT_TRUE(ContainsUtf8(content,
                           "[HTTP 401] message=\"http 401: "
                           "unauthorized\""));
  ::DeleteFileW(Utf8ToWide(path).c_str());
}

TEST(SystemOneLoggerTest, LogsTransportErrorLine) {
  const std::string path = MakeTempLogPath();
  SystemOneLogger logger(true);
  logger.SetLogFilePath(path);

  SystemOneResponse response;
  response.ok = false;
  response.error_message = "winhttp_receive_response_failed_12002";
  logger.LogExchange(MakeLoggerRequest(), response);

  const std::string content = ReadLogAsUtf8(path);
  EXPECT_TRUE(ContainsUtf8(content, "[winhttp_receive_response_failed_12002]"));
  EXPECT_TRUE(ContainsUtf8(content,
                           "message=\"winhttp_receive_response_"
                           "failed_12002\""));
  ::DeleteFileW(Utf8ToWide(path).c_str());
}

TEST(SystemOneLoggerTest, TruncatesLongStateAndKeepsOneLine) {
  const std::string path = MakeTempLogPath();
  SystemOneLogger logger(true);
  logger.SetLogFilePath(path);

  std::string long_state;
  for (int i = 0; i < 200; ++i) {
    long_state += "あ";
  }
  SystemOneRequest request = MakeLoggerRequest();
  request.state = long_state;  // 600 bytes, far beyond the truncation limit
  SystemOneResponse response;
  response.ok = true;
  response.choice = "天気";
  logger.LogExchange(request, response);

  const std::string content = ReadLogAsUtf8(path);
  EXPECT_TRUE(ContainsUtf8(content, "...(省略)"));
  // Exactly two lines: one BOM-only start plus the single log line.
  size_t newlines = 0;
  for (const char c : content) {
    if (c == '\n') {
      ++newlines;
    }
  }
  EXPECT_EQ(newlines, 1);

  ::DeleteFileW(Utf8ToWide(path).c_str());
}

TEST(SystemOneLoggerTest, DisabledLoggerWritesNothing) {
  const std::string path = MakeTempLogPath();
  SystemOneLogger logger(false);
  logger.SetLogFilePath(path);

  SystemOneResponse response;
  response.ok = true;
  response.choice = "天気";
  logger.LogExchange(MakeLoggerRequest(), response);

  EXPECT_EQ(::GetFileAttributesW(Utf8ToWide(path).c_str()),
            INVALID_FILE_ATTRIBUTES);
}

TEST(SystemOneLoggerTest, AppendsMultipleLines) {
  const std::string path = MakeTempLogPath();
  SystemOneLogger logger(true);
  logger.SetLogFilePath(path);

  SystemOneResponse ok;
  ok.ok = true;
  ok.choice = "天気";
  SystemOneResponse err;
  err.ok = false;
  err.http_status = 500;
  err.error_message = "http 500: boom";
  logger.LogExchange(MakeLoggerRequest(), ok);
  logger.LogExchange(MakeLoggerRequest(), err);

  const std::string content = ReadLogAsUtf8(path);
  size_t newlines = 0;
  for (const char c : content) {
    if (c == '\n') {
      ++newlines;
    }
  }
  EXPECT_EQ(newlines, 2);
  ::DeleteFileW(Utf8ToWide(path).c_str());
}

// ---------------------------------------------------------------------------
// Live check (opt-in).  Without SYSTEMONE_LIVE_ENDPOINT the suite stays
// hermetic; with it, a running Laya/Jev endpoint answers for real.
// ---------------------------------------------------------------------------

TEST(SystemOneLiveTest, TalksToConfiguredEndpointWhenAvailable) {
  const char* raw = std::getenv("SYSTEMONE_LIVE_ENDPOINT");
  if (raw == nullptr || *raw == '\0') {
    GTEST_SKIP() << "Set SYSTEMONE_LIVE_ENDPOINT "
                    "(e.g. http://127.0.0.1:8888/v1/systemone) to run it.";
  }

  SystemOneClient client;
  SystemOneRequest request;
  request.endpoint_url = raw;
  request.state = "読み: てんき";
  request.options = {"天気", "転記", "てんき"};
  request.timeout_msec = 5000;
  const SystemOneResponse response = client.Ask(request);
  if (!response.ok) {
    GTEST_SKIP() << "Live endpoint did not answer: " << response.error_message;
  }
  EXPECT_LE(response.latency_msec, request.timeout_msec);
  // The live endpoint is a decision model: with state "読み: てんき" it may
  // legitimately answer てんき rather than 天気.  The point of this check is
  // that the WinHTTP round trip works and returns an offered surface, not
  // which surface the model picks.
  const bool choice_is_offered =
      std::find(request.options.begin(), request.options.end(),
                response.choice) != request.options.end();
  EXPECT_TRUE(choice_is_offered);
}

#endif  // _WIN32

#if !defined(_WIN32)
TEST(SystemOneClientTest, SkippedOutsideWindows) {
  GTEST_SKIP() << "WinHTTP transport only";
}
#endif

}  // namespace
}  // namespace session
}  // namespace mozc
