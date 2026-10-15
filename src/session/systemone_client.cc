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

#define WIN32_LEAN_AND_MEAN
// clang-format off
#include <windows.h>
#include <winhttp.h>
// clang-format on

#include "session/systemone_client.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#pragma comment(lib, "Winhttp.lib")

namespace mozc {
namespace session {
namespace {

// Hard caps.  A SystemOne answer is small (one choice plus a probabilities
// map); the response cap mirrors zenz_scorer's kMaxHttpResponseBytes and the
// error-body cap mirrors the Go client's truncateForError.
constexpr size_t kMaxSystemOneResponseBytes = 65536;
constexpr size_t kErrorBodyTruncateBytes = 512;
// A hostile or buggy endpoint could send deeply nested JSON; without a cap,
// SkipValue's recursion would smash the stack (mozc_server crash).  64 is
// far beyond any real SystemOne answer.
constexpr int kMaxJsonDepth = 64;

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

std::string TruncateForError(const std::string& raw) {
  if (raw.size() <= kErrorBodyTruncateBytes) {
    return raw;
  }
  return raw.substr(0, kErrorBodyTruncateBytes) + "...";
}

// ---------------------------------------------------------------------------
// Minimal JSON support.  The project has no JSON dependency, so request
// building is plain string concatenation and response parsing is a small
// member-walking parser: just enough to reach answers.choice.choice /
// answers.choice.confidence while correctly skipping string values (so a
// state string that happens to contain "choice" cannot confuse it).
// ---------------------------------------------------------------------------

bool IsHexDigit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

uint32_t ParseHex4(const std::string& s, size_t pos) {
  uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    const char c = s[pos + i];
    value <<= 4;
    if (c >= '0' && c <= '9') {
      value |= static_cast<uint32_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      value |= static_cast<uint32_t>(c - 'a' + 10);
    } else {
      value |= static_cast<uint32_t>(c - 'A' + 10);
    }
  }
  return value;
}

void AppendUtf8(std::string* out, uint32_t cp) {
  if (cp < 0x80) {
    out->push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

// Decodes one UTF-8 code point.  Returns the byte length (1-4), or 0 when the
// sequence is malformed; the caller then advances one byte and treats the
// rune as "other" rather than failing the whole string.
int DecodeUtf8(const std::string& s, size_t pos, uint32_t* cp) {
  const unsigned char b0 = static_cast<unsigned char>(s[pos]);
  if (b0 < 0x80) {
    *cp = b0;
    return 1;
  }
  if ((b0 & 0xE0) == 0xC0) {
    if (pos + 1 >= s.size()) {
      return 0;
    }
    const unsigned char b1 = static_cast<unsigned char>(s[pos + 1]);
    if ((b1 & 0xC0) != 0x80) {
      return 0;
    }
    *cp = (static_cast<uint32_t>(b0 & 0x1F) << 6) | (b1 & 0x3F);
    return 2;
  }
  if ((b0 & 0xF0) == 0xE0) {
    if (pos + 2 >= s.size()) {
      return 0;
    }
    const unsigned char b1 = static_cast<unsigned char>(s[pos + 1]);
    const unsigned char b2 = static_cast<unsigned char>(s[pos + 2]);
    if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) {
      return 0;
    }
    *cp = (static_cast<uint32_t>(b0 & 0x0F) << 12) |
          (static_cast<uint32_t>(b1 & 0x3F) << 6) | (b2 & 0x3F);
    return 3;
  }
  if ((b0 & 0xF8) == 0xF0) {
    if (pos + 3 >= s.size()) {
      return 0;
    }
    const unsigned char b1 = static_cast<unsigned char>(s[pos + 1]);
    const unsigned char b2 = static_cast<unsigned char>(s[pos + 2]);
    const unsigned char b3 = static_cast<unsigned char>(s[pos + 3]);
    if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80) {
      return 0;
    }
    *cp = (static_cast<uint32_t>(b0 & 0x07) << 18) |
          (static_cast<uint32_t>(b1 & 0x3F) << 12) |
          (static_cast<uint32_t>(b2 & 0x3F) << 6) | (b3 & 0x3F);
    return 4;
  }
  return 0;
}

// Labels a surface by script so the criteria descriptions carry signal.
// Mirrors scriptHint in the Go reference client; priority order matches.
std::string ScriptHint(const std::string& surface) {
  bool has_kanji = false;
  bool has_kana = false;  // hiragana
  bool has_katakana = false;
  size_t i = 0;
  while (i < surface.size()) {
    uint32_t cp = 0;
    const int len = DecodeUtf8(surface, i, &cp);
    if (len <= 0) {
      i += 1;
      continue;
    }
    i += static_cast<size_t>(len);
    if ((cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
        (cp >= 0xF900 && cp <= 0xFAFF)) {
      has_kanji = true;
    } else if (cp >= 0x3040 && cp <= 0x309F) {
      has_kana = true;
    } else if ((cp >= 0x30A0 && cp <= 0x30FF) ||
               (cp >= 0xFF66 && cp <= 0xFF9F)) {
      has_katakana = true;
    }
  }
  if (has_kanji) {
    return "漢字表記";
  }
  if (has_katakana) {
    return "カタカナ表記";
  }
  if (has_kana) {
    return "ひらがな表記";
  }
  return "その他の表記";
}

std::string JsonEscape(const std::string& input) {
  std::string out;
  out.reserve(input.size() + 8);
  for (const char c : input) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(
              buf, sizeof(buf), "\\u%04x",
              static_cast<unsigned int>(static_cast<unsigned char>(c)));
          out += buf;
        } else {
          out.push_back(c);
        }
    }
  }
  return out;
}

