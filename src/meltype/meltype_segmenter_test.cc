// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The Mozkey Authors
//
// 英日混在の自動分割 (Meltype の ScoredSegmentation の移植) の確認。
//
//   clang++ -std=c++20 -Isrc src/meltype/meltype_segmenter_test.cc
//       src/meltype/scored_segmenter.cc src/meltype/romaji_detector.cc
//       src/meltype/word_list.cc -o meltype_segmenter_test
//   ./meltype_segmenter_test
//
// 辞書は MELTYPE_DICT_DIR (環境変数) > このファイルの隣の dictionaries >
// A:/Meltype/dictionaries (参照実装) の順に探す。

#include "meltype/romaji_detector.h"
#include "meltype/scored_segmenter.h"
#include "meltype/word_list.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

// 区間の並びを、比較しやすい 1 行にする (英語は打った文字、日本語はかな)。
std::string Render(const std::vector<mozkey::meltype::ScoredSegmenter::Segment>& segments) {
  std::string out;
  for (const mozkey::meltype::ScoredSegmenter::Segment& segment : segments) {
    if (!out.empty()) out += "|";
    out += segment.is_english ? "EN:" + segment.raw : "JA:" + segment.kana;
  }
  return out;
}

mozkey::meltype::ScoredSegmenter* g_segmenter = nullptr;

void Expect(const std::string& what, const std::string& input,
            const mozkey::meltype::ScoredSegmenter::Options& options,
            const std::string& expected) {
  ++g_checks;
  const std::string actual = Render(g_segmenter->SegmentInput(input, options));
  if (actual == expected) {
    std::cout << "  PASS  " << what << ": " << input << " -> " << actual << "\n";
    return;
  }
  ++g_failures;
  std::cout << "  FAIL  " << what << ": " << input << "\n"
            << "        期待: " << expected << "\n"
            << "        実際: " << actual << "\n";
}

// 打ち終わった状態 (Enter・Space) で区間分けする。
mozkey::meltype::ScoredSegmenter::Options Final() {
  return mozkey::meltype::ScoredSegmenter::Options{};
}

// 1 つの確認。ok なら PASS、違えば FAIL。
void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (ok) {
    std::cout << "  PASS  " << what << "\n";
    return;
  }
  ++g_failures;
  std::cout << "  FAIL  " << what << "\n";
}

// AnalyzeWord: 末尾に n が 1 文字だけ残る語 (anshin → あんしん)。
// Token::romaji は解析に渡した文字列を指すので、ここで読み出しが壊れていないかを確認する。
void RomajiDetectorTests() {
  std::cout << "\n[6] RomajiDetector (末尾の n と綴りの揺れ)\n";
  const mozkey::meltype::RomajiDetector romaji;
  for (const auto& [input, kana] : std::vector<std::pair<std::string, std::string>>{
           {"anshin", "あんしん"},
           {"kanren", "かんれん"},
           {"genkan", "げんかん"},
           {"nihon", "にほん"},
           {"konnnichiha", "こんにちは"},
       }) {
    const mozkey::meltype::RomajiDetector::Analysis analysis = romaji.AnalyzeWord(input);
    // かなと、Token::romaji をつなぎ直したものが元の入力と一致するかを両方見る。
    std::string romaji_back;
    for (const mozkey::meltype::RomajiDetector::Token& token : analysis.tokens) {
      romaji_back += token.romaji;
    }
    Check(analysis.is_valid && analysis.partial.empty() && analysis.Kana() == kana &&
              romaji_back == input,
          "AnalyzeWord: " + input + " -> " + kana + " (かなと綴りの両方)");
  }

  // SpellingVariants: 日本語の辞書の綴りの揺れ (si/shi, tu/tsu, ん の n/nn …) の展開。
  for (const auto& [canonical, variant] : std::vector<std::pair<std::string, std::string>>{
           {"anshin", "ansin"},
           {"kanren", "kanren"},
           {"genkan", "genkan"},
           {"kensaku", "kensaku"},
       }) {
    const std::vector<std::string> variants = romaji.SpellingVariants(canonical);
    bool has_variant = false;
    for (const std::string& candidate : variants) {
      if (candidate == variant) has_variant = true;
    }
    Check(has_variant, "SpellingVariants: " + canonical + " に " + variant + " が含まれる");
  }
}

