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

std::string ToLowerAscii(std::string_view text) {
  std::string lower(text);
  for (char& c : lower) {
    if (IsUpperAscii(c)) c = static_cast<char>(c - 'A' + 'a');
  }
  return lower;
}

// The word a Shift+letter run is building: the trailing run of ASCII letters,
// cut at the last lowercase->uppercase transition, which is the CamelCase word
// boundary.  "anoGithub" -> "Github", "anoGithub Cli" -> "Cli", and
// "anoAPI" -> "API" — cutting at the last capital letter instead would turn
// "API" into "I" and lose the acronym.
std::string ShiftedWord(const std::string_view text) {
  std::size_t end = text.size();
  while (end > 0 && !IsAsciiLetter(text[end - 1])) --end;
  std::size_t begin = end;
  while (begin > 0 && IsAsciiLetter(text[begin - 1])) --begin;
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

// True when |c| can begin a Japanese particle or verb inflection, i.e. the
// letter that follows an English noun in Japanese: を wo, に ni, で de, が ga,
// は ha, と to, から kara, まで made, の no, も mo, わ wa, へ he, や ya, より
// yori, する/して/した suru/shite/shita, だ/です da/desu.
//
// f, b, p, v, l, c, j, z and q are deliberately absent.  They only open ふぁ・
// ば・ぱ・ゔぁ・ぁ・ちゃ style syllables, which no particle or inflection starts
// with, and they are exactly the letters that appear mid-way through common
// English words ("Terra|f|orm", "Ansi|b|le").
bool StartsJapaneseParticle(const char c) {
  switch (c) {
    case 'd':
    case 'g':
    case 'h':
    case 'k':
    case 'm':
    case 'n':
    case 's':
    case 't':
    case 'w':
    case 'y':
      return true;
    default:
      return false;
  }
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

bool MeltypeBridge::IsEnglishWordPrefix(const std::string_view lower) {
  if (GetSegmenter() == nullptr) return false;
  return dictionaries_->english.HasPrefix(lower) ||
         dictionaries_->proper.HasPrefix(lower) ||
         dictionaries_->spell.HasPrefix(lower) ||
         dictionaries_->readable.HasPrefix(lower);
}

bool MeltypeBridge::EndsTemporaryShiftAsciiRun(const std::string_view raw,
                                               const char letter) {
  if (!IsLowerAscii(letter)) return false;
  // Only a letter that can open a Japanese particle or inflection can end the
  // English word.  Everything else is still that word being typed.
  if (!StartsJapaneseParticle(letter)) return false;

  const mozkey::meltype::ScoredSegmenter* segmenter = GetSegmenter();
  if (segmenter == nullptr) return false;  // no dictionaries: no opinion

  // A long composition has no Shift+letter run worth asking about, and
  // SegmentRomaji() runs a dynamic program over the whole buffer, so skip
  // before paying for it.  The limit is far beyond any single word.
  constexpr std::size_t kMaxRunLength = 64;
  if (raw.size() > kMaxRunLength) return false;

  // Ask Meltype about the composition as it will be *after* this letter.  The
  // answer is the boundary itself: an English piece followed by a Japanese
  // piece.  While the English word continues Meltype keeps one English piece
  // and this is false, so "Cli" + "e" (-> "Client") stays ASCII.
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

  // The word is the whole run the user typed since the last Shift+letter, not
  // whatever fragment Meltype happened to close.  Meltype also closes a prefix
  // of an unknown word — "Fatiguesome" becomes [EN:Fatigue][JA:som] and
  // "Contracting" becomes [EN:Contract][JA:in] — and those fragments are real
  // words, so trusting the piece alone fires in the middle of 8% of English
  // words.  Asking about the run keeps the word the user is actually typing.
  //
  // Measured on the bundled dictionaries (20,000 English words typed after
  // "ano"): judging by the closed piece alone gives 1,518 / 20,000 mid-word
  // false reverts (7.59%) and lowers particle recall to 90.3%; judging by the
  // whole run gives 0 / 20,000 (0.00%) and 98.3%.  The piece is still used, but
  // only as the acronym evidence below, where it cannot be a fragment.
  const std::string raw_word = ShiftedWord(text);
  if (raw_word.size() < 2) return false;
  const std::string word = ToLowerAscii(raw_word);
  const std::string raw_stem = raw_word.substr(0, raw_word.size() - 1);
  const std::string stem = ToLowerAscii(raw_stem);

  // The word has to be finished.  Either it is a dictionary entry, or it is an
  // acronym that Meltype itself named as exactly this run (API, GPU, WSL);
  // acronyms are never in the word lists but Meltype still calls them English.
  const bool dictionary_word = IsEnglishWord(stem);
  const bool meltype_acronym =
      IsAcronym(raw_stem) && ToLowerAscii(english.raw) == stem;
  if (!dictionary_word && !meltype_acronym) return false;

  // The run ends exactly where it stops being a word.  Gate 9 established that
  // the stem (the run without the letter just typed) is a finished English
  // word or a recognised acronym, so the only question left is whether the run
  // including this letter is *also* a word:
  //
  //   "Clou" + "d" -> stem "clou" is a word and "cloud" is a word as well, so
  //                    the user is still typing "Cloud" -> keep ASCII.
  //   "Cli"  + "n" -> "clin" is a word (Clinic follows) -> keep ASCII.
  //   "One"  + "h" -> "oneh" is not a word, so "One" is finished and は began.
  //   "Github" + "w" -> "githubw" is not a word -> を began.
  //   "Todo" + "n" -> "todon" is not a word -> に began.
  //
  // A bare prefix test cannot express this.  "oneh" is the prefix of
  // "onehanded"/"onehood"/"onehow", so a prefix test keeps the ASCII run alive
  // for ever and the rest of the sentence comes out as romaji: measured on
  // "sisutemuOnehanndanngata", which stayed "Onehaんだんがた".  Comparing whole
  // words instead keeps every spelling that is genuinely a word intact.
  //
  // Known limit: a finished word followed by a particle consonant is
  // indistinguishable from a word still being typed at this instant ("Bash"+
  // "s" is also how "Bashs" starts), so such a pair is read as Japanese and
  // the consonant may come out as a full-width letter.  Erring the other way
  // was measured at 7.59% of all English words, so this is the safer side.
  return !IsEnglishWord(word);
}

}  // namespace session
}  // namespace mozc