bool SkipWhitespace(const std::string& s, size_t* pos) {
  while (*pos < s.size()) {
    const char c = s[*pos];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      ++*pos;
    } else {
      break;
    }
  }
  return true;
}

// *pos must point at the opening quote.
bool SkipString(const std::string& s, size_t* pos) {
  if (*pos >= s.size() || s[*pos] != '"') {
    return false;
  }
  ++*pos;
  while (*pos < s.size()) {
    const char c = s[*pos];
    if (c == '"') {
      ++*pos;
      return true;
    }
    if (c == '\\') {
      ++*pos;
      if (*pos >= s.size()) {
        return false;
      }
      if (s[*pos] == 'u') {
        for (int i = 0; i < 4; ++i) {
          ++*pos;
          if (*pos >= s.size() || !IsHexDigit(s[*pos])) {
            return false;
          }
        }
      }
      ++*pos;
    } else {
      ++*pos;
    }
  }
  return false;  // unterminated string
}

bool SkipLiteral(const std::string& s, size_t* pos) {
  static const char* const kLiterals[] = {"true", "false", "null"};
  for (const char* literal : kLiterals) {
    const size_t len = std::strlen(literal);
    if (s.compare(*pos, len, literal) == 0) {
      *pos += len;
      return true;
    }
  }
  return false;
}

bool SkipNumber(const std::string& s, size_t* pos) {
  const size_t start = *pos;
  if (*pos < s.size() && (s[*pos] == '-' || s[*pos] == '+')) {
    ++*pos;
  }
  bool any_digit = false;
  while (*pos < s.size()) {
    const char c = s[*pos];
    if (c >= '0' && c <= '9') {
      any_digit = true;
      ++*pos;
    } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
      ++*pos;
    } else {
      break;
    }
  }
  return any_digit && *pos > start;
}

bool SkipValue(const std::string& s, size_t* pos, int depth);

bool SkipContainer(const std::string& s, size_t* pos, char close, int depth) {
  if (depth > kMaxJsonDepth) {
    return false;
  }
  ++*pos;  // consume the opening brace/bracket
  if (!SkipWhitespace(s, pos)) {
    return false;
  }
  if (*pos < s.size() && s[*pos] == close) {
    ++*pos;
    return true;
  }
  while (true) {
    if (!SkipWhitespace(s, pos)) {
      return false;
    }
    if (close == '}') {
      if (*pos >= s.size() || s[*pos] != '"') {
        return false;
      }
      if (!SkipString(s, pos)) {
        return false;
      }
      if (!SkipWhitespace(s, pos)) {
        return false;
      }
      if (*pos >= s.size() || s[*pos] != ':') {
        return false;
      }
      ++*pos;
    }
    if (!SkipWhitespace(s, pos)) {
      return false;
    }
    if (!SkipValue(s, pos, depth + 1)) {
      return false;
    }
    if (!SkipWhitespace(s, pos)) {
      return false;
    }
    if (*pos >= s.size()) {
      return false;
    }
    if (s[*pos] == ',') {
      ++*pos;
      continue;
    }
    if (s[*pos] == close) {
      ++*pos;
      return true;
    }
    return false;
  }
}

