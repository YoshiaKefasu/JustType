// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The Mozkey Authors
//
// Meltype (GPL-3.0-or-later, src/Meltype.Core/Detection/RomajiDetector.cs) から移植。
// 「この綴りはローマ字として成立するか」「かなに直すと何か」を答える。
// ここで分かるのは「ローマ字として成立するか」と日本語らしい特徴だけで、成立しても
// 英語の可能性は残る (kana, sushi, radio)。最終判断は ScoredSegmenter が行う。

#ifndef MOZKEY_MELTYPE_ROMAJI_DETECTOR_H_
#define MOZKEY_MELTYPE_ROMAJI_DETECTOR_H_

#include <string>
#include <string_view>
#include <vector>

namespace mozkey {
namespace meltype {

class RomajiDetector {
 public:
  // ローマ字 1 音。
  struct Token {
    std::string_view romaji;
    std::string kana;
  };

  // 解析結果。入力途中 (prefix) を前提にしている。
  struct Analysis {
    bool is_valid = false;
    std::vector<Token> tokens;
    std::string partial;  // 音になりきっていない末尾 (k, ky, n …)
    // 促音 (っ) の数。英単語かどうかの根拠に使う (issue = いっすえ)。
    int sokuon = 0;

    // tokens のかなを連結したもの。
    std::string Kana() const;
  };

  RomajiDetector() = default;

  // 語頭としての解析。語頭の「ん」「っ」は成立しない。
  Analysis Analyze(std::string_view letters) const;

  // 語の途中としての解析 (語頭の「ん」「っ」も許す)。小書きかなの綴り
  // (xa = ぁ, xtu = っ) も読む。l / x / v を読めるようにすると hello や live まで
  // ローマ字として成立してしまうので、英語かどうかの判定には使わない。
  Analysis AnalyzeFragment(std::string_view letters) const;

  // 語の途中としての解析。小書きかなの綴りは読まない (AnalyzeFragment との違い)。
  // 区間分けで「1 音ぶん進む」ときに使う。
  Analysis AnalyzeMidWord(std::string_view letters) const;

  // partial が "n" だけの語を、ん まで読んで解析し直す (nihon → にほん)。
  Analysis AnalyzeWord(std::string_view word) const;

  // c 行 (ca / cu / co = か く こ) を k 行に読み替える (ch はそのまま)。
  // fucarete を fukarete として調べるときに使う。
  static std::string ReadCRow(std::string_view letters);

  // かなの列から、あり得る綴りの組合せを列挙する (日本語辞書の綴り揺れの展開用)。
  std::vector<std::string> SpellingVariants(std::string_view canonical, int limit = 64) const;
};

}  // namespace meltype
}  // namespace mozkey

#endif  // MOZKEY_MELTYPE_ROMAJI_DETECTOR_H_
