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

#ifndef MOZC_SESSION_MELTYPE_BRIDGE_H_
#define MOZC_SESSION_MELTYPE_BRIDGE_H_

#include <memory>
#include <mutex>
#include <string>

#include "meltype/scored_segmenter.h"

namespace mozc {
namespace session {

// Bridges Meltype's English/Japanese segmentation into a Session.
//
// The dictionaries are tens of megabytes once loaded, so they are read exactly
// once per process and shared by every Session through a static instance.  That
// keeps the cost off the key-event thread: a keystroke only calls
// SegmentRomaji(), which runs the dynamic program over the composing buffer.
//
// When no dictionary directory can be found (a build without the data, or a
// sandbox that hides it), the segmenter is never built and every segmentation
// request returns an empty vector.  Callers must treat that as "no opinion"
// and keep Mozc's own output, never as "this is all Japanese".  An empty vector
// therefore covers both cases, so there is no separate availability query to
// keep in sync.
class MeltypeBridge {
 public:
  // Segments one romaji string into English and Japanese spans.
  // Returns an empty vector when the dictionaries are unavailable.
  static std::vector<mozkey::meltype::ScoredSegmenter::Segment> SegmentRomaji(
      const std::string& romaji, bool final);

  // Starts loading the dictionaries on a background thread so that the first
  // keystroke does not stall for the multi-second synchronous load.  Loads
  // exactly once per process (std::call_once underneath); a later
  // SegmentRomaji() only blocks if the load is still in flight.  Idempotent,
  // so call it once while the server starts up.
  static void PreloadAsync();

  // Joins the background loader started by PreloadAsync(), if any, so the load
  // never races static destruction at process exit.  No-op when nothing was
  // started, and it never blocks longer than the load itself would.
  static void WaitForPreload();

 private:
  // Loads the dictionaries on first use.  Returns the shared segmenter, or
  // nullptr when the data could not be found.
  static const mozkey::meltype::ScoredSegmenter* GetSegmenter();

  static std::once_flag init_flag_;
  static std::unique_ptr<mozkey::meltype::MeltypeDictionaries> dictionaries_;
  static std::unique_ptr<mozkey::meltype::ScoredSegmenter> segmenter_;
};

}  // namespace session
}  // namespace mozc

#endif  // MOZC_SESSION_MELTYPE_BRIDGE_H_