bool SkipValue(const std::string& s, size_t* pos, int depth) {
  if (depth > kMaxJsonDepth) {
    return false;
  }
  if (!SkipWhitespace(s, pos)) {
    return false;
  }
  if (*pos >= s.size()) {
    return false;
  }
  const char c = s[*pos];
  if (c == '"') {
    return SkipString(s, pos);
  }
  if (c == '{') {
    return SkipContainer(s, pos, '}', depth);
  }
  if (c == '[') {
    return SkipContainer(s, pos, ']', depth);
  }
  if (c == 't' || c == 'f' || c == 'n') {
    return SkipLiteral(s, pos);
  }
  return SkipNumber(s, pos);
}

// Walks the members of the object starting at obj_begin.  fn returns false to
// stop early.  Keys are compared raw (unescaped), which is sufficient for the
// ASCII keys this client looks up.
template <typename Fn>
bool ForEachMember(const std::string& s, size_t obj_begin, Fn fn) {
  if (obj_begin >= s.size() || s[obj_begin] != '{') {
    return false;
  }
  size_t pos = obj_begin + 1;
  if (!SkipWhitespace(s, &pos)) {
    return false;
  }
  if (pos < s.size() && s[pos] == '}') {
    return true;  // empty object
  }
  while (true) {
    if (pos >= s.size() || s[pos] != '"') {
      return false;
    }
    const size_t key_begin = pos;
    if (!SkipString(s, &pos)) {
      return false;
    }
    const std::string key = s.substr(key_begin + 1, pos - key_begin - 2);
    if (!SkipWhitespace(s, &pos)) {
      return false;
    }
    if (pos >= s.size() || s[pos] != ':') {
      return false;
    }
    ++pos;
    if (!SkipWhitespace(s, &pos)) {
      return false;
    }
    const size_t value_begin = pos;
    if (!SkipValue(s, &pos, 0)) {
      return false;
    }
    if (!fn(key, value_begin, pos)) {
      return true;  // early stop is not an error
    }
    if (!SkipWhitespace(s, &pos)) {
      return false;
    }
    if (pos >= s.size()) {
      return false;
    }
    if (s[pos] == ',') {
      ++pos;
      continue;
    }
    if (s[pos] == '}') {
      return true;
    }
    return false;
  }
}

// Locates `key` in the object at obj_begin and returns the value's
// span.  Returns true only when the key was actually found; a clean walk
// that never matches must not leave the caller treating the root object as
// the value (that was the answers-missing bug: a "model"-only response
// would have been misread as the answers object).
bool FindMemberValue(const std::string& s, size_t obj_begin,
                     const std::string& key, size_t* value_begin,
                     size_t* value_end) {
  bool found = false;
  ForEachMember(s, obj_begin, [&](const std::string& k, size_t vb, size_t ve) {
    if (k == key) {
      *value_begin = vb;
      *value_end = ve;
      found = true;
      return false;  // stop walking
    }
    return true;
  });
  return found;
}

