// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The Mozkey Authors
//
// Meltype (GPL-3.0-or-later, src/Meltype.Core/Composition/ScoredSegmentation.cs と
// CompositionDetector.IsEnglishSpan) から移植した、ローマ字入力の英日自動分割。
//
// 考え方は Meltype と同じで、前半が English か Japanese かの「決まり」を port した
// IsEnglishSpan で決め、どこで区切るかを動的計画法 (全体の点が一番高い区切り方) で選ぶ。
// 先頭から見て最初に英語と言える区間を取ると戻れない (some|teal|coholic) ので、
// あり得る区切り方を全部並べて点数で選ぶ。

#ifndef MOZKEY_MELTYPE_SCORED_SEGMENTER_H_
#define MOZKEY_MELTYPE_SCORED_SEGMENTER_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "meltype/romaji_detector.h"
#include "meltype/word_list.h"

namespace mozkey {
namespace meltype {

// 英日混在の判定に使う辞書。Meltype の dictionaries/*.txt をそのまま読める。
struct MeltypeDictionaries {
  // 英語として扱う語 (english.txt + propernouns.txt)。日本語としても読める語
  // (sushi, repo …) も入っていて、区間として英語にするかは前後の文脈で決める。
  WordList english;
  // 英語の固有名詞 (propernouns.txt)。小文字に直して登録する。
  // ローマ字として読めても英語として扱う規則 (amazon → あまぞん) はこれだけを見る。
  WordList proper;
  // ローマ字として最後まで読めても日本語の語にならない語 (english-readable.txt)。
  WordList readable;
  // 綴りチェッカー代わりの英単語の一覧 (english-words.txt)。根拠としては弱い
  // (ローマ字として読める語を含むので、前後の文脈か読めない英字が必要)。
  WordList spell;
  // 日本語の語 (japanese.txt)。綴りの揺れ (si/shi, tu/tsu …) を展開して登録する。
  WordList japanese;

  // 指定のディレクトリから読む。無いファイルは無視するので、必要な分だけ置けばよい。
  // 戻り値は読めた語数の合計。
  static std::size_t LoadFromDirectory(const std::string& directory,
                                       MeltypeDictionaries& out);
};

// True when |token| is an identifier-like IT token that Japanese users type as
// one unit even though it is not made of letters only: S3, N+1, ES6, C++, C#,
// and dotted product or file names (node.js, vue.js, file.exe, setup.bat).
//
// The digit and symbol shapes end with a digit or a symbol, which makes them
// self-delimiting: a longer span that swallows the Japanese letters after the
// token ("S3b", "C++g") no longer matches.  A dotted name is delimited by the
// closed list of known suffixes instead.  Either way the English piece ends
// exactly where the token does and the rest is left to the Japanese side.
bool IsIdentifierToken(std::string_view token);

class ScoredSegmenter {
 public:
  // 区間分けの結果の 1 つ。
  struct Segment {
    bool is_english = false;
    // ローマ字として最後まで読める英単語 (fine, take …)。日本語としても成立するので、
    // 英語にしたのは前後の文脈で決めたもの。
    bool ambiguous = false;
    std::string raw;   // 打った文字そのまま
    std::string kana;  // かな (英語の区間は空)
  };

  struct Options {
    // キャレットの前が英文 (空白で区切った英単語が続いて空白で終わる) なら true。
    bool english_sentence = false;
    // 直前の文字が英語か (nullopt = 分からない)。
    std::optional<bool> preceding_english;
    // 直後の文字が英語か (nullopt = 分からない)。
    std::optional<bool> following_english;
    // 打ち終わった (Space・Enter)。false なら末尾は打ちかけの英単語と見る。
    bool final = true;
  };

  explicit ScoredSegmenter(const MeltypeDictionaries& dictionaries)
      : dictionaries_(dictionaries) {}

  // 入力のローマ字を、英語と日本語の区間に分ける。
  // (メソッド名は型の Segment と衝突しないように SegmentInput にしてある)
  std::vector<Segment> SegmentInput(std::string_view input, const Options& options) const;

 private:
  const MeltypeDictionaries& dictionaries_;
};

}  // namespace meltype
}  // namespace mozkey

#endif  // MOZKEY_MELTYPE_SCORED_SEGMENTER_H_
