// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The Mozkey Authors

#include "meltype/romaji_detector.h"

#include <algorithm>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mozkey {
namespace meltype {
namespace {

// かな → 綴り (先頭が標準の綴り)。spellings はスペース区切り。
// l / x / v 系は英語と区別できないので意図的に含めない (hello や live まで
// ローマ字として成立してしまう)。
struct KanaSpellings {
  std::string_view kana;
  std::string_view spellings;
};

constexpr KanaSpellings kStrictTable[] = {
    {"あ", "a"},
    {"い", "i yi"},
    {"う", "u whu wu"},
    {"え", "e"},
    {"お", "o"},
    {"か", "ka"},
    {"き", "ki"},
    {"く", "ku"},
    {"け", "ke"},
    {"こ", "ko"},
    {"きゃ", "kya"},
    {"きゅ", "kyu"},
    {"きょ", "kyo"},
    {"さ", "sa"},
    {"し", "shi si"},
    {"す", "su"},
    {"せ", "se ce"},
    {"そ", "so"},
    {"しゃ", "sha sya"},
    {"しゅ", "shu syu"},
    {"しょ", "sho syo"},
    {"しぇ", "she sye"},
    {"た", "ta"},
    {"ち", "chi ti"},
    {"つ", "tsu tu"},
    {"て", "te"},
    {"と", "to"},
    {"ちゃ", "cha tya cya"},
    {"ちゅ", "chu tyu cyu"},
    {"ちょ", "cho tyo cyo"},
    {"ちぇ", "che tye cye"},
    {"てゃ", "tha"},
    {"てゅ", "thu"},
    {"てょ", "tho"},
    {"な", "na"},
    {"に", "ni"},
    {"ぬ", "nu"},
    {"ね", "ne"},
    {"の", "no"},
    {"にゃ", "nya"},
    {"にゅ", "nyu"},
    {"にょ", "nyo"},
    {"にぃ", "nyi"},
    {"にぇ", "nye"},
    {"は", "ha"},
    {"ひ", "hi"},
    {"ふ", "fu hu"},
    {"へ", "he"},
    {"ほ", "ho"},
    {"ひゃ", "hya"},
    {"ひゅ", "hyu"},
    {"ひょ", "hyo"},
    {"ふぁ", "fa"},
    {"ふぃ", "fi"},
    {"ふぇ", "fe"},
    {"ふぉ", "fo"},
    {"ま", "ma"},
    {"み", "mi"},
    {"む", "mu"},
    {"め", "me"},
    {"も", "mo"},
    {"みゃ", "mya"},
    {"みゅ", "myu"},
    {"みょ", "myo"},
    {"や", "ya"},
    {"ゆ", "yu"},
    {"よ", "yo"},
    {"ら", "ra"},
    {"り", "ri"},
    {"る", "ru"},
    {"れ", "re"},
    {"ろ", "ro"},
    {"りゃ", "rya"},
    {"りゅ", "ryu"},
    {"りょ", "ryo"},
    {"わ", "wa"},
    {"を", "wo"},
    {"うぉ", "who"},
    // Microsoft IME の wh の打ち方 (whu = う、wha = うぁ)。
    {"うぁ", "wha"},
    {"が", "ga"},
    {"ぎ", "gi"},
    {"ぐ", "gu"},
    {"げ", "ge"},
    {"ご", "go"},
    {"ぎゃ", "gya"},
    {"ぎゅ", "gyu"},
    {"ぎょ", "gyo"},
    {"ざ", "za"},
    {"じ", "ji zi"},
    {"ず", "zu"},
    {"ぜ", "ze"},
    {"ぞ", "zo"},
    {"じゃ", "ja jya zya"},
    {"じゅ", "ju jyu zyu"},
    {"じょ", "jo jyo zyo"},
    {"じぇ", "je jye zye"},
    {"だ", "da"},
    {"ぢ", "di"},
    {"づ", "du"},
    {"で", "de"},
    {"ど", "do"},
    {"でゃ", "dha"},
    {"でゅ", "dhu"},
    {"でょ", "dho"},
    {"ぢゃ", "dya"},
    {"ぢゅ", "dyu"},
    {"ぢょ", "dyo"},
    {"ば", "ba"},
    {"び", "bi"},
    {"ぶ", "bu"},
    {"べ", "be"},
    {"ぼ", "bo"},
    {"びゃ", "bya"},
    {"びゅ", "byu"},
    {"びょ", "byo"},
    {"ぱ", "pa"},
    {"ぴ", "pi"},
    {"ぷ", "pu"},
    {"ぺ", "pe"},
    {"ぽ", "po"},
    {"ぴゃ", "pya"},
    {"ぴゅ", "pyu"},
    {"ぴょ", "pyo"},
    {"とぁ", "twa"},
    {"とぃ", "twi"},
    {"とぇ", "twe"},
    {"とぅ", "twu"},
    {"どぁ", "dwa"},
    {"どぃ", "dwi"},
    {"どぇ", "dwe"},
    {"どぅ", "dwu"},
    {"ー", "-"},
    // 小書き文字の x 系 (ぁ ぃ ぅ ぇ ぉ ゃ ゅ ょ っ ゎ ゕ ゖ)。x + 母音の綴りは
    // 英単語にも現れる (axe, taxi) が、x はローマ字の普通の綴りではないので、
    // 読めたなら日本語を打っている (aidexianara -> あいでぃあなら)。
    // ここに xn (ん) は入れない: 入れると ax + ni の xn が「ん」として読めて
    // しまい、末尾 x の英単語 + 助詞 (axni) が「あんい」に化ける。xn は
    // 変換ボックスの綴り (kCompositionTable) だけに置く。
    // l 系 (la, li …) もここに入れない: li を読めると like が「有効なローマ字」に
    // なり、IsEnglishSpan の英語確定の経路 (!analysis.is_valid) を通らなくなる
    // (like -> ぃけ, life -> ぃふぇ になる)。英語かどうかの判定に小書きかなを
    // 混ぜない、がこの表の決まり。
    {"ぁ", "xa"},
    {"ぃ", "xi"},
    {"ぅ", "xu"},
    {"ぇ", "xe"},
    {"ぉ", "xo"},
    {"ゃ", "xya"},
    {"ゅ", "xyu"},
    {"ょ", "xyo"},
    {"っ", "xtu xtsu"},
    {"ゎ", "xwa"},
    {"ゕ", "xka"},
    {"ゖ", "xke"},
};

// 変換ボックスでだけ使う綴り (小書き文字・外来音)。英語かどうかの判定には使わない。
constexpr KanaSpellings kCompositionTable[] = {
    {"ぁ", "xa la"},
    {"ぃ", "xi li xyi lyi"},
    {"ぅ", "xu lu"},
    {"ぇ", "xe le xye lye"},
    {"ぉ", "xo lo"},
    {"ゃ", "xya lya"},
    {"ゅ", "xyu lyu"},
    {"ょ", "xyo lyo"},
    {"っ", "xtu ltu xtsu ltsu"},
    {"ゎ", "xwa lwa"},
    {"ゕ", "xka lka"},
    {"ゖ", "xke lke"},
    {"ん", "xn"},
    {"ゔぁ", "va"},
    {"ゔぃ", "vi"},
    {"ゔ", "vu"},
    {"ゔぇ", "ve"},
    {"ゔぉ", "vo"},
    {"いぇ", "ye"},
    {"うぃ", "wi whi"},
    {"うぇ", "we whe"},
    // 歴史的仮名 (Microsoft IME と同じ綴り)。
    {"ゐ", "wyi"},
    {"ゑ", "wye"},
    {"てぃ", "thi"},
    {"でぃ", "dhi"},
    {"てゅ", "thu"},
    {"でゅ", "dhu"},
    {"とぅ", "twu"},
    {"どぅ", "dwu"},
    {"てゃ", "tha"},
    {"てぇ", "the"},
    {"てょ", "tho"},
    {"でゃ", "dha"},
    {"でぇ", "dhe"},
    {"でょ", "dho"},
    {"にぃ", "nyi"},
    {"にぇ", "nye"},
    {"とぁ", "twa"},
    {"とぃ", "twi"},
    {"とぇ", "twe"},
    {"くぃ", "qi"},
    {"くぇ", "qe"},
    {"くぉ", "qo"},
    // c 行 (Microsoft IME と同じ。cake や code まで日本語として読めてしまうので
    // 判定には使わない)。
    {"か", "ca"},
    {"し", "ci"},
    {"く", "cu"},
    {"こ", "co"},
    {"ちぃ", "cyi tyi"},
    {"くぁ", "kwa"},
    {"ぐぁ", "gwa"},
    {"つぁ", "tsa"},
    {"つぃ", "tsi"},
    {"つぇ", "tse"},
    {"つぉ", "tso"},
    {"ー", "-"},
};

bool IsVowel(char c) { return c == 'a' || c == 'i' || c == 'u' || c == 'e' || c == 'o'; }

bool IsConsonant(char c) { return c >= 'a' && c <= 'z' && !IsVowel(c); }

std::vector<std::string_view> SplitSpaces(std::string_view text) {
  std::vector<std::string_view> out;
  std::size_t begin = 0;
  while (begin < text.size()) {
    std::size_t end = text.find(' ', begin);
    if (end == std::string_view::npos) end = text.size();
    if (end > begin) out.push_back(text.substr(begin, end - begin));
    if (end == text.size()) break;
    begin = end + 1;
  }
  return out;
}

using SpellingMap = std::unordered_map<std::string_view, std::string_view>;
using SpellingSet = std::unordered_set<std::string_view>;

SpellingMap BuildSpellingMap(bool composition) {
  SpellingMap map;
  for (const KanaSpellings& entry : kStrictTable) {
    for (std::string_view spelling : SplitSpaces(entry.spellings)) {
      map.emplace(spelling, entry.kana);
    }
  }
  if (!composition) return map;
  // C# の BuildSpellingMap([.. Table, .. CompositionTable]) と同じく後ろが優先。
  for (const KanaSpellings& entry : kCompositionTable) {
    for (std::string_view spelling : SplitSpaces(entry.spellings)) {
      map.insert_or_assign(spelling, entry.kana);
    }
  }
  return map;
}

const SpellingMap& StrictSpellingMap() {
  static const SpellingMap map = BuildSpellingMap(/*composition=*/false);
  return map;
}

const SpellingMap& CompositionSpellingMap() {
  static const SpellingMap map = BuildSpellingMap(/*composition=*/true);
  return map;
}

const SpellingMap& KanaToSpellings() {
  static const SpellingMap map = [] {
    SpellingMap built;
    for (const KanaSpellings& entry : kStrictTable) built.emplace(entry.kana, entry.spellings);
    return built;
  }();
  return map;
}

SpellingSet BuildPartials(bool composition) {
  SpellingSet set;
  // strict では x 系の綴りの途中 (x, xy, xt, xts, xw, xk) を partial に入れない。
  // 入れると box / index の末尾 x が「綴りの途中」として valid のままになり、
  // 続く助詞 (boxni) が xn (ん) + i と読めてしまう (ぼんい)。x は小書きかなの
  // 綴りの頭でしかなく、英単語の末尾に現れる x とは別物なので、strict の
  // 「読めた」の根拠にはしない。composition (変換ボックスの読み) では従来通り
  // x を含める (x を打った途中で「読める」必要があるため)。
  auto add = [&set](const KanaSpellings& entry, bool skip_x) {
    for (std::string_view spelling : SplitSpaces(entry.spellings)) {
      if (skip_x && !spelling.empty() && spelling[0] == 'x') continue;
      for (std::size_t i = 1; i < spelling.size(); ++i) set.insert(spelling.substr(0, i));
    }
  };
  for (const KanaSpellings& entry : kStrictTable) add(entry, /*skip_x=*/!composition);
  if (composition) {
    for (const KanaSpellings& entry : kCompositionTable) add(entry, /*skip_x=*/false);
  }
  return set;
}

const SpellingSet& StrictPartials() {
  static const SpellingSet set = BuildPartials(/*composition=*/false);
  return set;
}

const SpellingSet& CompositionPartials() {
  static const SpellingSet set = BuildPartials(/*composition=*/true);
  return set;
}

// letters を解析する。
// absorb_trailing_n: 末尾に「n」が 1 文字だけ残っていたら、ん として読み切る
// (nihon → にほん)。呼び出し側で文字列をつなぎ直さずに済むようにするためのフラグで、
// Token::romaji は letters を指す string_view なので、元のバッファのまま解析したい。
RomajiDetector::Analysis AnalyzeCore(std::string_view letters, bool strict_start,
                                     bool composition, bool absorb_trailing_n = false) {
  RomajiDetector::Analysis result;
  const SpellingMap& spellings = composition ? CompositionSpellingMap() : StrictSpellingMap();
  const SpellingSet& partials = composition ? CompositionPartials() : StrictPartials();
  std::size_t i = 0;

  auto invalid = [&result]() {
    result.is_valid = false;
    result.partial.clear();
    return result;
  };

  while (i < letters.size()) {
    const char c = letters[i];
    if ((c < 'a' || c > 'z') && c != '-') return invalid();

    // ん: 次が n か子音 (y を除く) なら「ん」。
    if (c == 'n' && i + 1 < letters.size() &&
        (letters[i + 1] == 'n' || (IsConsonant(letters[i + 1]) && letters[i + 1] != 'y'))) {
      if (i == 0 && strict_start) return invalid();
      std::size_t consumed = 1;
      if (letters[i + 1] == 'n' &&
          (composition || i + 2 >= letters.size() ||
           !(IsVowel(letters[i + 2]) || letters[i + 2] == 'y'))) {
        consumed = 2;
      }
      result.tokens.push_back({letters.substr(i, consumed), "ん"});
      i += consumed;
      continue;
    }

    // っ: 同じ子音が 2 つ続く (tch も)。
    if (i + 1 < letters.size() && IsConsonant(c) && c != 'n' &&
        (letters[i + 1] == c ||
         (c == 't' && letters[i + 1] == 'c' && i + 2 < letters.size() && letters[i + 2] == 'h'))) {
      if (i == 0 && strict_start) return invalid();
      result.tokens.push_back({letters.substr(i, 1), "っ"});
      ++result.sokuon;
      i += 1;
      continue;
    }

    bool matched = false;
    const std::size_t max_length = std::min<std::size_t>(4, letters.size() - i);
    for (std::size_t length = max_length; length >= 1; --length) {
      std::string_view piece = letters.substr(i, length);
      auto found = spellings.find(piece);
      if (found == spellings.end()) continue;
      std::string_view kana = found->second;
      result.tokens.push_back({piece, std::string(kana)});
      i += length;
      matched = true;
      break;
    }
    if (matched) continue;

    // 残りが綴りの途中 (k, tc …) なら、入力を続ける前提として成立とする。
    std::string_view rest = letters.substr(i);
    if (absorb_trailing_n && rest == "n") {
      result.tokens.push_back({rest, "ん"});
      i += 1;
      continue;
    }
    if (partials.count(rest) > 0 || rest == "n" || rest == "tc") {
      result.is_valid = true;
      result.partial.assign(rest);
      return result;
    }
    return invalid();
  }

  result.is_valid = true;
  return result;
}

}  // namespace

std::string RomajiDetector::Analysis::Kana() const {
  std::string out;
  for (const Token& token : tokens) out += token.kana;
  return out;
}

RomajiDetector::Analysis RomajiDetector::Analyze(std::string_view letters) const {
  return AnalyzeCore(letters, /*strict_start=*/true, /*composition=*/false);
}

RomajiDetector::Analysis RomajiDetector::AnalyzeFragment(std::string_view letters) const {
  return AnalyzeCore(letters, /*strict_start=*/false, /*composition=*/true);
}

RomajiDetector::Analysis RomajiDetector::AnalyzeMidWord(std::string_view letters) const {
  return AnalyzeCore(letters, /*strict_start=*/false, /*composition=*/false);
}

RomajiDetector::Analysis RomajiDetector::AnalyzeWord(std::string_view word) const {
  // 末尾の n を ん として読み切る。word + "n" をつなぎ直すと Token::romaji が
  // 一時オブジェクトを指してしまうので、フラグで元のバッファのまま解析する。
  return AnalyzeCore(word, /*strict_start=*/true, /*composition=*/false,
                     /*absorb_trailing_n=*/true);
}

std::string RomajiDetector::ReadCRow(std::string_view letters) {
  std::string out(letters);
  for (std::size_t i = 0; i + 1 < out.size(); ++i) {
    if (out[i] == 'c' && (out[i + 1] == 'a' || out[i + 1] == 'u' || out[i + 1] == 'o')) {
      out[i] = 'k';
    }
  }
  return out;
}

std::vector<std::string> RomajiDetector::SpellingVariants(std::string_view canonical,
                                                          int limit) const {
  Analysis analysis = AnalyzeWord(canonical);
  if (!analysis.is_valid || !analysis.partial.empty()) return {std::string(canonical)};

  // tokens[index] のかなに書ける綴りの候補。
  auto options_for = [&](std::size_t index) -> std::vector<std::string_view> {
    const Token& token = analysis.tokens[index];
    const Token* next = index + 1 < analysis.tokens.size() ? &analysis.tokens[index + 1] : nullptr;
    std::vector<std::string_view> options;
    if (token.kana == "ん") {
      options.emplace_back("nn");
      if (next == nullptr) options.emplace_back("n");
      if (next != nullptr && next->kana != "っ" && !next->romaji.empty() &&
          IsConsonant(next->romaji[0]) && next->romaji[0] != 'n' && next->romaji[0] != 'y') {
        options.emplace_back("n");
      }
      if (next != nullptr && !next->romaji.empty() && next->romaji[0] == 'n') {
        options.emplace_back("n");
      }
      return options;
    }
    if (token.kana == "っ") {
      if (next != nullptr) {
        auto found = KanaToSpellings().find(next->kana);
        if (found != KanaToSpellings().end()) {
          for (std::string_view spelling : SplitSpaces(found->second)) {
            if (!spelling.empty() && IsConsonant(spelling[0]) && spelling[0] != 'n') {
              options.push_back(spelling.substr(0, 1));
            }
          }
        }
      }
      if (options.empty()) options.push_back(token.romaji);
      return options;
    }
    auto found = KanaToSpellings().find(token.kana);
    if (found != KanaToSpellings().end()) {
      for (std::string_view spelling : SplitSpaces(found->second)) options.push_back(spelling);
    }
    if (options.empty()) options.push_back(token.romaji);
    return options;
  };

  std::vector<std::string> results;
  std::string builder;
  std::function<void(std::size_t)> walk = [&](std::size_t index) {
    if (static_cast<int>(results.size()) >= limit) return;
    if (index == analysis.tokens.size()) {
      results.push_back(builder);
      return;
    }
    const std::size_t mark = builder.size();
    for (std::string_view option : options_for(index)) {
      builder.append(option);
      walk(index + 1);
      builder.resize(mark);
      if (static_cast<int>(results.size()) >= limit) return;
    }
  };
  walk(0);

  bool has_canonical = false;
  for (const std::string& result : results) {
    if (result == canonical) has_canonical = true;
  }
  if (!has_canonical) results.insert(results.begin(), std::string(canonical));
  return results;
}

}  // namespace meltype
}  // namespace mozkey
