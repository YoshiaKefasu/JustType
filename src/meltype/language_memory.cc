// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The Mozkey Authors

#include "meltype/language_memory.h"

#include <iterator>
#include <utility>

namespace mozkey {
namespace meltype {
namespace {

bool IsLowerAsciiLetter(const char c) { return 'a' <= c && c <= 'z'; }
bool IsUpperAsciiLetter(const char c) { return 'A' <= c && c <= 'Z'; }

std::string ToLowerAscii(const std::string_view text) {
  std::string lower(text);
  for (char& c : lower) {
    if (IsUpperAsciiLetter(c)) c = static_cast<char>(c - 'A' + 'a');
  }
  return lower;
}

// |map| が上限を超えていれば、|used_at| が最も小さい (最後に使われたのが
// 最も古い) 要素を 1 つ捨てる。|used_at| は map の要素から通し番号を取り出す。
//
// 走査は上限を超えたときにだけ起きるので、普段の確定はここを通らない。
template <typename Map, typename UsedAtFn>
void EvictOldestWhileOverLimit(Map* map, const UsedAtFn& used_at) {
  while (map->size() > LanguageMemory::kMaxEntries) {
    auto oldest = map->begin();
    for (auto it = std::next(map->begin()); it != map->end(); ++it) {
      if (used_at(*it) < used_at(*oldest)) oldest = it;
    }
    map->erase(oldest);
  }
}

}  // namespace

void LanguageMemory::RememberEnglish(const std::string_view word) {
  if (word.empty() || word.size() > 64) return;
  for (const char c : word) {
    if (!IsLowerAsciiLetter(c) && !IsUpperAsciiLetter(c)) return;
  }
  const std::string lower = ToLowerAscii(word);
  std::lock_guard<std::mutex> lock(write_mutex_);
  const std::shared_ptr<const WordMap> current =
      words_.load(std::memory_order_acquire);
  if (current->find(lower) != current->end()) return;  // already known
  auto next = std::make_shared<WordMap>(*current);
  next->insert_or_assign(lower, next_used_++);
  EvictOldestWhileOverLimit(next.get(),
                            [](const auto& entry) { return entry.second; });
  words_.store(std::move(next), std::memory_order_release);
}

bool LanguageMemory::IsEnglish(const std::string_view lower) const {
  if (lower.empty()) return false;
  const std::shared_ptr<const WordMap> current =
      words_.load(std::memory_order_acquire);
  return current->find(std::string(lower)) != current->end();
}

std::size_t LanguageMemory::Count() const {
  const std::shared_ptr<const WordMap> current =
      words_.load(std::memory_order_acquire);
  return current->size();
}

void LanguageMemory::RememberSelection(const std::string_view reading,
                                       const std::string_view surface) {
  if (reading.empty() || surface.empty()) return;
  if (reading.size() > 128 || surface.size() > 256) return;
  // A reading that equals its surface still has to be remembered.  "GPU" typed
  // with Shift is committed as "GPU", so reading == surface; dropping that entry
  // meant the one case the user cares most about was the one case never stored.
  std::lock_guard<std::mutex> lock(write_mutex_);
  const std::shared_ptr<const SelectionMap> current =
      selections_.load(std::memory_order_acquire);
  const auto found = current->find(std::string(reading));
  if (found != current->end() && found->second.surface == surface) return;
  auto next = std::make_shared<SelectionMap>(*current);
  // Re-picking a reading the user already chose refreshes it instead of leaving
  // its original position behind: the entries that are being used are the ones
  // worth keeping when the table is full.
  (*next)[std::string(reading)] =
      Selection{std::string(surface), next_used_++};
  EvictOldestWhileOverLimit(
      next.get(), [](const auto& entry) { return entry.second.used; });
  selections_.store(std::move(next), std::memory_order_release);
}

const std::string* LanguageMemory::LookupSelection(
    const std::string_view reading) const {
  if (reading.empty()) return nullptr;
  const std::shared_ptr<const SelectionMap> current =
      selections_.load(std::memory_order_acquire);
  const auto found = current->find(std::string(reading));
  if (found == current->end()) return nullptr;
  // The map is immutable once published, so the pointer stays valid until the
  // next RememberSelection() swaps it in.  Callers copy the value.
  return &found->second.surface;
}

std::size_t LanguageMemory::SelectionCount() const {
  const std::shared_ptr<const SelectionMap> current =
      selections_.load(std::memory_order_acquire);
  return current->size();
}

LanguageMemory& SharedLanguageMemory() {
  static LanguageMemory instance;
  return instance;
}

}  // namespace meltype
}  // namespace mozkey
