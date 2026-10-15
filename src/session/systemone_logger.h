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

#ifndef MOZC_SESSION_SYSTEMONE_LOGGER_H_
#define MOZC_SESSION_SYSTEMONE_LOGGER_H_

#include <mutex>
#include <string>

#include "session/systemone_client.h"

namespace mozc {
namespace session {

// Writes one line per SystemOne exchange so a user who opens the log in a
// text editor can see what was sent, what came back, and why a request
// failed, without guessing.
//
// Sample lines:
//   [SystemOne] 2026-10-14 16:00:00 [URL: https://.../systemone]
//   [model: jev-1.13-free] [reading: きょうの天気] [options: 3]
//   -> choice="天気" (conf=0.95, latency=512ms)
//   [SystemOne ERROR] 2026-10-14 16:00:01 [URL: https://.../systemone]
//   [model: jev-1.13-free] [reading: きょうの天気] [options: 3]
//   [HTTP 401] message="http 401: {"error":"unauthorized"}"
//
// The API key is never logged.  The state string is logged because it is the
// input the model judged; the config UI warns users about that and lets them
// turn logging off (config.systemone_debug_logging).
//
// Output sinks:
//   - OutputDebugStringW (DebugView, debuggers)
//   - an appended UTF-16LE text file, BOM-prefixed so editors such as
//     Notepad detect the encoding instead of mangling the Japanese text,
//     %LOCALAPPDATA%\Mozc\systemone.log by default, overridable with
//     SetLogFilePath
//
// A logging failure (unwritable path, disk full) never breaks the IME: the
// debug sink keeps working and one marker line explains the file is gone.
class SystemOneLogger {
 public:
  // enabled=false makes every LogExchange call a no-op; pass
  // config.systemone_debug_logging() here.  The logger keeps no Config
  // dependency so this file stays decoupled from the config proto.
  explicit SystemOneLogger(bool enabled);

  // Overrides the default %LOCALAPPDATA%\Mozc\systemone.log target.  Pass an
  // empty path to keep OutputDebugStringW output only.
  void SetLogFilePath(std::string path);

  // Emits one line for the exchange.  Safe to call from any thread and while
  // the feature is off (early-returns).
  void LogExchange(const SystemOneRequest& request,
                   const SystemOneResponse& response);

 private:
  std::mutex mutex_;
  bool enabled_ = false;
  std::string log_file_path_;
};

}  // namespace session
}  // namespace mozc

#endif  // MOZC_SESSION_SYSTEMONE_LOGGER_H_