// Extracts the decoded string value of a span that must start and end with a
// double quote.
bool ExtractStringValue(const std::string& s, size_t value_begin,
                        size_t value_end, std::string* out) {
  if (value_begin + 1 >= value_end || value_end > s.size() ||
      s[value_begin] != '"' || s[value_end - 1] != '"') {
    return false;
  }
  std::string result;
  size_t pos = value_begin + 1;
  const size_t end = value_end - 1;
  while (pos < end) {
    const char c = s[pos];
    if (c != '\\') {
      result.push_back(c);
      ++pos;
      continue;
    }
    if (pos + 1 >= end) {
      return false;
    }
    const char esc = s[pos + 1];
    pos += 2;
    switch (esc) {
      case '"':
        result.push_back('"');
        break;
      case '\\':
        result.push_back('\\');
        break;
      case '/':
        result.push_back('/');
        break;
      case 'b':
        result.push_back('\b');
        break;
      case 'f':
        result.push_back('\f');
        break;
      case 'n':
        result.push_back('\n');
        break;
      case 'r':
        result.push_back('\r');
        break;
      case 't':
        result.push_back('\t');
        break;
      case 'u': {
        if (pos + 4 > end) {
          return false;
        }
        uint32_t cp = ParseHex4(s, pos);
        pos += 4;
        if (cp >= 0xD800 && cp <= 0xDBFF && pos + 6 <= end && s[pos] == '\\' &&
            s[pos + 1] == 'u') {
          const uint32_t low = ParseHex4(s, pos + 2);
          if (low >= 0xDC00 && low <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            pos += 6;
          }
        }
        AppendUtf8(&result, cp);
        break;
      }
      default:
        return false;
    }
  }
  *out = std::move(result);
  return true;
}

bool ExtractNumberValue(const std::string& s, size_t value_begin,
                        size_t value_end, double* out) {
  if (value_begin >= value_end || value_end > s.size()) {
    return false;
  }
  const std::string num = s.substr(value_begin, value_end - value_begin);
  char* end = nullptr;
  errno = 0;
  const double value = std::strtod(num.c_str(), &end);
  if (end != num.c_str() + num.size()) {
    return false;
  }
  *out = value;
  return true;
}

// A CR/LF in a header value would let a caller smuggle extra headers
// into the request (header injection).  The API key and session id are the
// only caller-controlled values that reach the header block.
bool ContainsCrLf(const std::string& value) {
  return value.find('\r') != std::string::npos ||
         value.find('\n') != std::string::npos;
}

// Closes HINTERNET handles on every path, including early returns.
class WinHttpHandle {
 public:
  WinHttpHandle() = default;
  explicit WinHttpHandle(HINTERNET handle) : handle_(handle) {}

  WinHttpHandle(const WinHttpHandle&) = delete;
  WinHttpHandle& operator=(const WinHttpHandle&) = delete;

  WinHttpHandle(WinHttpHandle&& other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
  }
  WinHttpHandle& operator=(WinHttpHandle&& other) noexcept {
    if (this != &other) {
      Reset();
      handle_ = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }

  ~WinHttpHandle() { Reset(); }

  HINTERNET get() const { return handle_; }
  explicit operator bool() const { return handle_ != nullptr; }

 private:
  void Reset() {
    if (handle_ != nullptr) {
      WinHttpCloseHandle(handle_);
      handle_ = nullptr;
    }
  }
  HINTERNET handle_ = nullptr;
};

std::string WinHttpErrorTag(const char* label) {
  return std::string(label) + "_" + std::to_string(::GetLastError());
}

}  // namespace

std::optional<ParsedEndpoint> SystemOneClient::ParseEndpointUrl(
    const std::string& url) {
  ParsedEndpoint endpoint;
  if (url.empty()) {
    endpoint.error_message = "endpoint url is empty";
    return endpoint;
  }

  const std::wstring wide = Utf8ToWide(url);
  // WinHttpCrackUrl writes host/path into caller-owned buffers sized by the
  // matching dw*Length fields and updates those lengths to the value sizes;
  // a length-only query (zero lengths, null pointers) leaves the component
  // pointers null on some SDK builds, so always pass real buffers.
  constexpr DWORD kMaxComponentChars = 2084;
  std::vector<wchar_t> host(kMaxComponentChars);
  std::vector<wchar_t> path(kMaxComponentChars);
  URL_COMPONENTS components{};
  components.dwStructSize = sizeof(components);
  components.lpszHostName = host.data();
  components.dwHostNameLength = static_cast<DWORD>(host.size());
  components.lpszUrlPath = path.data();
  components.dwUrlPathLength = static_cast<DWORD>(path.size());
  if (!WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0,
                       &components)) {
    endpoint.error_message = WinHttpErrorTag("url_parse_failed");
    return endpoint;
  }

  if (components.nScheme != INTERNET_SCHEME_HTTP &&
      components.nScheme != INTERNET_SCHEME_HTTPS) {
    endpoint.error_message =
        "unsupported_url_scheme_" + std::to_string(components.nScheme);
    return endpoint;
  }

  endpoint.secure = (components.nScheme == INTERNET_SCHEME_HTTPS);
  endpoint.host.assign(host.data(), components.dwHostNameLength);
  endpoint.port = static_cast<uint16_t>(components.nPort);
  endpoint.path.assign(path.data(), components.dwUrlPathLength);
  endpoint.ok = true;
  return endpoint;
}