// 曖昧な語 (ローマ字としても読める英単語) のフラグが立っているか。
bool HasAmbiguousEnglish(const std::string& input,
                         const mozkey::meltype::ScoredSegmenter::Options& options) {
  for (const mozkey::meltype::ScoredSegmenter::Segment& segment :
       g_segmenter->SegmentInput(input, options)) {
    if (segment.is_english && segment.ambiguous) return true;
  }
  return false;
}

// 辞書の入っているディレクトリを探す。
std::string DictionaryDirectory() {
  if (const char* from_env = std::getenv("MELTYPE_DICT_DIR"); from_env && *from_env) {
    return from_env;
  }
  const std::filesystem::path here =
      std::filesystem::absolute(__FILE__).parent_path();
  for (const std::filesystem::path& candidate :
       {here / "dictionaries", here.parent_path() / "dictionaries"}) {
    if (std::filesystem::exists(candidate / "english.txt")) return candidate.string();
  }
  // 参照実装 (Meltype) の辞書をそのまま読む (開発時の検証用)。
  return "A:/Meltype/dictionaries";
}

}  // namespace

int main() {
  using mozkey::meltype::MeltypeDictionaries;
  using mozkey::meltype::ScoredSegmenter;

  const std::string directory = DictionaryDirectory();
  MeltypeDictionaries dictionaries;
  const std::size_t loaded = MeltypeDictionaries::LoadFromDirectory(directory, dictionaries);
  std::cout << "辞書: " << directory << " ("
            << "english=" << dictionaries.english.WordCount()
            << " proper=" << dictionaries.proper.WordCount()
            << " readable=" << dictionaries.readable.WordCount()
            << " spell=" << dictionaries.spell.WordCount()
            << " japanese=" << dictionaries.japanese.WordCount() << ")\n";
  if (loaded == 0) {
    std::cout << "辞書が読めません。MELTYPE_DICT_DIR を設定してください。\n";
    return 2;
  }

  ScoredSegmenter segmenter(dictionaries);
  g_segmenter = &segmenter;

  std::cout << "\n[1] 代表的な混在入力\n";
  // 英語の区間は打った文字、日本語の区間はかな (漢字変換は Mozc 側の辞書の仕事)。
  Expect("英単語をはさんだ日本語", "kyouhagoogledekensaku", Final(),
         "JA:きょうは|EN:google|JA:でけんさく");
  Expect("外来語 + 助詞", "ashitanomeetingwotsuika", Final(),
         "JA:あしたの|EN:meeting|JA:をついか");
  Expect("英文のまま", "hello world", Final(), "EN:hello world");
  Expect("日本語のまま (空白つき)", "sushi o taberu", Final(), "JA:すし お たべる");
  Expect("日本語のまま", "nihongo", Final(), "JA:にほんご");

  std::cout << "\n[2] 日本語の語を壊さない\n";
  // 英単語の最後の子音と は が 1 音 (sha) になっても、日本語の語なら分けない。
  Expect("電車", "densha", Final(), "JA:でんしゃ");
  Expect("会社", "kaisha", Final(), "JA:かいしゃ");
  Expect("しゃぶしゃぶ", "shabushabu", Final(), "JA:しゃぶしゃぶ");
  Expect("日本語 + 英単語 + 日本語", "kyouhaiitenkidesunegoogledekensakushite", Final(),
         "JA:きょうはいいてんきですね|EN:google|JA:でけんさくして");

  std::cout << "\n[3] 英単語 + 助詞の は\n";
  // 英単語の最後の子音と は が 1 音 (medal + sha) になっても、英単語 + は に分ける。
  Expect("medals は", "soredemedalsha", Final(), "JA:それで|EN:medals|JA:は");
  Expect("president は", "tsukattepresidentha", Final(),
         "JA:つかって|EN:president|JA:は");
  Expect("straights は", "atodestraightsha", Final(), "JA:あとで|EN:straights|JA:は");

  std::cout << "\n[4] 曖昧な語 (日本語としても読める英単語)\n";
  {
    // 直前が英語の文脈なら、ローマ字として最後まで読める英単語も英語にする。
    ScoredSegmenter::Options after_english;
    after_english.preceding_english = true;
    Expect("fine (英文の続き)", "fine", after_english, "EN:fine");
    Expect("take (英文の続き)", "take", after_english, "EN:take");
    // 文脈が無ければ日本語の読みを優先する。
    Expect("fine (文脈なし)", "fine", Final(), "JA:ふぃね");
    Expect("take (文脈なし)", "take", Final(), "JA:たけ");
    ++g_checks;
    if (HasAmbiguousEnglish("fine", after_english)) {
      std::cout << "  PASS  曖昧フラグ: fine は日本語としても読める語として印が付く\n";
    } else {
      ++g_failures;
      std::cout << "  FAIL  曖昧フラグ: fine に印が付かない\n";
    }
    // ぢ・づ で読める英単語 (audio = あうぢお) は、文脈が無くても英語。
    Expect("audio (ぢで読める語)", "kyouhaaudiowokikasete", Final(),
           "JA:きょうは|EN:audio|JA:をきかせて");
  }

  std::cout << "\n[5] 端の入力\n";
  Expect("空", "", Final(), "");
  Expect("日本語だけ", "konnnichiha", Final(), "JA:こんにちは");
  Expect("記号つき英文", "hello, world!", Final(), "EN:hello, world!");

  std::cout << "\n[7] 促音の境界 (英単語 + 日本語の促音)\n";
  // 英単語の末尾と次の文字が同じ子音でも、日本語の促音「っ」に潰さず、
  // 英単語と日本語に切り分ける (tab + botan = たっぼたん にしない)。
  Expect("Tab + ボタン", "tabbotan", Final(), "EN:tab|JA:ぼたん");
  Expect("Tab + ボタンを", "tabbotanwo", Final(), "EN:tab|JA:ぼたんを");
  Expect("文中の Tab + ボタン", "kyouhatabbotanwoosita", Final(),
         "JA:きょうは|EN:tab|JA:ぼたんをおした");
  Expect("Tab + を + ボタン", "tabwobotan", Final(), "EN:tab|JA:をぼたん");

  // 英単語に直接続く日本語の促音の接尾辞・助詞は壊さない (§32§)。
  Expect("apple って", "applette", Final(), "EN:apple|JA:って");
  Expect("minute って", "minutette", Final(), "EN:minute|JA:って");
  Expect("hacker っぽい", "hackerppoi", Final(), "EN:hacker|JA:っぽい");
  Expect("minute っていう", "minutetteiuego", Final(),
         "EN:minute|JA:っていうえご");
  Expect("batman って", "batmantte", Final(), "EN:batman|JA:って");

  // 日本語の促音 (有声子音でも生じる外来語を含む) はそのまま読む。
  Expect("バッグ (有声促音)", "baggu", Final(), "JA:ばっぐ");
  Expect("ベッド (有声促音)", "beddo", Final(), "JA:べっど");
  Expect("グッズ (有声促音)", "guzzu", Final(), "JA:ぐっず");
  Expect("ドッグ (有声促音)", "doggu", Final(), "JA:どっぐ");
  Expect("キッズ (有声促音)", "kizzu", Final(), "JA:きっず");
  Expect("バッジ (有声促音)", "pajji", Final(), "JA:ぱっじ");
  Expect("きって", "kitte", Final(), "JA:きって");
  Expect("きってください", "kittekudasai", Final(), "JA:きってください");
  Expect("がっこう", "gakkou", Final(), "JA:がっこう");
  Expect("やっぱり", "yappari", Final(), "JA:やっぱり");
  Expect("まったく", "mattaku", Final(), "JA:まったく");
  Expect("ずっと", "zutto", Final(), "JA:ずっと");
  Expect("にっぽん", "nippon", Final(), "JA:にっぽん");
  Expect("はっぴょう", "happyou", Final(), "JA:はっぴょう");
  Expect("けっか", "kekka", Final(), "JA:けっか");
  Expect("っちゃうってっぽい", "cchautteppoi", Final(),
         "JA:っちゃうってっぽい");

  std::cout << "\n[8] 大文字の頭字語 (アクロニム) と日本語の境界\n";
  // 大文字の連続 (WSL, CPU, GPU, API) は英単語辞書に無くても英語の 1 語として
  // 切り出し、直後の日本語 (から起動して) を巻き込まない。以前は大文字の塊の
  // 途中 (SL, PU, PI) で区切られ、全体がローマ字のまま残っていた。
  Expect("WSL から起動して", "WSLkarakidoushite", Final(), "EN:WSL|JA:からきどうして");
  Expect("CPU から起動して", "CPUkarakidoushite", Final(), "EN:CPU|JA:からきどうして");
  Expect("GPU から起動して", "GPUkarakidoushite", Final(), "EN:GPU|JA:からきどうして");
  Expect("API から起動して", "APIkarakidoushite", Final(), "EN:API|JA:からきどうして");
  // 頭字語だけの入力と、助詞が続く形。
  Expect("WSL だけ", "WSL", Final(), "EN:WSL");
  Expect("API を", "APIwo", Final(), "EN:API|JA:を");
  Expect("CPU が", "CPUga", Final(), "EN:CPU|JA:が");
  Expect("GPU で", "GPUde", Final(), "EN:GPU|JA:で");
  // 日本語の途中に置いても、頭字語だけが英字になる。
  Expect("きょうは WSL から起動して", "kyouhaWSLkarakidoushite", Final(),
         "JA:きょうは|EN:WSL|JA:からきどうして");
  // 不規則な混在大文字 (PUka, WSLkara) は英語の 1 語にしない。
  Expect("PUka は 1 語の英語にしない", "PUka", Final(), "EN:PU|JA:か");
  Expect("WSLkara は WSL + から", "WSLkara", Final(), "EN:WSL|JA:から");
  // 大文字始まりの普通の語 (TitleCase) と、登録済みの混在大文字の固有名詞は残す。
  Expect("Google (TitleCase)", "Google", Final(), "EN:Google");
  Expect("iPhone を", "iPhonewo", Final(), "EN:iPhone|JA:を");
  Expect("ChatGPT に", "ChatGPTni", Final(), "EN:ChatGPT|JA:に");

  std::cout << "\n[9] CamelCase 複合語の単語境界\n";
  // 辞書に無い CamelCase の複合語は、両側とも知っている英単語のときだけ
  // 分ける。ピースの境界スペースで「ChatGPT Plus」のように空いて出る。
  Expect("ChatGPTPlus", "ChatGPTPlus", Final(), "EN:ChatGPT|EN:Plus");
  Expect("ChatGPTPluswo", "ChatGPTPluswo", Final(), "EN:ChatGPT|EN:Plus|JA:を");
  Expect("GoogleCloud", "GoogleCloud", Final(), "EN:Google|EN:Cloud");
  // 区間全体が登録済みの語 (YouTube, JavaScript) は 1 語のまま分けない。
  Expect("YouTube は分けない", "YouTube", Final(), "EN:YouTube");
  Expect("JavaScript は分けない", "JavaScript", Final(), "EN:JavaScript");
  // 片側が知らない語なら、ただの大文字の並び (McDonald) として分けない。
  Expect("McDonald は分けない", "McDonald", Final(), "EN:McDonald");
  // 打った空白を含む英文は 1 つの英文のまま。
  Expect("hello world は 1 つの英文", "hello world", Final(), "EN:hello world");
  Expect("hello, world! は 1 つの英文", "hello, world!", Final(), "EN:hello, world!");

  RomajiDetectorTests();

  std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " PASS"
            << (g_failures == 0 ? " (すべて通った)\n" : "\n");
  return g_failures == 0 ? 0 : 1;
}
