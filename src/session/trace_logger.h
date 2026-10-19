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

#ifndef MOZC_SESSION_TRACE_LOGGER_H_
#define MOZC_SESSION_TRACE_LOGGER_H_

#include <cstdint>
#include <string>
#include <vector>

namespace mozc {
namespace session {

// Writes the per-keystroke pipeline trace to the product directory as
// trace.log (Windows: AppData\LocalLow\JustType\trace.log, or
// AppData\LocalLow\Mozc\trace.log for a pre-rename installation), so every
// stage from a key press
// to Commit - Composer, Suggest/Predict, Mozc converter, Meltype, the
// SystemOne worker, and the final display - can be inspected afterwards with
// per-step timings.
//
// Output format (UTF-16LE with BOM, appended, one block per pipeline entry):
//
//   [TRACE #12] Total: 1.730ms | Key: 'a'
//     ├─ [Composer] time=0.052ms | raw="imahadarkaga" -> kana="いまはだrかが"
//     ├─ [Mozc Converter] time=1.200ms | candidates=["いま肌r化が", ...] (top=3)
//     └─ [Display Output] time=0.100ms | preedit="..." | window_visible=false
//   [TRACE #12 +231.000ms] Total: 2.400ms | Command: APPLY_LIVE_CONVERSION
//     ...
//   [TRACE #12 +640.000ms] Total: 0.300ms | Command: APPLY_SYSTEMONE_LIVE
//     ├─ [SystemOne Worker] time=0.010ms | status=ASYNC | latency=409ms |
//     |    state="読み: いまはだrかが" -> choice="今は誰かが"
//     └─ [Display Output] time=0.080ms | preedit="今は誰かが" | ...
//
// Sequence ids come from a process-wide counter, so interleaved sessions never
// collide, and a sequence survives across the delayed live-conversion and
// async SystemOne callbacks that follow the keystroke that started it.  A
// block is buffered in memory and appended with a single file write, keeping
// the per-keystroke cost to one open/append/close cycle.
//
// Enabled by default; setting MOZC_TRACE_LOG to "0", "false", "off" or "no"
// (case-insensitive) disables it.  A logging failure (unwritable path, disk
// full) never breaks the IME.
class TraceLogger {
 public:
  // Whether tracing is enabled.  Cached on first use.
  static bool Enabled();

  // Process-wide sequence id counter.
  static uint64_t NextSequenceId();

  // Appends one buffered block: the header line plus the indented step lines.
  // |seq_offset_ms| is the time from the sequence start to this block start,
  // or a negative value for the block that opens the sequence (no offset).
  static void WriteBlock(uint64_t seq, double seq_offset_ms, double total_ms,
                         const std::string& detail,
                         const std::vector<std::string>& steps);

  // Overrides the log file path.  Pass an empty string to restore the default
  // <UserProfile>/Mozc/trace.log.  Intended for tests.
  static void SetFilePathForTesting(std::string path);

  // Formats |value| milliseconds with microsecond resolution, e.g. "0.052".
  static std::string FormatMillis(double ms);
};

}  // namespace session
}  // namespace mozc

#endif  // MOZC_SESSION_TRACE_LOGGER_H_
