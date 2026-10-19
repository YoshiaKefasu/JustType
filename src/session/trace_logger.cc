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
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
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
#include "session/trace_logger.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "base/system_util.h"

namespace mozc {
namespace session {
namespace {

std::atomic<uint64_t> g_sequence_counter{0};

std::once_flag g_enabled_once;
bool g_enabled = true;

std::mutex g_path_mutex;
std::string g_path_override;

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

// A logging failure must never break the IME, so every fallible step degrades
// to a no-op instead of an error.
std::string DefaultLogFilePath() {
  const std::string profile = SystemUtil::GetUserProfileDirectory();
  if (profile.empty()) {
    return "";
  }
  const std::wstring wide_profile = Utf8ToWide(profile);
  if (wide_profile.empty()) {
    return "";
  }
  // GetUserProfileDirectory() already ends with the product directory
  // (C:\Users\<user>\AppData\LocalLow\JustType, or ...\Mozc for a pre-rename
  // installation), so appending another directory name would write to
  // LocalLow\<product>\<product>\trace.log, where nobody looks.
  return profile + "\\trace.log";
}

std::wstring ParentDirectory(const std::wstring& path) {
  const size_t slash = path.find_last_of(L'\\');
  if (slash == std::wstring::npos || slash == 0) {
    return L"";
  }
  return path.substr(0, slash);
}

bool EnvDisablesTrace() {
  char buffer[8] = {};
  const DWORD len =
      ::GetEnvironmentVariableA("MOZC_TRACE_LOG", buffer, sizeof(buffer));
  if (len == 0 || len >= sizeof(buffer)) {
    return false;  // Unset or an unrecognized longer value: keep tracing on.
  }
  return std::strcmp(buffer, "0") == 0 ||
         std::strcmp(buffer, "false") == 0 || std::strcmp(buffer, "off") == 0 ||
         std::strcmp(buffer, "no") == 0;
}

std::string ResolveLogFilePath() {
  std::lock_guard<std::mutex> lock(g_path_mutex);
  if (!g_path_override.empty()) {
    return g_path_override;
  }
  return DefaultLogFilePath();
}

}  // namespace

// static
bool TraceLogger::Enabled() {
  std::call_once(g_enabled_once, [] { g_enabled = !EnvDisablesTrace(); });
  return g_enabled;
}

// static
uint64_t TraceLogger::NextSequenceId() {
  return g_sequence_counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

// static
std::string TraceLogger::FormatMillis(double ms) {
  char buffer[32] = {};
  std::snprintf(buffer, sizeof(buffer), "%.3f", ms);
  return buffer;
}

// static
void TraceLogger::SetFilePathForTesting(std::string path) {
  std::lock_guard<std::mutex> lock(g_path_mutex);
  g_path_override = std::move(path);
}

// static
void TraceLogger::WriteBlock(uint64_t seq, double seq_offset_ms,
                             double total_ms, const std::string& detail,
                             const std::vector<std::string>& steps) {
  if (!Enabled()) {
    return;
  }

  std::string header = "[TRACE #" + std::to_string(seq);
  if (seq_offset_ms >= 0.0) {
    header += " +" + FormatMillis(seq_offset_ms) + "ms";
  }
  header += "] Total: " + FormatMillis(total_ms) + "ms | " + detail;

  std::string block = header + "\n";
  for (const std::string& step : steps) {
    block += step;
    block += "\n";
  }

  const std::wstring wide_block = Utf8ToWide(block);
  if (wide_block.empty()) {
    return;
  }

  const std::wstring wide_path = Utf8ToWide(ResolveLogFilePath());
  if (wide_path.empty()) {
    return;
  }

  // CreateDirectory fails with ERROR_ALREADY_EXISTS for the common case; the
  // return value is intentionally ignored because CreateFileW below is the
  // real check.  One level of parent directory is enough for the default
  // %LOCALAPPDATA%\Mozc path.
  ::CreateDirectoryW(ParentDirectory(wide_path).c_str(), nullptr);

  HANDLE file = ::CreateFileW(wide_path.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
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
  ::WriteFile(file, wide_block.data(),
              static_cast<DWORD>(wide_block.size() * sizeof(wchar_t)), &written,
              nullptr);
  ::CloseHandle(file);
}

}  // namespace session
}  // namespace mozc