std::string SystemOneClient::BuildRequestJson(const SystemOneRequest& request) {
  std::string out = "{";
  out += "\"model\":\"";
  out += JsonEscape(request.model_name);
  out += "\",\"state\":\"";
  out += JsonEscape(request.state);
  out +=
      "\",\"questions\":{\"choice\":{\"type\":\"choice\",\"instructions\":\"";
  out += JsonEscape(kSystemOneChoiceInstructions);
  out += "\",\"criteria\":{";
  for (size_t i = 0; i < request.options.size(); ++i) {
    if (i > 0) {
      out += ",";
    }
    out += "\"";
    out += JsonEscape(request.options[i]);
    out += "\":\"";
    out += JsonEscape(ScriptHint(request.options[i]));
    out += "\"";
  }
  out += "}}}}";
  return out;
}

bool SystemOneClient::BuildHeaders(const SystemOneRequest& request,
                                   std::wstring* headers) {
  if (ContainsCrLf(request.api_key) || ContainsCrLf(request.session_id)) {
    return false;
  }

  std::wstring out = L"Content-Type: application/json; charset=utf-8\r\n";
  // The free model answers keyless; only send the header when a key is
  // configured so an unset key cannot turn into "Bearer ".
  if (!request.api_key.empty()) {
    out += L"Authorization: Bearer ";
    out += Utf8ToWide(request.api_key);
    out += L"\r\n";
  }
  out += L"User-Agent: opencode/1.18.31\r\n";
  if (!request.session_id.empty()) {
    out += L"x-session-id: ";
    out += Utf8ToWide(request.session_id);
    out += L"\r\n";
    out += L"x-session-affinity: ";
    out += Utf8ToWide(request.session_id);
    out += L"\r\n";
    out += L"X-Opencode-Session: ";
    out += Utf8ToWide(request.session_id);
    out += L"\r\n";
  }
  *headers = std::move(out);
  return true;
}

