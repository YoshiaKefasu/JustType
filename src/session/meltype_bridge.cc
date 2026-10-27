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

#include "session/meltype_bridge.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "base/system_util.h"
#include "meltype/scored_segmenter.h"

namespace mozc {
namespace session {

namespace {

// Environment override.  Lets a developer point at a dictionary directory
// without reinstalling, and lets tests run against a small fixture.
constexpr char kDictionaryEnvVar[] = "MOZKEY_MELTYPE_DICT_DIR";

bool HasEnglishList(const std::string& directory) {
  if (directory.empty()) return false;
  std::ifstream stream(directory + "/english.txt", std::ios::binary);
  return stream.good();
}

// Where the bundled dictionaries live.
//
// 1. MOZKEY_MELTYPE_DICT_DIR, when set.
// 2. The source tree, relative to this file.  This is the path taken by a
//    Bazel test, whose runfiles mirror the package layout.
// 3. Installed next to the server binary, so a shipped MSI keeps working
//    without the source tree.
std::string ResolveDictionaryDirectory() {
  if (const char* from_env = std::getenv(kDictionaryEnvVar); from_env && *from_env) {
    return from_env;
  }

  std::error_code error;
  const std::filesystem::path here =
      std::filesystem::absolute(std::filesystem::path(__FILE__), error);
  if (!error) {
    for (const std::filesystem::path& candidate :
         {here.parent_path() / "dictionaries",
          here.parent_path().parent_path() / "dictionaries"}) {
      if (HasEnglishList(candidate.string())) return candidate.string();
    }
  }

  // GetServerDirectory() may or may not end with a separator, so normalize
  // before appending.
  std::string server_dir = mozc::SystemUtil::GetServerDirectory();
  if (!server_dir.empty() && server_dir.back() != '\\' && server_dir.back() != '/') {
    server_dir += '\\';
  }
  const std::string install_dir = server_dir + "meltype_dictionaries";
  if (HasEnglishList(install_dir)) return install_dir;

  return std::string();
}

}  // namespace

std::once_flag MeltypeBridge::init_flag_;
std::unique_ptr<mozkey::meltype::MeltypeDictionaries> MeltypeBridge::dictionaries_;
std::unique_ptr<mozkey::meltype::ScoredSegmenter> MeltypeBridge::segmenter_;

namespace {
// Guards the single background loader thread started by PreloadAsync().
std::mutex g_preload_mutex;
std::thread g_preload_thread;
}  // namespace

void MeltypeBridge::PreloadAsync() {
  std::lock_guard<std::mutex> lock(g_preload_mutex);
  if (g_preload_thread.joinable()) {
    return;
  }
  g_preload_thread = std::thread([] {
    (void)GetSegmenter();  // Triggers the std::call_once dictionary load.
  });
}

void MeltypeBridge::WaitForPreload() {
  std::thread loader;
  {
    std::lock_guard<std::mutex> lock(g_preload_mutex);
    loader = std::move(g_preload_thread);
  }
  if (loader.joinable()) {
    loader.join();
  }
}

const mozkey::meltype::ScoredSegmenter* MeltypeBridge::GetSegmenter() {
  std::call_once(init_flag_, [] {
    const std::string directory = ResolveDictionaryDirectory();
    if (directory.empty()) return;

    auto dictionaries = std::make_unique<mozkey::meltype::MeltypeDictionaries>();
    if (mozkey::meltype::MeltypeDictionaries::LoadFromDirectory(directory,
                                                               *dictionaries) == 0) {
      return;
    }
    dictionaries_ = std::move(dictionaries);
    segmenter_ = std::make_unique<mozkey::meltype::ScoredSegmenter>(*dictionaries_);
  });
  return segmenter_.get();
}

std::vector<mozkey::meltype::ScoredSegmenter::Segment> MeltypeBridge::SegmentRomaji(
    const std::string& romaji, bool final) {
  const mozkey::meltype::ScoredSegmenter* segmenter = GetSegmenter();
  if (segmenter == nullptr) return {};

  mozkey::meltype::ScoredSegmenter::Options options;
  options.final = final;
  return segmenter->SegmentInput(romaji, options);
}

namespace {

// True when |c| is a lowercase ASCII letter.
bool IsLowerAscii(const char c) { return 'a' <= c && c <= 'z'; }

// True when |c| is an ASCII letter in either case.
bool IsAsciiLetter(const char c) {
  return IsLowerAscii(c) || ('A' <= c && c <= 'Z');
}

// True when |c| is an uppercase ASCII letter.
bool IsUpperAscii(const char c) { return 'A' <= c && c <= 'Z'; }

// True when |c| can be part of an English run the user is typing: letters plus
// the digits and punctuation that product names use (S3, N+1, C++, Next.js).
bool IsWordChar(const char c) {
  if (IsAsciiLetter(c)) return true;
  if ('0' <= c && c <= '9') return true;
  return c == '+' || c == '#' || c == '.' || c == '_' || c == '-';
}

std::string ToLowerAscii(std::string_view text) {
  std::string lower(text);
  for (char& c : lower) {
    if (IsUpperAscii(c)) c = static_cast<char>(c - 'A' + 'a');
  }
  return lower;
}

// The English run the user is typing, taken from the END of |text|.
//
// The run is the trailing sequence of word characters, cut at the last
// lowercase->uppercase transition (the CamelCase boundary): "anoGithub" ->
// "Github", "anoGithub Cli" -> "Cli", "anoAPI" -> "API".  Cutting at the last
// capital instead would turn "API" into "I" and lose the acronym.
//
// The scan skips a trailing separator, so "Android " and "API " still yield the
// word before the space.  That matters because the boundary decision is made on
// the keystroke *after* the space, when the composition ends with it: without
// skipping it the run would be empty and the caller could never end the ASCII
// run, which is the "space makes everything stay romaji" failure.
std::string ShiftedWord(const std::string_view text) {
  std::size_t end = text.size();
  while (end > 0 && !IsWordChar(text[end - 1])) --end;
  std::size_t begin = end;
  while (begin > 0 && IsWordChar(text[begin - 1])) --begin;
  std::size_t word_begin = begin;
  for (std::size_t i = begin + 1; i < end; ++i) {
    if (IsLowerAscii(text[i - 1]) && IsUpperAscii(text[i])) word_begin = i;
  }
  return std::string(text.substr(word_begin, end - word_begin));
}

// True when |raw| is an all-uppercase acronym of 2..8 letters (API, GPU, WSL).
// Meltype treats those as English words even though no dictionary lists them.
bool IsAcronym(const std::string_view raw) {
  if (raw.size() < 2 || raw.size() > 8) return false;
  for (const char c : raw) {
    if (!IsUpperAscii(c)) return false;
  }
  return true;
}

}  // namespace

// The bundled vocabulary.  Both are answered from the same word lists
// ScoredSegmenter judges English with, so a spelling Meltype treats as English
// here is English there too.
bool MeltypeBridge::IsEnglishWord(const std::string_view lower) {
  if (GetSegmenter() == nullptr) return false;  // no dictionaries: no opinion
  return dictionaries_->english.ContainsWord(lower) ||
         dictionaries_->proper.ContainsWord(lower) ||
         dictionaries_->spell.ContainsWord(lower) ||
         dictionaries_->readable.ContainsWord(lower);
}

bool MeltypeBridge::HasNonCompoundContinuation(const std::string_view prefix,
                                              const std::string_view stem) {
  if (GetSegmenter() == nullptr) return false;
  std::vector<std::string_view> candidates;
  for (const mozkey::meltype::WordList* list :
       {&dictionaries_->english, &dictionaries_->proper,
        &dictionaries_->spell, &dictionaries_->readable}) {
    list->CollectWordsWithPrefix(prefix, &candidates);
    for (const std::string_view candidate : candidates) {
      if (candidate.size() <= stem.size()) continue;
      if (stem != candidate.substr(0, stem.size())) continue;
      // The part after the finished word decides: if it is not itself a word,
      // this spelling is a longer word the user may still be typing, not a
      // compound of the word already on screen.
      //
      // Two-letter remainders ("s", "es", "ed", "ly", "er") are not treated as
      // compounds: they are the ordinary inflections, and reading
      // "adjustor" + "es" as a compound would cut "adjustores" in half.  A real
      // compound's second half is a word in its own right ("handed", "hood",
      // "some").
      const std::string_view rest = candidate.substr(stem.size());
      if (rest.size() < 3) return true;
      if (!IsEnglishWord(rest)) return true;
    }
  }
  return false;
}

bool MeltypeBridge::EndsTemporaryShiftAsciiRun(const std::string_view raw,
                                               const char letter) {
  // Every lowercase letter can start Japanese.  Earlier revisions only allowed
  // the ten consonants that open a particle, which silently dropped the rest:
  // ら行 (r), ば行 (b), ぱ行 (p), ざ行 (z), ちゃ行 (c) and every vowel.  Those
  // are ordinary sentence starts ("Android" + "sumaho" needs no gate at all
  // once the word is finished), so the letter set is no longer a filter; the
  // word tests below are.
  if (!IsLowerAscii(letter)) return false;

  const mozkey::meltype::ScoredSegmenter* segmenter = GetSegmenter();
  if (segmenter == nullptr) return false;  // no dictionaries: no opinion

  // A separator immediately before the letter means the English word is
  // already finished: "Android " + "s" is Android + すまほ, not "Androids".
  // Without this the run taken from the composition is empty and the caller
  // could never leave ASCII mode, which left whole sentences as raw romaji.
  if (!raw.empty() && !IsWordChar(raw.back())) return true;

  // The run the user is typing, WITHOUT the letter just pressed.  It is taken
  // from the composition, so it is only bounded by the run itself and never by
  // the length of the sentence typed before it (an earlier revision gave up on
  // any composition longer than 64 characters, which made a long Japanese
  // sentence followed by English unrecoverable).
  const std::string run = ShiftedWord(raw);
  if (run.empty()) return true;
  constexpr std::size_t kMaxRunLength = 64;
  if (run.size() > kMaxRunLength) return false;

  const std::string stem = ToLowerAscii(run);
  const std::string word = stem + letter;

  // Ask Meltype about the composition as it will be *after* this letter.  A
  // boundary is required: while an English word continues Meltype keeps one
  // English piece, so "Cli" + "e" (-> "Client") stays ASCII, and "Ab" + "a"
  // (-> "Aba") does too, which is what keeps ordinary English words whole.
  std::string text(raw);
  text.push_back(letter);
  mozkey::meltype::ScoredSegmenter::Options options;
  options.final = false;
  const std::vector<mozkey::meltype::ScoredSegmenter::Segment> pieces =
      segmenter->SegmentInput(text, options);
  if (pieces.size() < 2) return false;
  if (pieces.back().is_english) return false;
  const mozkey::meltype::ScoredSegmenter::Segment& english =
      pieces[pieces.size() - 2];
  if (!english.is_english) return false;

  // The word has to be finished.  Either it is a dictionary entry, or it is an
  // acronym that Meltype itself named as exactly this run (API, GPU, WSL);
  // acronyms are never in the word lists but Meltype still calls them English.
  const bool dictionary_word = IsEnglishWord(stem);
  const bool meltype_acronym =
      IsAcronym(run) && ToLowerAscii(english.raw) == stem;
  // 識別子 (S3, C++, N+1, node.js) は辞書に載らず頭字語でもないが、
  // Meltype が英語の区間として確定させているならそのまま信頼する。
  const bool identifier =
      mozkey::meltype::IsIdentifierToken(run) &&
      ToLowerAscii(english.raw) == ToLowerAscii(run);
  if (!dictionary_word && !meltype_acronym && !identifier) return false;

  // The run ends where it stops being a word.  Both questions have to be asked:
  //
  //   "cloud"  is a word               -> still typing Cloud   -> keep ASCII
  //   "aa"     is not a word, but      -> still typing Abandon -> keep ASCII
  //            "aardvark" continues it
  //   "oneh"   is not a word, and its  -> One is finished, は began -> end
  //            continuations are only
  //            compounds (one+handed)
  //   "githubw" is not a word at all   -> を began             -> end
  //
  // Asking only "is it a word" (the previous rule) cut 22.7% of ordinary
  // English words at the first letter that broke a word boundary ("Ab" of
  // "Abandon").  Asking only "is it a prefix" never ended the run after "One",
  // because "oneh" prefixes "onehanded".  The compound test separates the two:
  // a continuation that is merely this word plus another word is not a word the
  // user is still typing.
  if (IsEnglishWord(word)) return false;
  if (HasNonCompoundContinuation(word, stem)) return false;
  return true;
}

std::size_t MeltypeBridge::JapaneseTailLength(const std::string_view raw,
                                             const char letter) {
  const mozkey::meltype::ScoredSegmenter* segmenter = GetSegmenter();
  if (segmenter == nullptr) return 0;
  if (raw.empty() || !IsLowerAscii(letter)) return 0;

  std::string text(raw);
  text.push_back(letter);
  mozkey::meltype::ScoredSegmenter::Options options;
  options.final = false;
  const std::vector<mozkey::meltype::ScoredSegmenter::Segment> pieces =
      segmenter->SegmentInput(text, options);
  if (pieces.size() < 2 || pieces.back().is_english) return 0;

  // The last piece is the Japanese that follows the English word.  Only the
  // part of it that is already inside |raw| has to be rewritten: |letter| has
  // not been inserted yet, so it will be typed in Hiragana once the mode is
  // back.
  std::size_t japanese_begin = text.size();
  for (std::size_t i = pieces.size(); i > 0; --i) {
    const mozkey::meltype::ScoredSegmenter::Segment& piece = pieces[i - 1];
    if (piece.raw.empty()) break;
    if (piece.is_english) break;  // the Japanese tail starts after this piece
    if (japanese_begin < piece.raw.size()) break;
    japanese_begin -= piece.raw.size();
  }
  if (japanese_begin >= raw.size()) return 0;  // nothing typed yet
  return raw.size() - japanese_begin;
}

}  // namespace session
}  // namespace mozc
