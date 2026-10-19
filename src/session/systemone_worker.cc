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

#include "session/systemone_worker.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace mozc {
namespace session {

SystemOneWorker::SystemOneWorker(SystemOneClient::ExchangeFn exchange)
    : client_(std::move(exchange)) {}

SystemOneWorker::~SystemOneWorker() {
  Stop();
}

void SystemOneWorker::Start() {
  std::lock_guard<std::mutex> lock(mu_);
  if (started_) {
    return;
  }
  stop_ = false;
  started_ = true;
  worker_ = std::thread(&SystemOneWorker::WorkerLoop, this);
}

void SystemOneWorker::Stop() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!started_) {
      return;
    }
    stop_ = true;
    latest_requests_.clear();
    results_.clear();
  }
  cv_.notify_all();

  if (worker_.joinable()) {
    worker_.join();
  }

  std::lock_guard<std::mutex> lock(mu_);
  started_ = false;
}

void SystemOneWorker::Submit(std::vector<SystemOneRequest> requests,
                             const uint32_t generation) {
  Start();

  {
    std::lock_guard<std::mutex> lock(mu_);
    latest_requests_ = std::move(requests);
    latest_request_generation_ = generation;
    // Results belong to the batch that is being asked, and that batch has
    // just been replaced.
    results_.clear();
    results_generation_ = 0;
  }
  cv_.notify_one();
}

void SystemOneWorker::CancelPending() {
  std::lock_guard<std::mutex> lock(mu_);
  latest_requests_.clear();
  results_.clear();
  results_generation_ = 0;
}

std::vector<std::optional<SystemOneResponse>> SystemOneWorker::TakeResults(
    const uint32_t generation, const std::size_t count) {
  std::vector<std::optional<SystemOneResponse>> snapshot(count);
  std::lock_guard<std::mutex> lock(mu_);
  if (results_generation_ != generation) {
    return snapshot;
  }
  const std::size_t shared = std::min(count, results_.size());
  for (std::size_t i = 0; i < shared; ++i) {
    snapshot[i] = results_[i];
  }
  return snapshot;
}

void SystemOneWorker::WorkerLoop() {
  while (true) {
    std::vector<SystemOneRequest> requests;
    uint32_t generation = 0;

    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock,
               [this] { return stop_ || !latest_requests_.empty(); });

      if (stop_) {
        return;
      }

      requests = std::move(latest_requests_);
      generation = latest_request_generation_;
      latest_requests_.clear();
      results_.assign(requests.size(), std::nullopt);
      results_generation_ = generation;
    }

    for (std::size_t i = 0; i < requests.size(); ++i) {
      {
        std::lock_guard<std::mutex> lock(mu_);
        if (stop_) {
          break;
        }
        // A newer batch replaced this one between questions: stop burning
        // network on answers the session will drop by generation anyway.
        if (!latest_requests_.empty() &&
            latest_request_generation_ != generation) {
          break;
        }
      }

      // SystemOneClient::Ask never throws; every failure lands in
      // error_message.
      SystemOneResponse response = client_.Ask(requests[i]);

      {
        std::lock_guard<std::mutex> lock(mu_);
        // Store only against our own generation: Submit() clears the slot for
        // a newer batch, CancelPending() clears it outright, and a stale
        // answer must never surface under someone else's questions.
        if (!stop_ && results_generation_ == generation &&
            results_.size() == requests.size()) {
          results_[i] = std::move(response);
        }
      }
    }
  }
}

}  // namespace session
}  // namespace mozc