bool SystemOneClient::ParseResponseJson(const std::string& body,
                                        const std::vector<std::string>& options,
                                        std::string* choice,
                                        double* confidence) {
  size_t root = 0;
  if (!SkipWhitespace(body, &root) || root >= body.size() ||
      body[root] != '{') {
    return false;
  }

  size_t answers_begin = 0;
  size_t answers_end = 0;
  if (!FindMemberValue(body, root, "answers", &answers_begin, &answers_end)) {
    return false;
  }

  // Mirror the Go client: prefer answers["choice"], but fall back to the
  // first choice-shaped answer when the id was renamed on the gateway.
  bool have_primary = false;
  size_t primary_begin = 0;
  size_t primary_end = 0;
  bool have_fallback = false;
  size_t fallback_begin = 0;
  size_t fallback_end = 0;
  ForEachMember(
      body, answers_begin, [&](const std::string& key, size_t vb, size_t ve) {
        if (vb >= body.size() || body[vb] != '{') {
          return true;
        }
        if (key == "choice" && !have_primary) {
          have_primary = true;
          primary_begin = vb;
          primary_end = ve;
        }
        if (!have_fallback) {
          size_t type_begin = 0;
          size_t type_end = 0;
          if (FindMemberValue(body, vb, "type", &type_begin, &type_end)) {
            std::string type;
            if (ExtractStringValue(body, type_begin, type_end, &type) &&
                type == "choice") {
              size_t cb = 0;
              size_t ce = 0;
              std::string candidate;
              if (FindMemberValue(body, vb, "choice", &cb, &ce) &&
                  ExtractStringValue(body, cb, ce, &candidate) &&
                  !candidate.empty()) {
                have_fallback = true;
                fallback_begin = vb;
                fallback_end = ve;
              }
            }
          }
        }
        return true;
      });

  bool have_answer = false;
  size_t answer_begin = 0;
  size_t answer_end = 0;
  if (have_primary) {
    bool type_ok = true;
    size_t type_begin = 0;
    size_t type_end = 0;
    if (FindMemberValue(body, primary_begin, "type", &type_begin, &type_end)) {
      std::string type;
      if (ExtractStringValue(body, type_begin, type_end, &type) &&
          !type.empty() && type != "choice") {
        type_ok = false;
      }
    }
    if (type_ok) {
      have_answer = true;
      answer_begin = primary_begin;
      answer_end = primary_end;
    }
  }
  if (!have_answer && have_fallback) {
    have_answer = true;
    answer_begin = fallback_begin;
    answer_end = fallback_end;
  }
  if (!have_answer) {
    return false;
  }

  size_t choice_begin = 0;
  size_t choice_end = 0;
  if (!FindMemberValue(body, answer_begin, "choice", &choice_begin,
                       &choice_end)) {
    return false;
  }
  std::string chosen;
  if (!ExtractStringValue(body, choice_begin, choice_end, &chosen) ||
      chosen.empty()) {
    return false;
  }

  double conf = 0.0;
  size_t conf_begin = 0;
  size_t conf_end = 0;
  if (FindMemberValue(body, answer_begin, "confidence", &conf_begin,
                      &conf_end)) {
    ExtractNumberValue(body, conf_begin, conf_end, &conf);
  }

  // The chosen surface must be one that was offered; forwarding an unvetted
  // string into the IME would be worse than reporting an error.
  for (const std::string& option : options) {
    if (option == chosen) {
      if (choice != nullptr) {
        *choice = std::move(chosen);
      }
      if (confidence != nullptr) {
        *confidence = conf;
      }
      return true;
    }
  }
  return false;
}

SystemOneClient::SystemOneClient(ExchangeFn exchange)
    : exchange_(std::move(exchange)) {}

SystemOneResponse SystemOneClient::Ask(const SystemOneRequest& request) {
  SystemOneResponse response;

  if (request.options.empty()) {
    response.error_message = "no options to choose from";
    return response;
  }
  if (request.endpoint_url.empty()) {
    response.error_message = "endpoint url is empty";
    return response;
  }

  std::optional<ParsedEndpoint> endpoint =
      ParseEndpointUrl(request.endpoint_url);
  if (!endpoint.has_value() || !endpoint->ok) {
    response.error_message =
        endpoint.has_value() ? endpoint->error_message : "url parse failed";
    return response;
  }

  const std::string body = BuildRequestJson(request);
  // Reject CR/LF in header-bound values before anything is sent.
  if (ContainsCrLf(request.api_key)) {
    response.error_message = "api_key contains CR/LF";
    return response;
  }
  if (ContainsCrLf(request.session_id)) {
    response.error_message = "session_id contains CR/LF";
    return response;
  }
  std::wstring headers;
  if (!BuildHeaders(request, &headers)) {
    response.error_message = "header build rejected api_key or session_id";
    return response;
  }

  HttpResult http;
  if (exchange_) {
    http = exchange_(*endpoint, headers, body, request.timeout_msec);
  } else {
    http = WinHttpExchange(*endpoint, headers, body, request.timeout_msec);
  }

  response.http_status = http.http_status;
  response.latency_msec = http.latency_msec;

  if (!http.transport_ok) {
    response.error_message = http.transport_error.empty()
                                 ? "transport failed"
                                 : http.transport_error;
    return response;
  }
  if (http.http_status < 200 || http.http_status > 299) {
    response.error_message = "http " + std::to_string(http.http_status) + ": " +
                             TruncateForError(http.body);
    return response;
  }

  std::string choice;
  double confidence = 0.0;
  if (!ParseResponseJson(http.body, request.options, &choice, &confidence)) {
    response.error_message = "response carried no usable choice";
    return response;
  }

  response.ok = true;
  response.choice = std::move(choice);
  response.confidence = confidence;
  return response;
}

