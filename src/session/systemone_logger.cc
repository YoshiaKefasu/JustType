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
#include "session/systemone_logger.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

namespace mozc {
namespace session {
namespace {

// Keeps a long state string from turning one log line into a wall of text.
// The tail marker tells the reader the middle was dropped on purpose.
constexpr size_t kReadingTruncateBytes = 512;

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

std::string FormatTimestamp() {
  SYSTEMTIME local{};
  ::GetLocalTime(&local);
  char buffer[32] = {};
  std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d",
                local.wYear, local.wMonth, local.wDay, local.wHour,
                local.wMinute, local.wSecond);
  return buffer;
}

std::string DefaultLogFilePath() {
  wchar_t buffer[MAX_PATH] = {};
  const DWORD len =
      ::GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
  if (len == 0 || len >= MAX_PATH) {
    return "";  // No LOCALAPPDATA: debug sink only.
  }
  std::wstring path(buffer, len);
  path += L"\\Mozc\\systemone.log";
  return WideToUtf8(path);
}

// One line must stay one line, so embedded newlines become spaces.
std::string SanitizeForLog(const std::string& value) {
  std::string out = value;
  std::replace(out.begin(), out.end(), '\r', ' ');
  std::replace(out.begin(), out.end(), '\n', ' ');
  return out;
}

std::string TruncateReading(const std::string& state) {
  if (state.size() <= kReadingTruncateBytes) {
    return SanitizeForLog(state);
  }
  // Cut on a UTF-8 boundary so the reader never sees a broken rune.
  size_t cut = kReadingTruncateBytes;
  while (cut > 0 && (static_cast<unsigned char>(state[cut]) & 0xC0) == 0x80) {
    --cut;
  }
  return SanitizeForLog(state.substr(0, cut)) + "...(省略)";
}

std::string FormatConfidence(double confidence) {
  char buffer[32] = {};
  std::snprintf(buffer, sizeof(buffer), "%.2f", confidence);
  return buffer;
}

std::wstring ParentDirectory(const std::wstring& path) {
  const size_t slash = path.find_last_of(L'\\');
  if (slash == std::wstring::npos || slash == 0) {
    return L"";
  }
  return path.substr(0, slash);
}

}  // namespace

SystemOneLogger::SystemOneLogger(bool enabled)
    : enabled_(enabled), log_file_path_(DefaultLogFilePath()) {}

void SystemOneLogger::SetLogFilePath(std::string path) {
  std::lock_guard<std::mutex> lock(mutex_);
  log_file_path_ = std::move(path);
}

void SystemOneLogger::LogExchange(const SystemOneRequest& request,
                                  const SystemOneResponse& response) {
  if (!enabled_) {
    return;
  }

  std::string line;
  if (response.ok) {
    line = "[SystemOne] " + FormatTimestamp() +
           " [URL: " + request.endpoint_url + "]" +
           " [model: " + request.model_name + "]" +
           " [reading: " + TruncateReading(request.state) + "]" +
           " [options: " + std::to_string(request.options.size()) + "]" +
           " -> choice=\"" + SanitizeForLog(response.choice) + "\"" +
           " (conf=" + FormatConfidence(response.confidence) +
           ", latency=" + std::to_string(response.latency_msec) + "ms)";
  } else {
    // The HTTP status and the WinHTTP error are mutually exclusive in
    // practice: a status means the response arrived, a WinHTTP error means it
    // did not.  Protocol failures carry no code, so the message leads.
    std::string cause;
    if (response.http_status != 0) {
      cause = "HTTP " + std::to_string(response.http_status);
    } else if (!response.error_message.empty()) {
      cause = SanitizeForLog(response.error_message);
    } else {
      cause = "unknown";
    }
    line = "[SystemOne ERROR] " + FormatTimestamp() +
           " [URL: " + request.endpoint_url + "]" +
           " [model: " + request.model_name + "]" +
           " [reading: " + TruncateReading(request.state) + "]" +
           " [options: " + std::to_string(request.options.size()) + "]" + " [" +
           cause + "] message=\"" + SanitizeForLog(response.error_message) +
           "\"";
  }

  std::lock_guard<std::mutex> lock(mutex_);

  // Debug sink first: it survives even when the file path is unwritable.
  std::wstring wide_line = Utf8ToWide(line);
  wide_line.push_back(L'\n');
  ::OutputDebugStringW(wide_line.c_str());

  if (log_file_path_.empty()) {
    return;
  }

  const std::wstring wide_path = Utf8ToWide(log_file_path_);
  // CreateDirectory fails with ERROR_ALREADY_EXISTS for the common case; the
  // return value is intentionally ignored because the CreateFileW below is
  // the real check.
  // Note: this creates one level of parent directory, which covers the
  // default %LOCALAPPDATA%\Mozc path; deeper custom paths need the user to
  // pre-create them.
  ::CreateDirectoryW(ParentDirectory(wide_path).c_str(), nullptr);

  HANDLE file = ::CreateFileW(wide_path.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    std::wstring marker = L"[SystemOne] log file unavailable: " + wide_path +
                          L" (CreateFileW error " +
                          std::to_wstring(::GetLastError()) + L")\n";
    ::OutputDebugStringW(marker.c_str());
    return;
  }

  // New file: write a UTF-16LE BOM so editors (Notepad included) detect the
  // encoding instead of mangling the Japanese text.
  LARGE_INTEGER size{};
  if (::GetFileSizeEx(file, &size) && size.QuadPart == 0) {
    const wchar_t bom = 0xFEFF;
    DWORD written = 0;
    ::WriteFile(file, &bom, sizeof(bom), &written, nullptr);
  }

  DWORD written = 0;
  ::WriteFile(file, wide_line.data(),
              static_cast<DWORD>(wide_line.size() * sizeof(wchar_t)), &written,
              nullptr);
  ::CloseHandle(file);
}

}  // namespace session
}  // namespace mozc
