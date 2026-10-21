// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The Mozkey Authors

#include "meltype/scored_segmenter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace mozkey {
namespace meltype {
namespace {

// --- 点数 (Meltype の ScoredSegmentation.cs と同じ値) -------------------------

/// 知っている英単語の区間の点の係数 (長さの LengthExponent 乗に掛ける)。
constexpr double kEnglishCharScore = 1.0;
/// 辞書に無い英字の並び (buglowers) の 1 文字あたりの点。知っている英単語
/// (glowers) より低くして、前の日本語を巻き込まないようにする。
constexpr double kUnknownCharScore = 0.5;
/// 英語の区間の長さを何乗して点にするか (1 より大きいと、長い 1 語を短い 2 語より
/// 優先する: mo|string、alcoholic)。
constexpr double kLengthExponent = 2.0;
/// 英語の区間を 1 つ置くたびの減点 (日本語との切り替え)。
constexpr double kSwitchPenalty = 0.5;
/// 英語の区間のすぐ後ろに英語の区間を続けるときの減点 (英字の並びを 2 語に分ける)。
constexpr double kAdjacentPenalty = 4.0;
/// 日本語の区間に、読めない英字が 1 文字残るたびの減点。
constexpr double kUnreadablePenalty = 1.0;

// 助詞 (Meltype の Detection/DictionaryDetector.cs の Particles)。
constexpr std::string_view kParticles[] = {"no", "ga", "wo", "ni", "de", "to",
                                           "ha", "mo", "wa", "he", "ya"};

// 語尾につきうる助詞 (CompositionDetector.cs の TrailingParticles)。
constexpr std::string_view kTrailingParticles[] = {"kara", "made", "yori", "ga", "wo", "ni",
                                                   "de",   "no",   "to",   "mo", "ha", "wa",
                                                   "ya"};

// する の活用 (CompositionDetector.cs の SuruForms)。
constexpr std::string_view kSuruForms[] = {
    "する", "すれ", "した", "して", "しま", "しな",
    "しよ", "しと", "しちゃ", "しろ", "され", "させ", "せず",
};

bool IsAsciiLetter(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool IsLowerAscii(char c) { return c >= 'a' && c <= 'z'; }

bool IsUpperAscii(char c) { return c >= 'A' && c <= 'Z'; }

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

// 英文の中では半角のままにする記号。[ ] は日本語の入力では「」なので含めない。
// (Meltype の IsAsciiSymbol と同じ。空白は記号に数えない)
bool IsAsciiSymbol(char c) {
  return c >= '!' && c <= '~' && !IsAsciiLetter(c) && !IsDigit(c) && c != '[' && c != ']';
}

// 英語の区間の後ろにそのまま続けてよい文字 (, . ! ? - と空白)。
// Meltype は IsAsciiSymbol と同じ判定で、英文の中の空白は日本語の区間のまま通して
// 確定時に間を空ける (AutomaticEnglishSpacing)。このモジュールには確定時の層が無いので、
// 「hello world」を 1 つの英語の区間として返せるよう空白も英語側に付ける。
bool IsEnglishSymbol(char c) { return c == ' ' || IsAsciiSymbol(c); }

std::string ToLowerAscii(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

char ToLowerAscii(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool StartsWith(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool EndsWith(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// 英単語の直後に続けて、たまたま実在の英単語と同じ綴りになる日本語の
// 助詞・助動詞・形式名詞 (made=まで, demo=でも, tame=ため, wake=わけ,
// toki=とき, node=ので, mono=もの, koto=こと, koso=こそ)。英語の区間の
// すぐ後ろなら英語同士の合体ではなく日本語に切り分ける。
constexpr std::string_view kJapaneseNominalTails[] = {
    "made", "demo", "tame", "wake", "toki",
    "node", "mono", "koto", "koso",
};

/// 区間が英字のみで、全綴りが上の日本語の助詞・形式名詞と一致するとき true。
bool IsJapaneseNominalTail(std::string_view raw) {
  if (raw.empty()) return false;
  for (char c : raw) {
    if (!IsAsciiLetter(c)) return false;
  }
  const std::string lower = ToLowerAscii(raw);
  for (std::string_view word : kJapaneseNominalTails) {
    if (lower == word) return true;
  }
  return false;
}

// 助詞 (の が を に で と は も わ へ や) で始まっていれば、その助詞。
std::optional<std::string_view> StartsWithParticle(std::string_view letters) {
  for (std::string_view particle : kParticles) {
    if (StartsWith(letters, particle)) return particle;
  }
  return std::nullopt;
}

// -pedia / -pedias 複合 (pedia / protopedia / wikipedias …)。
// ローマ字だと ぺぢあ になり、途中の to が助詞「と」に割れる。
bool IsPediaEnglishWord(std::string_view lower) {
  std::size_t stem = 0;
  if (EndsWith(lower, "pedias")) {
    stem = 6;
  } else if (EndsWith(lower, "pedia")) {
    stem = 5;
  }
  if (stem == 0 || lower.size() < stem) return false;
  return lower.size() == stem || lower.size() - stem >= 3;
}

// する の活用 (して・した・します …) だけでできたかなか。
bool IsSuruForm(std::string_view kana) {
  bool starts = false;
  for (std::string_view form : kSuruForms) {
    if (StartsWith(kana, form)) {
      starts = true;
      break;
    }
  }
  if (!starts) return false;
  // ひらがな (U+3041 - U+3096) と ー だけか。
  // UTF-8 の第 2 バイトで区切る: b2==0x81 は U+3040..U+307F、b2==0x82 は
  // U+3080..U+30BF。旧判定 (b2==0x81 && b3<=0x96) は し (E3 81 97) 以降を
  // 誤って非ひらがなにし、する活用がぜっと効かなかった。
  for (std::size_t i = 0; i < kana.size();) {
    const unsigned char c = static_cast<unsigned char>(kana[i]);
    if (c == 0xE3 && i + 2 < kana.size()) {
      const unsigned char b2 = static_cast<unsigned char>(kana[i + 1]);
      const unsigned char b3 = static_cast<unsigned char>(kana[i + 2]);
      const bool hiragana =
          (b2 == 0x81 && b3 >= 0x81 && b3 <= 0xBF) ||
          (b2 == 0x82 && b3 >= 0x80 && b3 <= 0x96);
      const bool prolonged = b2 == 0x83 && b3 == 0xBC;  // ー
      if (!hiragana && !prolonged) return false;
      i += 3;
      continue;
    }
    return false;
  }
  return true;
}

// 前の文脈の点数: 英文の続きなら +2、英語なら +1、日本語なら -1、分からなければ 0。
int Score(std::optional<bool> english) {
  if (!english) return 0;
  return *english ? 1 : -1;
}

// UTF-8 の 1 文字ぶんのバイト数。
std::size_t Utf8SequenceLength(std::string_view text, std::size_t i) {
  const unsigned char c = static_cast<unsigned char>(text[i]);
  std::size_t length = 1;
  if (c >= 0xF0) {
    length = 4;
  } else if (c >= 0xE0) {
    length = 3;
  } else if (c >= 0xC0) {
    length = 2;
  }
  return std::min(length, text.size() - i);
}

// ここまでの区切り方 (点・英語の区間・最後の日本語の区間の始まり)。
struct Path {
  double score = 0.0;
  std::vector<std::pair<int, int>> english;  // (始まり, 終わり) の半開区間、昇順
  int japanese_start = 0;
};

}  // namespace

// Segment は同名のメソッドに隠されるので、別名で扱う。
using Segment = ScoredSegmenter::Segment;

// 1 回の Segment() の中だけの状態。再入可能にするためメンバーには持たない。
class Context {
 public:
  Context(std::string_view input, const MeltypeDictionaries& dictionaries,
          const ScoredSegmenter::Options& options)
      : input_(input),
        dictionaries_(dictionaries),
        romaji_(),
        options_(options),
        n_(static_cast<int>(input.size())) {
    Prepare();
  }

  std::vector<Segment> Run();

 private:
  // 位置 i から読めるローマ字 1 音。読めない文字は 1 文字ぶん、kana は空。
  struct Syllable {
    int length = 1;
    std::string kana;
    bool readable = false;
  };

  // 入力の [start, end) / [start, 末尾) を指す。
  std::string_view Sub(int start, int end) const {
    return std::string_view(input_).substr(static_cast<std::size_t>(start),
                                           static_cast<std::size_t>(end - start));
  }
  std::string_view SubFrom(int start) const {
    return std::string_view(input_).substr(static_cast<std::size_t>(start));
  }

  Syllable NextSyllable(int i) const;

  // 区間 [start, end) にローマ字として読めなかった英字があるか。
  bool HasUnreadable(int start, int end) const;

  // 区間 [start, end) を日本語として読んだかな。
  std::string JapaneseKana(int start, int end) const;

  // 区間 [start, end) を英語の区間として認めるか (IsEnglishSpan の移植)。
  bool IsEnglishSpan(std::string_view span, bool at_end, int before,
                     std::optional<bool> after, bool start_of_input, bool unreadable,
                     std::optional<char> next, bool ends_word) const;

  // 重い解析の前に落とす、英語の根拠がまるで無い区間 (CanBeEnglishSpan の移植)。
  bool CanBeEnglishSpan(int start, int end, bool growing, std::optional<char> next) const;

  // 知っている英単語か (同梱の辞書・固有名詞・4 文字以上なら綴りの一覧)。
  bool IsKnownEnglishWord(const std::string& lower) const;

  // 同梱の英単語・固有名詞にある語か。
  bool IsListedEnglishWord(const std::string& lower) const;

  // 小文字の語の末尾が大文字 1 文字 (holoX) なら true。
  bool IsTrailingCapitalName(std::string_view span, std::optional<char> next) const;

  // 知らない英字の語 + 助詞 (grokga = grok + が) か。
  bool EndsWithParticleAfterUnknownWord(const std::string& lower) const;

  // 英語の区間を 1 つ伸ばす (得点と、そこまでの英語の区間を更新する)。
  Path Extend(const Path& path, int start, int end) const;

  // 位置 p から始まる 3 文字の綴りが [子音]ha (しゃ・てゃ …) か。
  bool IsConsonantHaSyllable(int p) const;

  // 区切り方を、最後の英語の区間までの区間の並びにする。
  std::vector<Segment> BuildSegments(const Path& path) const;

  // 位置ごとに 1 音ぶんの情報を作っておく (Meltype の CompositionDetector.Prepare と同じ)。
  // 区間ごとに読み直すと O(n^3) になるので、ここで 1 回だけ計算する。
  void Prepare();

  std::string input_;
  const MeltypeDictionaries& dictionaries_;
  RomajiDetector romaji_;
  ScoredSegmenter::Options options_;
  const int n_;
  // 位置 i から始まる 1 音の長さ・かな・ローマ字として読めたか。
  std::vector<int> syllable_length_;
  std::vector<bool> syllable_readable_;
  std::string kana_text_;              // 位置ごとのかなを連結したもの
  std::vector<std::size_t> kana_offset_;  // kana_offset_[i] = 位置 i のかなの始まり
  std::vector<int> unreadable_prefix_;    // unreadable_prefix_[i] = 位置 i までに読めなかった文字数
  // 英単語の最後の子音と助詞の は が 1 音 (medals + ha = medal + sha) になっている位置。
  // そこだけを英単語 (sha) にすると「medalssha」になってしまうので、区間として認めない。
  std::vector<bool> split_particle_;
};

Context::Syllable Context::NextSyllable(int i) const {
  const unsigned char c = static_cast<unsigned char>(input_[static_cast<std::size_t>(i)]);
  const bool ascii_letter = IsAsciiLetter(static_cast<char>(c));
  if (c < 0x80 && !ascii_letter) {
    // ハイフンは日本語の長音記号「ー」として扱う (sa-ba -> さーば)。
    if (c == '-') {
      return {1, "ー", true};
    }
    // 空白・記号はそのまま 1 文字ぶん進む (読めない英字ではない)。
    return {1, std::string(Sub(i, i + 1)), true};
  }
  if (c >= 0x80) {
    // 既に確定している日本語などは、まとめてそのまま通す。
    const int length = static_cast<int>(Utf8SequenceLength(input_, static_cast<std::size_t>(i)));
    return {length, std::string(Sub(i, i + length)), true};
  }
  // 先頭の 1 音は、うしろをどれだけ読んでも変わらない (綴りは 4 文字まで、ん・っ の
  // 先読みは 2 文字)。なので窓で切って 1 音ぶんだけ解析する。
  constexpr int kWindow = 8;
  const RomajiDetector::Analysis analysis =
      romaji_.AnalyzeMidWord(Sub(i, std::min(n_, i + kWindow)));
  if (!analysis.tokens.empty()) {
    const RomajiDetector::Token& token = analysis.tokens.front();
    return {static_cast<int>(token.romaji.size()), token.kana, true};
  }
  if (analysis.is_valid && analysis.partial == "n") {
    // 末尾の「ん」だけが残っている (nihon → にほん)。
    return {1, "ん", true};
  }
  // 読めなかった英字。かなには打った文字をそのまま残す (Meltype の CompositionUnit と同じ)。
  return {1, std::string(Sub(i, i + 1)), false};
}

void Context::Prepare() {
  syllable_length_.assign(static_cast<std::size_t>(n_), 1);
  syllable_readable_.assign(static_cast<std::size_t>(n_), false);
  kana_offset_.assign(static_cast<std::size_t>(n_) + 1, 0);
  unreadable_prefix_.assign(static_cast<std::size_t>(n_) + 1, 0);

  // 1) 位置ごとの 1 音と、そのかな。日本語の区間は、その区間の始まりから読み直す
  // (英語の区間は音の境界で終わらないことがある: google の le のあとの d)。
  kana_text_.reserve(static_cast<std::size_t>(n_) * 3);
  for (int i = 0; i < n_; ++i) {
    const Syllable syllable = NextSyllable(i);
    const std::size_t at = static_cast<std::size_t>(i);
    syllable_length_[at] = syllable.length;
    syllable_readable_[at] = syllable.readable;
    kana_offset_[at] = kana_text_.size();
    kana_text_ += syllable.kana;
    unreadable_prefix_[at + 1] = unreadable_prefix_[at] + (syllable.readable ? 0 : 1);
  }
  kana_offset_[static_cast<std::size_t>(n_)] = kana_text_.size();

  // 3) 英単語 + は に分けられるところを探す (?ha の 1 音の前に、知っている英単語がある)。
  // 見に行くのは 16 文字ぶんまで (英単語の打ちかけの範囲)。
  split_particle_.assign(static_cast<std::size_t>(n_), false);
  for (int p = 0; p + 3 <= n_; ++p) {
    if (!IsConsonantHaSyllable(p)) continue;
    for (int k = p - 1; k >= 0 && k >= p - 16; --k) {
      if (!IsKnownEnglishWord(ToLowerAscii(Sub(k, p + 1)))) continue;
      // 日本語の語 (densha = でんしゃ) の中の しゃ は分けない。
      if (dictionaries_.japanese.ContainsWord(ToLowerAscii(Sub(k, p + 3)))) continue;
      split_particle_[static_cast<std::size_t>(p)] = true;
      break;
    }
  }
}

bool Context::HasUnreadable(int start, int end) const {
  return unreadable_prefix_[static_cast<std::size_t>(end)] >
         unreadable_prefix_[static_cast<std::size_t>(start)];
}

std::string Context::JapaneseKana(int start, int end) const {
  std::string kana;
  for (int i = start; i < end;) {
    const std::size_t at = static_cast<std::size_t>(i);
    kana.append(kana_text_, kana_offset_[at], kana_offset_[at + 1] - kana_offset_[at]);
    i += syllable_length_[at];
  }
  return kana;
}

bool Context::IsListedEnglishWord(const std::string& lower) const {
  return lower.size() >= 2 && dictionaries_.english.ContainsWord(lower);
}

bool Context::IsKnownEnglishWord(const std::string& lower) const {
  if (lower.size() < 3) return false;
  for (char c : lower) {
    if (!IsLowerAscii(c)) return false;
  }
  if (dictionaries_.english.ContainsWord(lower)) return true;
  // 4 文字以上なら、綴りの一覧 (スペルチェッカー代わり) も根拠にする。
  if (lower.size() >= 4 && dictionaries_.spell.ContainsWord(lower)) return true;
  // ローマ字として最後まで読めても日本語の語にならない語 (english-readable.txt)。
  if (lower.size() >= 4 && dictionaries_.readable.ContainsWord(lower)) return true;
  return IsPediaEnglishWord(lower);
}

bool Context::IsTrailingCapitalName(std::string_view span, std::optional<char> next) const {
  if (span.size() < 4 || !IsUpperAscii(span.back())) return false;
  // 大文字の後ろに英字が続く (English の E) ときは、途中の大文字として扱う。
  if (next && IsAsciiLetter(*next)) return false;
  const std::string_view stem = span.substr(0, span.size() - 1);
  if (stem.size() < 3) return false;
  for (char c : stem) {
    if (!IsLowerAscii(c)) return false;
  }
  // 小文字側がふつうの日本語 (kore) なら分けたまま。
  return !dictionaries_.japanese.ContainsWord(stem);
}

bool Context::EndsWithParticleAfterUnknownWord(const std::string& lower) const {
  // 同梱の辞書の語・固有名詞・読める語 (discode / opencode) は「知らない語 + 助詞」ではない。
  if (dictionaries_.english.ContainsWord(lower) || dictionaries_.proper.ContainsWord(lower) ||
      dictionaries_.readable.ContainsWord(lower) || dictionaries_.spell.ContainsWord(lower)) {
    return false;
  }
  for (std::string_view particle : kTrailingParticles) {
    if (!EndsWith(lower, particle)) continue;
    const std::string stem = lower.substr(0, lower.size() - particle.size());
    // 語の最後の子音と助詞の頭が同じ (lot + to = ろっと) なら っ の綴りなので除く。
    if (stem.size() < 3 || stem.back() == particle.front()) return false;
    return !romaji_.Analyze(stem).is_valid;
  }
  return false;
}

bool Context::IsConsonantHaSyllable(int p) const {
  if (p + 3 > n_) return false;
  const char first = input_[static_cast<std::size_t>(p)];
  if (!IsLowerAscii(first)) return false;
  for (char vowel : {'a', 'i', 'u', 'e', 'o', 'n'}) {
    if (first == vowel) return false;
  }
  if (input_[static_cast<std::size_t>(p) + 1] != 'h' ||
      input_[static_cast<std::size_t>(p) + 2] != 'a') {
    return false;
  }
  // 綴りの表にある 3 文字の綴り (?ha) であること (cha・sha・tha・dha・wha)。
  const RomajiDetector::Analysis analysis =
      romaji_.AnalyzeMidWord(SubFrom(p));
  return analysis.tokens.size() == 1 && analysis.tokens.front().romaji.size() == 3;
}

Path Context::Extend(const Path& path, int start, int end) const {
  const std::string_view raw = Sub(start, end);
  // すぐ前も英語の区間 (cup|sha、most|ring) なら、切り替えの減点の代わりに、
  // 語を 2 つに分けた減点 (AdjacentPenalty)。英字どうしがくっついたときだけ
  // (空白・記号をはさむ Let's Think wow... のは語の自然な区切り)。
  const bool adjacent =
      !path.english.empty() && path.english.back().second == start &&
      path.japanese_start == start && start > 0 &&
      IsAsciiLetter(input_[static_cast<std::size_t>(start) - 1]) && !raw.empty() &&
      IsAsciiLetter(raw.front());
  bool letters = !raw.empty();
  for (char c : raw) {
    if (!IsAsciiLetter(c)) {
      letters = false;
      break;
    }
  }
  const std::string lower = ToLowerAscii(raw);
  // 知っている英単語・決まり文句と、記号・数字の入った区間 (v1.2、node.js) は長さの 2 乗。
  // 辞書に無い英字の並び (buglowers) は長さに比例するだけ (前後の日本語や知っている語を
  // 巻き込まない)。
  const bool known = !letters || IsKnownEnglishWord(lower) || IsListedEnglishWord(lower);
  const double score =
      known ? kEnglishCharScore * std::pow(static_cast<double>(raw.size()), kLengthExponent)
            : kUnknownCharScore * static_cast<double>(raw.size());
  Path out = path;
  out.score += score - (adjacent ? kAdjacentPenalty : kSwitchPenalty);
  out.english.push_back({start, end});
  out.japanese_start = end;
  return out;
}

bool Context::CanBeEnglishSpan(int start, int end, bool growing,
                               std::optional<char> next) const {
  const std::size_t length = static_cast<std::size_t>(end - start);
  bool has_upper = false;
  bool has_apostrophe = false;
  bool has_non_letter = false;
  for (char c : Sub(start, end)) {
    if (IsUpperAscii(c)) has_upper = true;
    if (c == '\'') has_apostrophe = true;
    if (!IsAsciiLetter(c)) has_non_letter = true;
  }
  // 記号・空白の入る区間は英語にしない (英文の中の記号は別の区間として扱う)。
  if (has_non_letter) return false;
  if (length < 2) return true;
  // 大文字やアポストロフィが混じる区間は、IsEnglishSpan に任せる。
  if (has_upper || has_apostrophe) return true;
  if (length <= 6 && next && IsDigit(*next)) return true;
  const std::string lower = ToLowerAscii(Sub(start, end));
  if (dictionaries_.english.ContainsWord(lower) || dictionaries_.proper.ContainsWord(lower) ||
      dictionaries_.readable.ContainsWord(lower) || IsPediaEnglishWord(lower)) {
    return true;
  }
  if (growing &&
      (dictionaries_.english.HasPrefix(lower) || dictionaries_.proper.HasPrefix(lower))) {
    return true;
  }
  return dictionaries_.spell.ContainsWord(lower);
}

bool Context::IsEnglishSpan(std::string_view span, bool at_end, int before,
                            std::optional<bool> after, bool start_of_input, bool unreadable,
                            std::optional<char> next, bool ends_word) const {
  // まだ続きを打つかもしれない末尾の区間 (打ちかけの英単語を英語と見てよい)。
  const bool growing = at_end && !options_.final;
  if (span.empty()) return false;
  for (char c : span) {
    if (!IsAsciiLetter(c)) return false;
  }
  const std::string lower = ToLowerAscii(span);

  // 小文字の語の末尾が大文字 1 文字 (holoX → ほぉX になっていた)。
  if (IsTrailingCapitalName(span, next)) return true;
  // 小文字で始まって途中に大文字がある区間 (iPC、meteSNS) は、固有名詞の書き方
  // (iPhone・eBay・macOS) でなければ 1 語ではない。
  if (IsLowerAscii(span.front())) {
    bool has_upper = false;
    for (char c : span.substr(1)) {
      if (IsUpperAscii(c)) has_upper = true;
    }
    if (has_upper && !dictionaries_.proper.ContainsWord(lower)) return false;
  }
  // 促音の境界 (この区間の末尾の子音と、すぐ次の文字が同じ) は、英語の区間を
  // 日本語の促音「っ」に潰す結合をここで止める。英単語 (tab) の後ろに日本語
  // (botan) が続いても、tab + botan = たっぼたん にはしない。
  // ただし、重なりが日本語の語の続き (kitt + e = きって, bagg + u = ばっぐ のような
  // 日本語の促音) なら、日本語として読む。日本語の促音は有声子音 (b, d, g, z, j) でも
  // 生じる (バッグ・ベッド・グッズ) ので、子音の種類では判定しない。
  if (next && ToLowerAscii(*next) == lower.back() &&
      (dictionaries_.english.ContainsWord(lower) ||
       dictionaries_.proper.ContainsWord(lower)) &&
      !dictionaries_.japanese.HasPrefix(lower + ToLowerAscii(*next))) {
    return true;
  }
  // 短い区間の最後の子音が、次の音と合わせて っ になる (u|lo|t + ti = ぉっち) なら、
  // 英単語 (lot) ではなく日本語の途中。
  if (lower.size() <= 4 && next && ToLowerAscii(*next) == lower.back() &&
      lower.back() != 'a' && lower.back() != 'i' && lower.back() != 'u' &&
      lower.back() != 'e' && lower.back() != 'o' && lower.back() != 'n' &&
      romaji_.AnalyzeFragment(lower.substr(0, lower.size() - 1)).is_valid) {
    return false;
  }
  // 数字のすぐ前の、ローマ字として読めない子音で始まる短い英字 (kaibunsho|rta|2026): 略語。
  if (next && IsDigit(*next) && span.size() >= 2 && span.size() <= 6 &&
      !romaji_.AnalyzeFragment(lower.substr(0, 2)).is_valid) {
    return true;
  }
  // 日本語のすぐ後ろで、助詞 + 英単語 (dochira|mo|user の mouser) は 助詞 + 英単語。
  if (before < 0) {
    if (std::optional<std::string_view> leading = StartsWithParticle(lower)) {
      if (lower.size() - leading->size() >= 3 && !dictionaries_.english.ContainsWord(lower) &&
          IsKnownEnglishWord(lower.substr(leading->size()))) {
        return false;
      }
    }
  }

  const bool in_dictionary = dictionaries_.english.ContainsWord(lower);
  const bool known_spelling = dictionaries_.spell.ContainsWord(lower);
  const bool readable_english =
      lower.size() >= 3 && dictionaries_.readable.ContainsWord(lower);
  // 英語とする根拠がまるで無い区間では、かなの全解析を繰り返さない。
  if (lower.size() >= 2 && !IsUpperAscii(span.front()) && !in_dictionary && !known_spelling &&
      !readable_english && !IsPediaEnglishWord(lower) &&
      !(growing && (dictionaries_.english.HasPrefix(lower) ||
                    dictionaries_.proper.HasPrefix(lower)))) {
    return false;
  }

  // 英語の末尾 's' で、後ろに母音が続く場合 (discordsa-ba -> discord + sa-ba):
  if (lower.size() >= 3 && lower.back() == 's' && next) {
    const char n = ToLowerAscii(*next);
    if (n == 'a' || n == 'i' || n == 'u' || n == 'e' || n == 'o') {
      const std::string without_s = lower.substr(0, lower.size() - 1);
      if (IsKnownEnglishWord(without_s)) {
        return false;
      }
    }
  }

  const RomajiDetector::Analysis analysis = romaji_.Analyze(lower);
  const RomajiDetector::Analysis fragment = romaji_.AnalyzeFragment(lower);
  // 知らない英字の語 + 助詞 (grok|ga、figma|de) は、助詞までを 1 語にしない。
  if (EndsWithParticleAfterUnknownWord(lower)) return false;
  // 小書き文字の綴り (mala = まぁ, xtu = っ) で最後まで読める語は、日本語をわざわざ
  // 打っている。同梱の辞書の英単語以外は日本語 (6 文字以上の綴り一覧の語は除く)。
  const bool small_kana_spelling = !analysis.is_valid && fragment.is_valid &&
                                   fragment.partial.empty() &&
                                   !(lower.size() >= 6 && known_spelling);
  const bool spell_word = !in_dictionary && !small_kana_spelling && known_spelling;
  const bool exact = in_dictionary || (spell_word && !analysis.is_valid);
  const bool prefix = growing && lower.size() >= 4 && !small_kana_spelling &&
                      dictionaries_.english.HasPrefix(lower);

  // 大文字で始まる語 (Shift を押して打った) は固有名詞や英文。
  if (IsUpperAscii(span.front()) && (exact || prefix || at_end)) return true;
  // readable_english の単語でも、日本語として最後まで読める語 (rose=ろせ, tenai=てない等) は
  // 強制英語化せず、後段の文脈判定 (ambiguous) に回す。
  // ローマ字として最後まで読めない語 (analysis.is_valid が false) のみ即英語とする。
  if (readable_english && (!analysis.is_valid || !analysis.partial.empty())) return true;
  // 5 文字以上の英単語で、ローマ字としても読めるもの:
  // - c 行の綴り (camera、coffee) は英語。日本語を打つときは k を使う。
  // - ローマ字として読むと ぢ・づ になる綴り (radio = らぢお) で、ふつうの日本語の語に
  //   ならないなら英語。日本語の語 (suzuki) と同じ綴りなら除く。
  if (lower.size() >= 5 && (in_dictionary || known_spelling) &&
      !dictionaries_.japanese.HasPrefix(lower) && fragment.is_valid &&
      fragment.partial.empty()) {
    if (lower.find('c') != std::string::npos && lower.find("ch") == std::string::npos) {
      return true;
    }
    const std::string fragment_kana = fragment.Kana();
    if (fragment_kana.find("ぢ") != std::string::npos ||
        fragment_kana.find("づ") != std::string::npos) {
      return true;
    }
  }
  // readable_english の単語でも、日本語として読める語 (rose=ろせ, tenai=てない等) は
  // 強制英語化せず文脈判定に回す。
  if (lower.size() >= 3 && readable_english && (!analysis.is_valid || !analysis.partial.empty())) return true;
  // -pedia (pedia / protopedia …)。ローマ字だと ぺぢあ になり、途中の to が割れる。
  if (IsPediaEnglishWord(lower)) return true;

  if (!(start_of_input && at_end)) {
    // c 行の綴りで読める語 (care = かれ) が日本語の途中にあるなら、日本語を打っている
    // (fucarete → ふかれて)。入力全体がその語だけのときは英語。
    const bool english_then_particle =
        (in_dictionary || known_spelling) && lower.size() >= 4 && before >= 0 && next &&
        StartsWithParticle(std::string(1, ToLowerAscii(*next))).has_value();
    if (lower.find('c') != std::string::npos && english_then_particle) return true;
    if (lower.find('c') != std::string::npos) {
      const RomajiDetector::Analysis c_row = romaji_.Analyze(RomajiDetector::ReadCRow(lower));
      if (c_row.is_valid && (c_row.partial.empty() || c_row.partial == "n")) return false;
    }
    // v 行 (va = ゔぁ): 辞書の英単語 (video) でなければ日本語 (vanpaia → ゔぁんぱいあ)。
    if (lower.find('v') != std::string::npos && lower.find('l') == std::string::npos &&
        lower.find('x') == std::string::npos && !in_dictionary &&
        !(lower.size() >= 5 && known_spelling) && fragment.is_valid &&
        (fragment.partial.empty() || fragment.partial == "n")) {
      return false;
    }
  }

  // 1 文字は英文の中の a / i と、チャットの略し方の u (you)・r (are) だけ。
  // 前が英語の語なら英字。語として独立している (後ろが空白・記号・終わり) ときだけ。
  if (span.size() < 2) {
    if ((lower == "a" || lower == "i" || lower == "u" || lower == "r") && before >= 1 &&
        !(next && IsAsciiLetter(*next))) {
      return true;
    }
    return false;
  }

  // 英語の固有名詞 (amazon, adobe, netflix) は、ローマ字として読めても英語。
  // 日本語の語と同じ綴りなら除く。
  // ローマ字として最後まで読める固有名詞 (korea = これあ) の後ろに、助詞でない日本語が
  // 続くなら、日本語の語の途中 (korea|reka → これあれか)。
  if (lower.size() >= 4 && dictionaries_.proper.ContainsWord(lower) && next &&
      IsAsciiLetter(*next) && analysis.is_valid && analysis.partial.empty() &&
      !StartsWithParticle(std::string(1, ToLowerAscii(*next))).has_value()) {
    return false;
  }
  if (lower.size() >= 4 && dictionaries_.proper.ContainsWord(lower) &&
      !dictionaries_.japanese.ContainsWord(lower) &&
      ((at_end && before >= 0) || lower.size() >= 5 || !analysis.is_valid)) {
    return true;
  }
  if (growing && before >= 0 && lower.size() >= 4 && !small_kana_spelling &&
      dictionaries_.proper.HasPrefix(lower) && !dictionaries_.japanese.HasPrefix(lower)) {
    return true;
  }

  // 英単語で、ローマ字として読めない英字を含む (zoom + でかいぎ → m が読めない)。
  // 日本語の後ろで、助詞 + 読めない英字 (の + ts) は、英単語 (not) ではなく 助詞 + 英字。
  if (unreadable && before < 0) {
    if (std::optional<std::string_view> particle = StartsWithParticle(lower)) {
      if (lower.size() - particle->size() <= 2 &&
          romaji_.AnalyzeFragment(lower.substr(particle->size())).is_valid) {
        return false;
      }
    }
  }
  // 同梱の辞書の英単語で、ローマ字としては促音 (っ) を使わないと読めない語
  // (issue = いっすえ, apple) は英語。日本語の語 (の先頭) なら除く。
  if (in_dictionary && lower.size() >= 4 && analysis.is_valid && analysis.sokuon > 0 &&
      !dictionaries_.japanese.HasPrefix(lower)) {
    return true;
  }
  // 2 文字でも、同梱の辞書の語で読めない英字がある (ok + notasuku の k) なら英語。
  if (unreadable && (exact || spell_word) && (lower.size() >= 3 || in_dictionary)) return true;

  // 英語とも日本語とも読める語 (sushi, repo, make) は前後の両方で決める。前が英語なら
  // +1・日本語なら -1、後ろも同じように数え、合計が必要な点数に届けば英語
  // (どちらも分からない・食い違うときは日本語)。
  // 助詞などと同じ 2 文字の語 (no, to, ga) は、前後の両方が英語のときだけ。
  // 引用の「って」がそのまま続く英単語 (minuteっていう英語): 引用される語は
  // 前後の文脈が無くても英語として切り出す。日本語の綴り (きって・くって) は
  // japanese.txt の語の綴りなので (japanese.HasPrefix) ここで除かれる。
  if ((exact || spell_word) && !small_kana_spelling && lower.size() >= 4 &&
      !dictionaries_.japanese.HasPrefix(lower)) {
    const std::size_t begin =
        static_cast<std::size_t>(span.data() - input_.data());
    const std::size_t letters_end = begin + span.size();
    if (begin <= static_cast<std::size_t>(n_) &&
        letters_end + 2 < static_cast<std::size_t>(n_) &&
        input_[letters_end] == 't' && input_[letters_end + 1] == 't' &&
        input_[letters_end + 2] == 'e') {
      return true;
    }
  }

  const bool ambiguous =
      (start_of_input && ends_word)
          ? (exact || spell_word)
          : (start_of_input && at_end
                 ? (exact || spell_word || lower.size() == 1 ||
                    (!options_.final && !small_kana_spelling && dictionaries_.english.HasPrefix(lower)))
                 : ((exact || spell_word) && lower.size() >= 3));
  const int needed =
      (lower.size() <= 2 && analysis.is_valid &&
       (analysis.partial.empty() || analysis.partial == "n"))
          ? 2
          : 1;
  // 綴りの一覧だけが知っている、最後までローマ字として読める語 (shite, kore) は、
  // 前の英単語 1 つ (push) では足りない (pushshite → pushして)。
  int required = needed;
  if (spell_word && !in_dictionary && analysis.is_valid && analysis.partial.empty()) {
    required = std::max(required, 2);
  }
  // 英単語の末尾と、この区間の頭が同じ文字 (tab|botan の bb) なら、それは
  // 日本語の促音の綴り (っ) であって英語 2 語の並びではない。前が英語でも
  // 文脈として数えない (tab が botan を英語に引き込むのを防ぐ)。
  const std::size_t span_begin =
      static_cast<std::size_t>(span.data() - input_.data());
  const bool doubled_letter_junction =
      span_begin > 0 &&
      ToLowerAscii(input_[span_begin - 1]) == ToLowerAscii(span.front());
  const int before_context = doubled_letter_junction ? 0 : before;
  // 前が英文でも、後ろが日本語なら英文の強さは数えない (I love |sushi| が好き)。
  if (ambiguous &&
      (after.has_value() && !*after ? std::min(before_context, 1) : before_context) +
              Score(after) >=
          required) {
    return true;
  }
  if (!exact && !prefix && !spell_word) return false;

  if (!analysis.is_valid) {
    if (!exact && !prefix) return false;
    // 日本語のすぐ後ろの短い英単語 (thin) が、変換ボックスの綴り (thi = てぃ) では最後まで
    // 読めて、続き (gu) とも読めるなら、外来語のカタカナを打っている途中。
    if (before < 0 && !at_end && lower.size() <= 4 && next && IsAsciiLetter(*next) &&
        fragment.is_valid && (fragment.partial.empty() || fragment.partial == "n")) {
      const RomajiDetector::Analysis together =
          romaji_.AnalyzeFragment(lower + ToLowerAscii(*next));
      if (together.is_valid) return false;
    }
    if (!exact && small_kana_spelling) return false;
    // 日本語のすぐ後ろの 2 文字の語で、変換ボックスでは読める綴り (こ + we = こうぇ) は日本語。
    if (small_kana_spelling && lower.size() <= 2 && before < 0) return false;
    // ローマ字として読めない英単語。途中の区間は 3 文字以上だけ (短い語の偶然の一致を避ける)。
    // (Meltype の `atEnd && (exact || !conservative)`。conservative でないので atEnd で足りる)
    return at_end || span.size() >= 3;
  }
  // ローマ字として読めても、末尾が子音の英単語 (git, zoom, about) で、日本語の語の
  // 途中でもないなら英語。打ち終わっていれば、日本語の語の打ちかけ (it → itai) かどうかは
  // 気にしなくてよい。
  const bool not_japanese_prefix =
      options_.final || !dictionaries_.japanese.HasPrefix(lower);
  if (spell_word && at_end && before >= 0 && !analysis.partial.empty() &&
      analysis.partial != "n" && not_japanese_prefix) {
    return true;
  }
  return at_end && exact && analysis.partial.size() > 0 && analysis.partial != "n" &&
         not_japanese_prefix;
}

std::vector<Segment> Context::BuildSegments(const Path& path) const {
  std::vector<Segment> segments;
  int position = 0;
  for (const std::pair<int, int>& span : path.english) {
    if (span.first > position) {
      Segment japanese;
      japanese.is_english = false;
      japanese.raw = std::string(Sub(position, span.first));
      japanese.kana = JapaneseKana(position, span.first);
      segments.push_back(std::move(japanese));
    }
    Segment english;
    english.is_english = true;
    english.raw = std::string(Sub(span.first, span.second));
    // ローマ字として最後まで読める英単語 (fine, take) は日本語としても成立する。
    const std::string lower = ToLowerAscii(english.raw);
    const RomajiDetector::Analysis analysis = romaji_.Analyze(lower);
    english.ambiguous = analysis.is_valid && analysis.partial.empty() &&
                        IsKnownEnglishWord(lower);
    segments.push_back(std::move(english));
    position = span.second;
  }
  if (position < n_) {
    Segment japanese;
    japanese.is_english = false;
    japanese.raw = SubFrom(position);
    japanese.kana = JapaneseKana(position, n_);
    segments.push_back(std::move(japanese));
  }

  // 続いた日本語の区間を 1 つに、空白をはさんだ英語 (hello world) も 1 つにまとめる。
  std::vector<Segment> merged;
  merged.reserve(segments.size());
  for (Segment& segment : segments) {
    if (!merged.empty() && merged.back().is_english == segment.is_english) {
      merged.back().raw += segment.raw;
      merged.back().kana += segment.kana;
      merged.back().ambiguous = merged.back().ambiguous || segment.ambiguous;
      continue;
    }
    merged.push_back(std::move(segment));
  }
  return merged;
}

std::vector<Segment> Context::Run() {
  if (n_ == 0) return {};
  // best[i][e]: 位置 i まで区切ったときの一番良い区切り方。
  // e は「直前の区間が i で終わる英語の区間か」。
  std::vector<std::array<std::optional<Path>, 2>> best(static_cast<std::size_t>(n_) + 1);
  best[0][0] = Path{};

  auto offer = [&best, this](int position, bool english, Path path) {
    if (position < 0 || position > n_) {
      return;
    }
    const int e = english ? 1 : 0;
    // 同点なら先に見つけたもの (前から始まる区間) を残す。
    if (best[static_cast<std::size_t>(position)][static_cast<std::size_t>(e)].has_value() &&
        best[static_cast<std::size_t>(position)][static_cast<std::size_t>(e)]->score >=
            path.score) {
      return;
    }
    best[static_cast<std::size_t>(position)][static_cast<std::size_t>(e)] = std::move(path);
  };

  // 位置 j から後ろが記号だけか (末尾の語を「語がそこで終わっている」と見るため)。
  std::vector<bool> symbol_suffix(static_cast<std::size_t>(n_) + 1, false);
  symbol_suffix[static_cast<std::size_t>(n_)] = true;
  for (int i = n_ - 1; i >= 0; --i) {
    symbol_suffix[static_cast<std::size_t>(i)] =
        symbol_suffix[static_cast<std::size_t>(i) + 1] &&
        IsAsciiSymbol(input_[static_cast<std::size_t>(i)]);
  }

  for (int i = 0; i < n_; ++i) {
    for (int e = 0; e < 2; ++e) {
      const std::optional<Path>& current =
          best[static_cast<std::size_t>(i)][static_cast<std::size_t>(e)];
      if (!current.has_value()) continue;
      const Path& path = *current;
      // 区間の前が英語か: 先頭なら入力欄の確定済みの文字、途中なら直前の区間。
      const std::optional<bool> preceded =
          (i == 0) ? options_.preceding_english : std::optional<bool>(e == 1);
      const bool preceded_by_english = preceded.value_or(false);
      const int before =
          (i == 0 && options_.english_sentence) ? 2 : Score(preceded);

      // 英文の中の記号 (, . ! ? -) は半角のまま。日本語の文の中の英単語の後ろは
      // 日本語の記号にするので、日本語が前にあれば記号は日本語側に付く。
      // (ここまで英語の区間だけで敷き詰まっているときだけ英文の記号として扱う)
      if (IsEnglishSymbol(input_[static_cast<std::size_t>(i)]) && preceded_by_english &&
          !path.english.empty() && path.english.front().first == 0 &&
          path.english.back().second == i) {
        offer(i + 1, true, Extend(path, i, i + 1));
        continue;
      }

      // 日本語として 1 音進む。読めない英字があれば減点。
      const int length = syllable_length_[static_cast<std::size_t>(i)];
      Path japanese_path = path;
      if (!syllable_readable_[static_cast<std::size_t>(i)]) {
        japanese_path.score -= kUnreadablePenalty;
      }
      offer(i + length, false, std::move(japanese_path));

      // 英語の区間として i から j まで取る (IsEnglishSpan が英語と言える区間だけ)。
      for (int j = n_; j > i; --j) {
        // 英単語 + は に分かれるところの 1 音 (medals の後の sha) は、それだけで
        // 英単語にしない (medalssha になってしまう)。
        if (j == i + 3 && split_particle_[static_cast<std::size_t>(i)]) continue;
        const bool at_end = (j == n_);
        const bool symbols_after =
            (i == 0 && j < n_ && symbol_suffix[static_cast<std::size_t>(j)]);
        const std::optional<bool> after =
            (j == n_ || symbols_after) ? options_.following_english
                                       : std::optional<bool>(false);
        const std::optional<char> next =
            (j < n_) ? std::optional<char>(input_[static_cast<std::size_t>(j)])
                     : std::nullopt;
        if (!CanBeEnglishSpan(i, j, at_end && !options_.final, next)) continue;
        // 英単語のすぐ後ろの する の活用 (push + site = して) は日本語。
        if (preceded_by_english && IsSuruForm(JapaneseKana(i, j))) continue;
        // 英単語のすぐ後ろの、たまたま実在の英単語と同じ綴りの日本語の
        // 助詞・形式名詞 (slack + made = まで) は日本語。英語同士の
        // 2 乗スコアでの合体を防ぐ (slackmade が全体英語に化けるのを防ぐ)。
        if (preceded_by_english && IsJapaneseNominalTail(Sub(i, j))) continue;
        // 英語の末尾 's' を日本語側に移すとすると活用の頭になるなら、その 's' は
        // 日本語の一部 (stopshita → stop + した。stops + ひた ではない)。
        // 辞書にある 's' 終わりの短い英単語 (stops) が長さの 2 乗で勝つのを防ぐ。
        if (j - i >= 3 && input_[static_cast<std::size_t>(j) - 1] == 's' &&
            IsSuruForm(JapaneseKana(j - 1, n_))) {
          continue;
        }
        if (!IsEnglishSpan(Sub(i, j),
                           at_end, before, after, /*start_of_input=*/i == 0,
                           /*unreadable=*/HasUnreadable(i, j), next,
                           /*ends_word=*/symbols_after)) {
          continue;
        }
        offer(j, true, Extend(path, i, j));
      }

      // 単位の途中で区切る: 英単語の最後の子音と、続く日本語の頭が 1 音になっているとき
      // (medals + ha が medal + sha (しゃ) になる)。1 文字目までを英語、残り (ha =
      // 助詞の は) を日本語にする。
      for (int p = n_ - 3; p > i; --p) {
        if (!IsConsonantHaSyllable(p)) continue;
        // その 1 音を含む日本語の語 (densha = でんしゃ) があれば区切らない。
        if (dictionaries_.japanese.ContainsWord(
                ToLowerAscii(Sub(i, p + 3)))) {
          continue;
        }
        if (preceded_by_english && IsSuruForm(JapaneseKana(i, p))) continue;
        const std::string_view word = Sub(i, p + 1);
        if (!IsKnownEnglishWord(ToLowerAscii(word))) continue;
        const char rest = input_[static_cast<std::size_t>(p) + 1];
        if (!IsEnglishSpan(word, /*at_end=*/false, before, /*after=*/false,
                           /*start_of_input=*/i == 0, /*unreadable=*/true, rest,
                           /*ends_word=*/false)) {
          continue;
        }
        offer(p + 1, false, Extend(path, i, p + 1));
      }
    }
  }

  // 末尾まで英語で終わる区切りと、日本語で終わる区切りのうち、点の高いほう。
  const std::optional<Path>& english =
      best[static_cast<std::size_t>(n_)][1];
  const std::optional<Path>& japanese =
      best[static_cast<std::size_t>(n_)][0];
  const std::optional<Path>* chosen = nullptr;
  if (english.has_value() &&
      (!japanese.has_value() || english->score >= japanese->score)) {
    chosen = &english;
  } else if (japanese.has_value()) {
    chosen = &japanese;
  } else {
    return {};
  }
  return BuildSegments(**chosen);
}

std::size_t MeltypeDictionaries::LoadFromDirectory(const std::string& directory,
                                                   MeltypeDictionaries& out) {
  auto join = [&directory](const char* name) -> std::string {
    if (directory.empty()) return std::string(name);
    const char last = directory.back();
    if (last == '/' || last == '\\') return directory + name;
    return directory + "/" + name;
  };
  auto read = [](const std::string& path) -> std::string {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
  };

  std::size_t total = 0;
  // 固有名詞は大文字小文字をそのまま持つので、小文字に直したものも登録する。
  // 同梱の英単語の辞書 (english) は、固有名詞もまとめて持つ (Meltype と同じ)。
  const std::string proper_nouns = read(join("propernouns.txt"));
  if (!proper_nouns.empty()) {
    total += out.proper.AddTextLowercased(proper_nouns);
    total += out.english.AddTextLowercased(proper_nouns);
  }
  total += out.english.AddFile(join("english.txt"));
  total += out.readable.AddFile(join("english-readable.txt"));
  total += out.spell.AddFile(join("english-words.txt"));
  // 日本語の語は、綴りの揺れ (si/shi, tu/tsu, ん の n/nn …) を展開して登録する。
  const RomajiDetector romaji;
  const std::string japanese = read(join("japanese.txt"));
  if (!japanese.empty()) {
    WordList scratch;
    scratch.AddText(japanese);
    for (const std::string& word : scratch.Words()) {
      for (const std::string& variant : romaji.SpellingVariants(word)) {
        if (out.japanese.AddWord(variant)) ++total;
      }
    }
  }
  return total;
}

std::vector<Segment> ScoredSegmenter::SegmentInput(std::string_view input,
                                                   const Options& options) const {
  Context context(input, dictionaries_, options);
  return context.Run();
}

}  // namespace meltype
}  // namespace mozkey