HttpResult WinHttpExchange(const ParsedEndpoint& endpoint,
                           const std::wstring& headers, const std::string& body,
                           uint32_t timeout_msec) {
  HttpResult result;
  const auto started = std::chrono::steady_clock::now();
  // Explicit stamping, no NRVO assumption: every return path stamps latency
  // before returning.  A destructor-based guard would write into a
  // moved-from result whenever the return value is move-constructed.
  const auto stamp_latency = [&result, started]() {
    result.latency_msec = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started)
            .count());
  };

  // WINHTTP_ACCESS_TYPE_NO_PROXY: the client talks to the endpoint directly.
  // No system proxy is consulted, matching the user requirement.
  WinHttpHandle session(
      WinHttpOpen(L"opencode/1.18.31", WINHTTP_ACCESS_TYPE_NO_PROXY,
                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
  if (!session) {
    result.transport_error = WinHttpErrorTag("winhttp_open_failed");
    stamp_latency();
    return result;
  }

  const int timeout = static_cast<int>(std::max<uint32_t>(timeout_msec, 50));
  WinHttpSetTimeouts(session.get(), timeout, timeout, timeout, timeout);

  const INTERNET_PORT port =
      endpoint.port != 0 ? static_cast<INTERNET_PORT>(endpoint.port)
                         : (endpoint.secure ? INTERNET_DEFAULT_HTTPS_PORT
                                            : INTERNET_DEFAULT_HTTP_PORT);
  WinHttpHandle connect(
      WinHttpConnect(session.get(), endpoint.host.c_str(), port, 0));
  if (!connect) {
    result.transport_error = WinHttpErrorTag("winhttp_connect_failed");
    stamp_latency();
    return result;
  }

  const std::wstring path = endpoint.path.empty() ? L"/" : endpoint.path;
  WinHttpHandle request(WinHttpOpenRequest(
      connect.get(), L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, endpoint.secure ? WINHTTP_FLAG_SECURE : 0));
  if (!request) {
    result.transport_error = WinHttpErrorTag("winhttp_open_request_failed");
    stamp_latency();
    return result;
  }

  // WinHttpSendRequest takes a non-const LPVOID but only reads from it;
  // the cast keeps the body immutable.  MSVC accepts the plain pointer, so
  // this matches src/zenz_scorer/main.cc behavior under strict compilers.
  if (!WinHttpSendRequest(
          request.get(), headers.c_str(), static_cast<DWORD>(-1L),
          const_cast<void*>(static_cast<const void*>(body.data())),
          static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()),
          0)) {
    result.transport_error = WinHttpErrorTag("winhttp_send_request_failed");
    stamp_latency();
    return result;
  }

  if (!WinHttpReceiveResponse(request.get(), nullptr)) {
    result.transport_error = WinHttpErrorTag("winhttp_receive_response_failed");
    stamp_latency();
    return result;
  }

  DWORD status_code = 0;
  DWORD status_size = sizeof(status_code);
  if (WinHttpQueryHeaders(request.get(),
                          WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &status_code,
                          &status_size, WINHTTP_NO_HEADER_INDEX)) {
    result.http_status = static_cast<int>(status_code);
  }

  std::string response_body;
  for (;;) {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request.get(), &available)) {
      result.transport_error =
          WinHttpErrorTag("winhttp_query_data_available_failed");
      stamp_latency();
      return result;
    }
    if (available == 0) {
      break;
    }
    std::vector<char> buffer(available);
    DWORD read = 0;
    if (!WinHttpReadData(request.get(), buffer.data(), available, &read) ||
        read == 0) {
      result.transport_error = WinHttpErrorTag("winhttp_read_data_failed");
      stamp_latency();
      return result;
    }
    response_body.append(buffer.data(), read);
    if (response_body.size() > kMaxSystemOneResponseBytes) {
      response_body.resize(kMaxSystemOneResponseBytes);
      result.transport_error = "response_too_large";
      stamp_latency();
      return result;
    }
  }

  stamp_latency();
  result.transport_ok = true;
  result.body = std::move(response_body);
  return result;
}

}  // namespace session
}  // namespace mozc
