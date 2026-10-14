// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The Mozkey Authors

#include "meltype/word_list.h"

#include <fstream>
#include <sstream>

namespace mozkey {
namespace meltype {
namespace {

// 語の区切りに使う文字 (Meltype の WordList.ParseWords と同じ)。
bool IsSeparator(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == ',' || c == '\n';
}

}  // namespace

bool WordList::AddWordOwned(const std::string& block, std::size_t offset, std::size_t length) {
  if (length == 0) return false;
  std::string_view word(block.data() + offset, length);
  if (!words_.insert(word).second) return false;
  // 語のすべての prefix を登録する (HasPrefix 用)。
  for (std::size_t i = 1; i <= length; ++i) {
    prefixes_.insert(std::string_view(block.data() + offset, i));
  }
  return true;
}

bool WordList::AddWord(std::string_view word) {
  if (word.empty()) return false;
  blocks_.push_back(std::string(word));
  return AddWordOwned(blocks_.back(), 0, blocks_.back().size());
}

std::size_t WordList::AddText(std::string_view text) {
  // 渡された文字列を 1 塊として取り、その中を指す string_view をキーにする。
  blocks_.push_back(std::string(text));
  const std::string& block = blocks_.back();
  std::size_t added = 0;
  std::size_t line_begin = 0;
  while (line_begin <= block.size()) {
    std::size_t line_end = block.find('\n', line_begin);
    if (line_end == std::string::npos) line_end = block.size();
    std::string_view line(block.data() + line_begin, line_end - line_begin);
    // # 以降はコメント。
    if (std::size_t hash = line.find('#'); hash != std::string_view::npos) {
      line = line.substr(0, hash);
    }
    std::size_t token_begin = 0;
    while (token_begin < line.size()) {
      while (token_begin < line.size() && IsSeparator(line[token_begin])) ++token_begin;
      if (token_begin >= line.size()) break;
      std::size_t token_end = token_begin;
      while (token_end < line.size() && !IsSeparator(line[token_end])) ++token_end;
      std::size_t offset = line_begin + token_begin;
      std::size_t length = token_end - token_begin;
      token_begin = token_end;
      // 英小文字以外を含む語は無視する (大文字は小文字にしてから渡すこと)。
      bool only_lower = true;
      for (std::size_t i = 0; i < length; ++i) {
        char c = block[offset + i];
        if (c < 'a' || c > 'z') {
          only_lower = false;
          break;
        }
      }
      if (!only_lower) continue;
      if (AddWordOwned(block, offset, length)) ++added;
    }
    if (line_end == block.size()) break;
    line_begin = line_end + 1;
  }
  return added;
}

std::size_t WordList::AddTextLowercased(std::string_view text) {
  std::string lowered(text);
  for (char& c : lowered) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return AddText(lowered);
}

std::size_t WordList::AddFile(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return 0;
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return AddText(buffer.str());
}

std::vector<std::string> WordList::Words() const {
  std::vector<std::string> out;
  out.reserve(words_.size());
  for (std::string_view word : words_) out.emplace_back(word);
  return out;
}

}  // namespace meltype
}  // namespace mozkey
