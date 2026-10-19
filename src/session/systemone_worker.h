// Copyright 2026 The Mozkey Authors
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
// A PARTICULAR PURPOSE ARE DISCLAIMED.

#ifndef MOZC_SESSION_SYSTEMONE_WORKER_H_
#define MOZC_SESSION_SYSTEMONE_WORKER_H_

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "session/systemone_client.h"

namespace mozc {
namespace session {

// Runs SystemOneClient::Ask on a dedicated worker thread so the key-event path
// never blocks on the network.
//
// The unit of work is one *batch*: one request per Japanese piece of the
// current composition, executed in order.  The concurrency model mirrors the
// old single-request worker — latest-only submission (a queued old batch is
// overwritten), stored results, and generation-matched retrieval so the
// session drops any answer that no longer matches the current composition —
// only that "one question per keystroke" became "one question per Japanese
// piece":
//
//   Submit({q_sakki, q_deshirabeta}, generation)   // keystroke N
//   TakeResults(generation, 2)                     // poll tick: whichever
//                                                  // answers have landed
//
// A keystroke therefore costs Submit() (a mutex + notify, sub-microsecond),
// and the answers are applied later through an APPLY_SYSTEMONE_LIVE delayed
// callback that polls TakeResults().  Results are *not* consumed on read, so
// one poll tick can collect several answers while the session keeps its own
// per-piece "already applied" flags; a superseded batch's late answers never
// match the generation and are dropped.
class SystemOneWorker {
 public:
  // |exchange| is forwarded to the internal SystemOneClient; pass nullptr for
  // the default WinHTTP transport.  Tests inject a fake exchange to run the
  // whole worker deterministically without a network.
  explicit SystemOneWorker(SystemOneClient::ExchangeFn exchange = nullptr);
  ~SystemOneWorker();

  SystemOneWorker(const SystemOneWorker&) = delete;
  SystemOneWorker& operator=(const SystemOneWorker&) = delete;

  // Idempotent.  Called lazily on the first Submit().
  void Start();
  void Stop();

  // Submits one batch of questions, replacing any queued batch.  An empty
  // batch clears the pending work.  Starts the worker on first use.
  void Submit(std::vector<SystemOneRequest> requests, uint32_t generation);

  // Clears the queued batch and stored results.  Requests already in flight
  // on the worker are not forcibly cancelled; their answers are dropped by
  // the generation check when they land.
  void CancelPending();

  // Returns a vector of |count| slots for |generation|: the answers that have
  // arrived so far, std::nullopt for the ones that have not.  Returns all
  // nullopt when the stored results belong to another generation (nothing
  // submitted yet, a newer batch replaced this one, or CancelPending ran).
  // Reads do not consume: the session decides when an answer is applied.
  std::vector<std::optional<SystemOneResponse>> TakeResults(
      uint32_t generation, std::size_t count);

 private:
  void WorkerLoop();

  SystemOneClient client_;

  std::mutex mu_;
  std::condition_variable cv_;

  bool started_ = false;
  bool stop_ = false;

  // Queued batch (latest-only): the next work the worker thread picks up.
  std::vector<SystemOneRequest> latest_requests_;
  uint32_t latest_request_generation_ = 0;

  // Results of the batch the worker is executing, aligned with its request
  // order.  A batch started under another generation drops its answers here.
  std::vector<std::optional<SystemOneResponse>> results_;
  uint32_t results_generation_ = 0;

  std::thread worker_;
};

}  // namespace session
}  // namespace mozc

#endif  // MOZC_SESSION_SYSTEMONE_WORKER_H_
