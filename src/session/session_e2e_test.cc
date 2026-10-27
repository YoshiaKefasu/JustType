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

// End-to-end pipeline test: romaji input all the way to a committed string.
//
// The stages mirror the Session's piece pipeline (session/session.cc), in the
// same order:
//
//   1. input       the romaji the user types
//   2. Meltype     the real ScoredSegmenter over the real dictionaries in
//                  meltype/dictionaries: raw romaji -> English/Japanese pieces
//   3. candidates  the (mocked) dictionary candidates for each Japanese
//                  piece's reading; an English piece has none
//   4. plan        session/meltype_piece_pipeline.cc: the per-piece plan that
//                  feeds the SystemOne questions and their left contexts
//   5. SystemOne   one question per Japanese piece — left context, reading,
//                  candidate list — through the real systemone_client.cc
//                  (injected transport, so URL parsing, request building and
//                  response validation all run) and the real
//                  systemone_logger.cc recording every exchange
//   6. screen + commit
//                  ProtectSegmentsWithPieces: the (mocked) Mozc multi-clause
//                  baseline with the English pieces protected to their typed
//                  letters and the answers collapsed in — Japanese clauses
//                  stay exactly what Mozc converted
//
// plus the two things the same pipeline depends on that have nothing to do
// with conversion: the ms-ime.tsv keymap binding for Space, and the bundled
// Gen Interface JP Display registration.
//
// What is mocked, and why: only the dictionary converter's candidate lists
// and the transport.  Real candidates come from mozc.data behind the engine;
// Mozc's own session tests mock the same layer (MockEngineConverterInterface),
// so this test does the same and states exactly what it substitutes.
//
// What is NOT covered here, and where it is covered instead: the TSF client
// side (win32/tip), the real composer's romaji state machine, the session-side
// asynchronous worker plumbing, and the installed <JustTypeDir>\fonts path.  The
// first three are compile-checked with the Session build; the last needs a
// live desktop session, so it is verified by typing in the installed build
// after the MSI is rebuilt.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "composer/composer.h"
#include "composer/table.h"
#include "config/config_handler.h"
#include "engine/engine_interface.h"
#include "engine/mock_data_engine_factory.h"
#include "meltype/scored_segmenter.h"
#include "protocol/commands.pb.h"
#include "protocol/config.pb.h"
#include "renderer/win32/win32_font_resource.h"
#include "session/meltype_bridge.h"
#include "session/meltype_piece_pipeline.h"
#include "session/session.h"
#include "session/systemone_client.h"
#include "session/systemone_logger.h"
#include "transliteration/transliteration.h"

// This test is Windows-only (see the target_compatible_with in
// session/BUILD.bazel): it drives the real SystemOne WinHTTP client and the real
// GDI font registration, both of which exist only there.
#include <windows.h>

namespace {

using mozc::composer::Composer;
using mozc::composer::Table;
using mozc::commands::Command;
using mozc::commands::KeyEvent;
using mozc::commands::Request;
using mozc::config::Config;
using mozc::config::ConfigHandler;
using mozc::transliteration::FULL_ASCII;
using mozc::transliteration::FULL_KATAKANA;
using mozc::transliteration::HALF_ASCII;
using mozc::transliteration::HIRAGANA;
using mozc::Engine;
using mozc::MockDataEngineFactory;
using mozc::session::MeltypeBridge;
using mozc::session::Session;
using mozc::session::OrderPieceCandidates;
using mozc::session::OrderedPieceCandidates;
using mozc::session::PieceCandidate;
using mozc::session::PiecePlan;
using mozc::session::PieceQuestion;
using mozc::session::PieceSegment;
using mozc::session::SystemOneLogger;
using mozc::session::SystemOneResponse;
using mozkey::meltype::ScoredSegmenter;

int g_checks = 0;
int g_failures = 0;

void Report(bool ok, const std::string& what, const std::string& detail) {
  ++g_checks;
  if (ok) {
    std::cout << "  PASS  " << what << (detail.empty() ? "" : ": " + detail)
              << "\n";
    return;
  }
  ++g_failures;
  std::cout << "  FAIL  " << what << (detail.empty() ? "" : ": " + detail)
            << "\n";
}

void ReportEq(const std::string& what, const std::string& expected,
              const std::string& actual) {
  Report(expected == actual, what, "expected=[" + expected + "] actual=[" +
                                      actual + "]");
}

// ワイド文字列を UTF-8 に直して比較する (パス解決の検証用)。
std::string ToUtf8(const std::wstring& value) {
  if (value.empty()) return std::string();
  const int size = ::WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
  if (size <= 0) return std::string();
  std::string out(static_cast<std::size_t>(size), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, value.data(),
                        static_cast<int>(value.size()), out.data(), size,
                        nullptr, nullptr);
  return out;
}

void ReportEq(const std::string& what, const std::wstring& expected,
              const std::wstring& actual) {
  ReportEq(what, ToUtf8(expected), ToUtf8(actual));
}

// Reads a UTF-16LE text file (with or without BOM) and returns it as UTF-8.
//
// systemone_logger.cc writes UTF-16LE with a BOM so editors detect the encoding
// instead of mangling the Japanese text, so the assertions below have to decode
// before they can search for the field names a reader would see.
std::string ReadUtf16LeAsUtf8(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return std::string();
  std::stringstream buffer;
  buffer << input.rdbuf();
  const std::string bytes = buffer.str();
  if (bytes.size() < 2) return std::string();

  std::size_t offset = 0;
  if (static_cast<unsigned char>(bytes[0]) == 0xFF &&
      static_cast<unsigned char>(bytes[1]) == 0xFE) {
    offset = 2;  // BOM
  }
  const std::size_t wide_chars = (bytes.size() - offset) / 2;
  std::wstring wide(wide_chars, L'\0');
  for (std::size_t i = 0; i < wide_chars; ++i) {
    const std::size_t at = offset + i * 2;
    wide[i] = static_cast<wchar_t>(
        static_cast<unsigned char>(bytes[at]) |
        (static_cast<unsigned char>(bytes[at + 1]) << 8));
  }
  return ToUtf8(wide);
}

// ---------------------------------------------------------------------------
// Test data locations.
// ---------------------------------------------------------------------------

std::string FromEnv(const char* name, const std::string& fallback) {
  if (const char* value = std::getenv(name); value != nullptr && *value) {
    return value;
  }
  return fallback;
}

std::string DictionaryDirectory() {
  return FromEnv("MELTYPE_DICT_DIR",
                 std::filesystem::absolute(__FILE__).parent_path()
                     .parent_path()
                     .append("meltype")
                     .append("dictionaries")
                     .string());
}

std::string KeymapDirectory() {
  return FromEnv("MOZKEY_KEYMAP_DIR",
                 std::filesystem::absolute(__FILE__).parent_path()
                     .parent_path()
                     .append("data")
                     .append("keymap")
                     .string());
}

std::string BundledFontsSourceDirectory() {
  return FromEnv("MOZKEY_FONTS_DIR",
                 std::filesystem::absolute(__FILE__).parent_path()
                     .parent_path()
                     .append("data")
                     .append("fonts")
                     .string());
}

// ---------------------------------------------------------------------------
// The pipeline under test.
//
// kRejectOffer answers with a surface that was never offered, which
// ParseResponseJson must refuse rather than forward into the IME.
//
constexpr char kRejectOffer[] = "<answer with a surface that was not offered>";

// One end-to-end case.  |candidates| is parallel to the Meltype segmentation:
// the Mozc candidates for each Japanese piece's reading; the English span's
// entry is ignored by the pipeline.
//
// |baseline| is what Mozc itself converted the whole reading to (keys = the
// reading slices, values = the surfaces): the screen and the commit are this
// multi-clause baseline with the English pieces protected on top of it.
struct PipelineCase {
  std::string romaji;
  std::vector<std::pair<std::string, std::string>> baseline;
  std::vector<std::vector<std::string>> candidates;
  bool systemone = true;
  // Scripted answers, one per Japanese question, in question order.
  std::vector<std::string> answers;
  std::string expected_commit;
  // Optional exact expectations, checked only when non-empty.
  std::vector<std::string> expected_states;
  std::vector<std::vector<std::string>> expected_options;
  std::string note;
  // Alignment-refusal cases keep Mozc's baseline on purpose — the English
  // letters are expected to be absent there.
  bool skip_english_check = false;
  // Parallel to |candidates| and optional: a non-empty entry names the surface
  // Mozc itself marked as its typo correction ("もしかして") for that piece's
  // reading — the list stays exactly what Mozc returns, correction in its real
  // position.  The pipeline promotes it, so the case proves the screen and the
  // commit show the correction with nobody choosing anything.
  std::vector<std::string> typo_corrections;
};

struct PipelineResult {
  // The plan after the replay: baseline first candidate + applied answers.
  PiecePlan plan;
  // The `state` string of every question that went out, in order.
  std::vector<std::string> asked_states;
  // The options of every question that went out, in order.
  std::vector<std::vector<std::string>> asked_options;
  std::vector<SystemOneResponse> responses;
  int transport_calls = 0;
  // Mozc's baseline with the English protection and the answers overlaid —
  // what the user sees and commits.
  std::vector<PieceSegment> protected_segments;
};

// Runs the production stages.  The Session submits its questions on a worker
// thread and applies the answers from the APPLY_SYSTEMONE_LIVE delayed
// callback; this test replays the same build/ask/apply logic inline and in
// order, so every question carries the left context that is actually on
// screen when it goes out (the steady state the Session reaches: a piece
// already answered is answered from the choice cache before the next batch is
// built).  The screen itself is the case's Mozc baseline with the English
// pieces protected on top — Japanese clauses always stay Mozc's own.
//
// |logger| records every exchange exactly as Session does.
PipelineResult RunPipeline(const ScoredSegmenter& segmenter,
                           const PipelineCase& test_case,
                           SystemOneLogger* logger) {
  PipelineResult result;

  // Stage 2: Meltype segments the romaji the user typed.
  const std::vector<ScoredSegmenter::Segment> spans =
      segmenter.SegmentInput(test_case.romaji, ScoredSegmenter::Options{});

  // Stage 3: candidates per piece; English pieces get (and need) none.
  // Mozc's typo correction is ordered first here, exactly as the Session's
  // CollectMozcPieceOptions() does it, so the baseline and the options the
  // model sees are the production ones.
  std::vector<std::vector<std::string>> options(spans.size());
  std::vector<bool> typo_corrections(spans.size(), false);
  for (std::size_t i = 0; i < spans.size(); ++i) {
    if (spans[i].is_english) continue;
    if (i >= test_case.candidates.size()) continue;
    const std::string correction =
        i < test_case.typo_corrections.size() ? test_case.typo_corrections[i]
                                              : std::string();
    std::vector<PieceCandidate> candidates;
    candidates.reserve(test_case.candidates[i].size());
    for (const std::string& surface : test_case.candidates[i]) {
      // Only the surface the case names is Mozc's correction; the rest are
      // Mozc's ordinary conversions in Mozc's own order.
      const bool is_correction = !correction.empty() && surface == correction;
      candidates.push_back(PieceCandidate{surface, is_correction});
    }
    const OrderedPieceCandidates ordered = OrderPieceCandidates(candidates);
    options[i] = ordered.surfaces;
    typo_corrections[i] = ordered.typo_correction;
  }

  // Stage 4: the question plan (per-piece baselines and left contexts).
  result.plan = mozc::session::BuildPiecePlan(spans, options, typo_corrections);

  std::vector<std::string> choices(spans.size());
  if (test_case.systemone) {
    // Stage 5: one question per Japanese piece, asked in order.
    for (std::size_t qi = 0; qi < result.plan.questions.size(); ++qi) {
      // Copy: applying an answer refreshes the *later* questions' left
      // contexts, and this question's own text must stay what was asked.
      const PieceQuestion question = result.plan.questions[qi];

      mozc::session::SystemOneRequest request;
      request.endpoint_url = mozc::session::kSystemOneDefaultEndpointUrl;
      request.model_name = mozc::session::kSystemOneDefaultModelName;
      request.timeout_msec = 500;
      request.state = mozc::session::PieceQuestionState(question);
      request.options = question.options;

      result.asked_states.push_back(request.state);
      result.asked_options.push_back(request.options);

      const std::string chosen =
          qi < test_case.answers.size() ? test_case.answers[qi]
                                        : question.options.front();

      // The transport is injected, so URL parsing, request building and
      // response validation all still run — only WinHTTP is replaced.
      ++result.transport_calls;
      mozc::session::SystemOneClient client(
          [&chosen](const mozc::session::ParsedEndpoint&,
                    const std::wstring&, const std::string&,
                    uint32_t timeout_msec) {
            mozc::session::HttpResult http;
            http.transport_ok = true;
            http.http_status = 200;
            http.latency_msec = timeout_msec;
            std::string surface =
                chosen == kRejectOffer ? std::string("NOT_OFFERED") : chosen;
            std::string escaped;
            for (const char c : surface) {
              if (c == '"' || c == '\\') escaped += '\\';
              escaped += c;
            }
            http.body = "{\"answers\":{\"choice\":{\"type\":\"choice\","
                        "\"choice\":\"" +
                        escaped + "\",\"confidence\":0.97}}}";
            return http;
          });

      const SystemOneResponse response = client.Ask(request);
      result.responses.push_back(response);
      if (logger != nullptr) logger->LogExchange(request, response);

      // A rejected or failed answer keeps the baseline: the pipeline never
      // forwards a surface it was not offered.
      if (!response.ok) continue;
      if (mozc::session::ApplyPieceChoice(&result.plan, question.piece_index,
                                          response.choice)) {
        choices[question.piece_index] = response.choice;
      }
    }
  }

  // Stage 6: the screen and the commit are Mozc's own baseline with the
  // English protection (and any answer) overlaid.  A piece Mozc corrected
  // shows the correction even with no answer — that is the promotion.
  std::vector<PieceSegment> baseline_segments;
  for (const auto& [key, value] : test_case.baseline) {
    baseline_segments.push_back(PieceSegment{key, value});
  }
  const std::vector<std::string> display_choices =
      mozc::session::ResolvePieceDisplayChoices(result.plan, choices);
  result.protected_segments = mozc::session::ProtectSegmentsWithPieces(
      baseline_segments, spans, display_choices);
  return result;
}

std::string JoinProtectedValues(const std::vector<PieceSegment>& segments) {
  std::string out;
  for (const PieceSegment& segment : segments) out += segment.value;
  return out;
}

std::string JoinProtectedKeys(const std::vector<PieceSegment>& segments) {
  std::string out;
  for (const PieceSegment& segment : segments) out += segment.key;
  return out;
}

// ---------------------------------------------------------------------------
// The keymap data files.
// ---------------------------------------------------------------------------

// Splits a keymap .tsv row into its tab-separated columns.
std::vector<std::string> SplitRow(const std::string& line) {
  std::vector<std::string> fields;
  std::size_t start = 0;
  while (true) {
    const std::size_t tab = line.find('\t', start);
    if (tab == std::string::npos) {
      fields.push_back(line.substr(start));
      return fields;
    }
    fields.push_back(line.substr(start, tab - start));
    start = tab + 1;
  }
}

// The commands keymap.cc registers for the Precomposition state.  Read out of
// the source so a command the .tsv names but the code never registered cannot
// silently become a no-op.
//
// keymap.cc splits long registrations across lines, so the name is searched for
// after the call rather than on the call's own line.
std::vector<std::string> RegisteredPrecompositionCommands(
    const std::string& keymap_source_path) {
  std::vector<std::string> names;
  std::ifstream input(keymap_source_path);
  if (!input) return names;

  std::stringstream buffer;
  buffer << input.rdbuf();
  const std::string source = buffer.str();

  std::size_t at = source.find("RegisterPrecompositionCommand");
  while (at != std::string::npos) {
    const std::size_t quote = source.find('"', at);
    const std::size_t call_end = source.find(')', at);
    if (quote != std::string::npos &&
        (call_end == std::string::npos || quote < call_end)) {
      const std::size_t end = source.find('"', quote + 1);
      if (end != std::string::npos) {
        names.push_back(source.substr(quote + 1, end - quote - 1));
      }
    }
    at = source.find("RegisterPrecompositionCommand", at + 1);
  }
  return names;
}

}  // namespace

int main() {
  // -----------------------------------------------------------------------
  // Stage 2 setup: the real dictionaries.
  // -----------------------------------------------------------------------
  const std::string dictionary_directory = DictionaryDirectory();
  mozkey::meltype::MeltypeDictionaries dictionaries;
  const std::size_t loaded = mozkey::meltype::MeltypeDictionaries::LoadFromDirectory(
      dictionary_directory, dictionaries);
  std::cout << "Meltype 辞書: " << dictionary_directory << " (english="
            << dictionaries.english.WordCount()
            << " proper=" << dictionaries.proper.WordCount()
            << " readable=" << dictionaries.readable.WordCount()
            << " spell=" << dictionaries.spell.WordCount()
            << " japanese=" << dictionaries.japanese.WordCount() << ")\n";
  if (loaded == 0) {
    std::cout << "辞書が読めません。MELTYPE_DICT_DIR を設定してください。\n";
    return 2;
  }
  const ScoredSegmenter segmenter(dictionaries);

  // -----------------------------------------------------------------------
  // [1] Meltype の前段処理: 英語スパンが英字のまま切り離される
  // -----------------------------------------------------------------------
  std::cout << "\n[1] Meltype 前段処理 (英語スパンの分離)\n";
  {
    const std::vector<ScoredSegmenter::Segment> spans =
        segmenter.SegmentInput("kyouhagoogledekensaku",
                               ScoredSegmenter::Options{});
    std::string rendered;
    for (const auto& span : spans) {
      if (!rendered.empty()) rendered += "|";
      rendered += span.is_english ? "EN:" + span.raw : "JA:" + span.kana;
    }
    ReportEq("kyouhagoogledekensaku の区間分け", "JA:きょうは|EN:google|JA:でけんさく",
             rendered);
  }
  {
    const std::vector<ScoredSegmenter::Segment> spans =
        segmenter.SegmentInput("zenzpaipuran", ScoredSegmenter::Options{});
    std::string rendered;
    for (const auto& span : spans) {
      if (!rendered.empty()) rendered += "|";
      rendered += span.is_english ? "EN:" + span.raw : "JA:" + span.kana;
    }
    ReportEq("zenzpaipuran の区間分け", "EN:zenz|JA:ぱいぷらん", rendered);
  }
  {
    const std::vector<ScoredSegmenter::Segment> spans =
        segmenter.SegmentInput("sistemu", ScoredSegmenter::Options{});
    std::string rendered;
    for (const auto& span : spans) {
      if (!rendered.empty()) rendered += "|";
      rendered += span.is_english ? "EN:" + span.raw : "JA:" + span.kana;
    }
    // 文脈が無くても sistemu は英語として読める語 (english-readable) なので
    // 英語スパンになる。日本語読みを優先させたい入力は shisutemu のように綴る。
    ReportEq("sistemu の区間分け", "EN:sistemu", rendered);
  }
  {
    const std::vector<ScoredSegmenter::Segment> spans =
        segmenter.SegmentInput("shisutemu", ScoredSegmenter::Options{});
    std::string rendered;
    for (const auto& span : spans) {
      if (!rendered.empty()) rendered += "|";
      rendered += span.is_english ? "EN:" + span.raw : "JA:" + span.kana;
    }
    ReportEq("shisutemu の区間分け (日本語のまま)", "JA:しすてむ", rendered);
  }

  {
    const std::vector<ScoredSegmenter::Segment> spans =
        segmenter.SegmentInput("kyouhagoogledekennsakushita",
                               ScoredSegmenter::Options{});
    std::string rendered;
    for (const auto& span : spans) {
      if (!rendered.empty()) rendered += "|";
      rendered += span.is_english ? "EN:" + span.raw : "JA:" + span.kana;
    }
    // kenn は english-readable.txt に載せていないので、ローマ字の「けん」が
    // 英語区間として切り離されることはない。載せていると でけん が JA:で と
    // EN:kenn に割れて、英語スパンが2つになる。
    ReportEq("kyouhagoogledekennsakushita の区間分け",
             "JA:きょうは|EN:google|JA:でけんさくした", rendered);
  }
  {
    const std::vector<ScoredSegmenter::Segment> spans =
        segmenter.SegmentInput("kennsakushita", ScoredSegmenter::Options{});
    std::string rendered;
    for (const auto& span : spans) {
      if (!rendered.empty()) rendered += "|";
      rendered += span.is_english ? "EN:" + span.raw : "JA:" + span.kana;
    }
    ReportEq("kennsakushita は日本語として読める", "JA:けんさくした",
             rendered);
  }

  // [修正検証] 辞書にあった suru 活用 (shita/sita) の削除と IsSuruForm の
  // ひらがな判定修正で、英語 + する活用が 1 つの英語に潰れないこと。
  {
    const std::vector<ScoredSegmenter::Segment> spans =
        segmenter.SegmentInput("stopshita", ScoredSegmenter::Options{});
    std::string rendered;
    for (const auto& span : spans) {
      if (!rendered.empty()) rendered += "|";
      rendered += span.is_english ? "EN:" + span.raw : "JA:" + span.kana;
    }
    ReportEq("stopshita の区間分け", "EN:stop|JA:した", rendered);
  }
  // [修正検証] 辞書にある実在英単語と同じ綴りの日本語の助詞・形式名詞
  // (made=まで, demo=でも, tame=ため …) は、英単語の直後では英語同士の
  // 2 乗スコア合体をさせない (slackmade が全体英語に化けるのを防ぐ)。
  {
    const std::vector<ScoredSegmenter::Segment> spans =
        segmenter.SegmentInput("slackmade", ScoredSegmenter::Options{});
    std::string rendered;
    for (const auto& span : spans) {
      if (!rendered.empty()) rendered += "|";
      rendered += span.is_english ? "EN:" + span.raw : "JA:" + span.kana;
    }
    ReportEq("slackmade の区間分け (made=まで合体防止)", "EN:slack|JA:まで", rendered);
  }
  {
    const std::vector<ScoredSegmenter::Segment> spans =
        segmenter.SegmentInput("googledemo", ScoredSegmenter::Options{});
    std::string rendered;
    for (const auto& span : spans) {
      if (!rendered.empty()) rendered += "|";
      rendered += span.is_english ? "EN:" + span.raw : "JA:" + span.kana;
    }
    ReportEq("googledemo の区間分け (demo=でも合体防止)", "EN:google|JA:でも", rendered);
  }
  // [修正検証] 有名サービス名の固有名詞登録。未登録だと短い実在英単語
  // (site, xiv, pt …) に誤爆して引き裂かれていた。
  {
    const std::vector<ScoredSegmenter::Segment> spans =
        segmenter.SegmentInput("dlsiteni", ScoredSegmenter::Options{});
    std::string rendered;
    for (const auto& span : spans) {
      if (!rendered.empty()) rendered += "|";
      rendered += span.is_english ? "EN:" + span.raw : "JA:" + span.kana;
    }
    ReportEq("dlsiteni の区間分け (DLsite 登録)", "EN:dlsite|JA:に", rendered);
  }
  {
    const std::vector<ScoredSegmenter::Segment> spans =
        segmenter.SegmentInput("pixivni", ScoredSegmenter::Options{});
    std::string rendered;
    for (const auto& span : spans) {
      if (!rendered.empty()) rendered += "|";
      rendered += span.is_english ? "EN:" + span.raw : "JA:" + span.kana;
    }
    ReportEq("pixivni の区間分け (pixiv 登録)", "EN:pixiv|JA:に", rendered);
  }
  // [修正検証] ローマ字として読める英単語 + 引用の「って」は、文脈が無くても
  // 英単語として切り出される (minuteっていう英語)。
  {
    const std::vector<ScoredSegmenter::Segment> spans =
        segmenter.SegmentInput("minutetteiuego", ScoredSegmenter::Options{});
    std::string rendered;
    for (const auto& span : spans) {
      if (!rendered.empty()) rendered += "|";
      rendered += span.is_english ? "EN:" + span.raw : "JA:" + span.kana;
    }
    ReportEq("minutetteiuego の区間分け", "EN:minute|JA:っていうえご",
             rendered);
  }

  // [修正検証] 英字保護が失敗していた 3 入力の区間分け。ここが EN スパンに
  // なっていないと後の保護ケースは何も検証しないので、まず切り分けを固定する。
  {
    const std::vector<ScoredSegmenter::Segment> spans =
        segmenter.SegmentInput("anofacebook", ScoredSegmenter::Options{});
    std::string rendered;
    for (const auto& span : spans) {
      if (!rendered.empty()) rendered += "|";
      rendered += span.is_english ? "EN:" + span.raw : "JA:" + span.kana;
    }
    ReportEq("anofacebook の区間分け", "JA:あの|EN:facebook", rendered);
  }
  {
    const std::vector<ScoredSegmenter::Segment> spans =
        segmenter.SegmentInput("sakkireddit", ScoredSegmenter::Options{});
    std::string rendered;
    for (const auto& span : spans) {
      if (!rendered.empty()) rendered += "|";
      rendered += span.is_english ? "EN:" + span.raw : "JA:" + span.kana;
    }
    ReportEq("sakkireddit の区間分け", "JA:さっき|EN:reddit", rendered);
  }
  {
    const std::vector<ScoredSegmenter::Segment> spans =
        segmenter.SegmentInput("ashitahagoogle", ScoredSegmenter::Options{});
    std::string rendered;
    for (const auto& span : spans) {
      if (!rendered.empty()) rendered += "|";
      rendered += span.is_english ? "EN:" + span.raw : "JA:" + span.kana;
    }
    ReportEq("ashitahagoogle の区間分け", "JA:あしたは|EN:google",
             rendered);
  }
  {
    const std::vector<ScoredSegmenter::Segment> spans =
        segmenter.SegmentInput("googleakaunnto", ScoredSegmenter::Options{});
    std::string rendered;
    for (const auto& span : spans) {
      if (!rendered.empty()) rendered += "|";
      rendered += span.is_english ? "EN:" + span.raw : "JA:" + span.kana;
    }
    ReportEq("googleakaunnto の区間分け", "EN:google|JA:あかうんと", rendered);
  }
  {
    const std::vector<ScoredSegmenter::Segment> spans =
        segmenter.SegmentInput("oregafacebookakaunnto", ScoredSegmenter::Options{});
    std::string rendered;
    for (const auto& span : spans) {
      if (!rendered.empty()) rendered += "|";
      rendered += span.is_english ? "EN:" + span.raw : "JA:" + span.kana;
    }
    ReportEq("oregafacebookakaunnto の区間分け", "JA:おれが|EN:facebook|JA:あかうんと", rendered);
  }
  {
    const std::vector<ScoredSegmenter::Segment> spans =
        segmenter.SegmentInput("discordsa-ba", ScoredSegmenter::Options{});
    std::string rendered;
    for (const auto& span : spans) {
      if (!rendered.empty()) rendered += "|";
      rendered += span.is_english ? "EN:" + span.raw : "JA:" + span.kana;
    }
    ReportEq("discordsa-ba の区間分け", "EN:discord|JA:さーば", rendered);
  }
  {
    const std::vector<ScoredSegmenter::Segment> spans =
        segmenter.SegmentInput("aruteidononagasade", ScoredSegmenter::Options{});
    std::string rendered;
    for (const auto& span : spans) {
      if (!rendered.empty()) rendered += "|";
      rendered += span.is_english ? "EN:" + span.raw : "JA:" + span.kana;
    }
    ReportEq("aruteidononagasade の区間分け (sade除外)", "JA:あるていどのながさで", rendered);
  }
  {
    const std::vector<ScoredSegmenter::Segment> spans =
        segmenter.SegmentInput("nagainndato", ScoredSegmenter::Options{});
    std::string rendered;
    for (const auto& span : spans) {
      if (!rendered.empty()) rendered += "|";
      rendered += span.is_english ? "EN:" + span.raw : "JA:" + span.kana;
    }
    ReportEq("nagainndato の区間分け (inn除外)", "JA:ながいんだと", rendered);
  }

  // [回帰検証] 促音の境界 (英単語の末尾子音と次の文字が同じ) を跨いで
  // 日本語の促音「っ」に潰さない。tab + botan = たっぼたん にしない。
  {
    const std::vector<ScoredSegmenter::Segment> spans =
        segmenter.SegmentInput("tabbotan", ScoredSegmenter::Options{});
    std::string rendered;
    for (const auto& span : spans) {
      if (!rendered.empty()) rendered += "|";
      rendered += span.is_english ? "EN:" + span.raw : "JA:" + span.kana;
    }
    ReportEq("tabbotan の区間分け (促音の境界を跨がない)",
             "EN:tab|JA:ぼたん", rendered);
  }
  {
    const std::vector<ScoredSegmenter::Segment> spans =
        segmenter.SegmentInput("tabbotanwo", ScoredSegmenter::Options{});
    std::string rendered;
    for (const auto& span : spans) {
      if (!rendered.empty()) rendered += "|";
      rendered += span.is_english ? "EN:" + span.raw : "JA:" + span.kana;
    }
    ReportEq("tabbotanwo の区間分け", "EN:tab|JA:ぼたんを", rendered);
  }
  {
    const std::vector<ScoredSegmenter::Segment> spans =
        segmenter.SegmentInput("kyouhatabbotanwoosita",
                               ScoredSegmenter::Options{});
    std::string rendered;
    for (const auto& span : spans) {
      if (!rendered.empty()) rendered += "|";
      rendered += span.is_english ? "EN:" + span.raw : "JA:" + span.kana;
    }
    ReportEq("kyouhatabbotanwoosita の区間分け",
             "JA:きょうは|EN:tab|JA:ぼたんをおした", rendered);
  }
  // 英単語に直接続く日本語の促音の接尾辞・助詞は壊さない。
  {
    const std::vector<ScoredSegmenter::Segment> spans =
        segmenter.SegmentInput("applette", ScoredSegmenter::Options{});
    std::string rendered;
    for (const auto& span : spans) {
      if (!rendered.empty()) rendered += "|";
      rendered += span.is_english ? "EN:" + span.raw : "JA:" + span.kana;
    }
    ReportEq("applette の区間分け (apple + って)", "EN:apple|JA:って",
             rendered);
  }
  {
    const std::vector<ScoredSegmenter::Segment> spans =
        segmenter.SegmentInput("hackerppoi", ScoredSegmenter::Options{});
    std::string rendered;
    for (const auto& span : spans) {
      if (!rendered.empty()) rendered += "|";
      rendered += span.is_english ? "EN:" + span.raw : "JA:" + span.kana;
    }
    ReportEq("hackerppoi の区間分け (hacker + っぽい)",
             "EN:hacker|JA:っぽい", rendered);
  }
  // 日本語の促音 (有声子音でも生じる外来語を含む) はそのまま読む。
  {
    const std::vector<ScoredSegmenter::Segment> spans =
        segmenter.SegmentInput("baggu", ScoredSegmenter::Options{});
    std::string rendered;
    for (const auto& span : spans) {
      if (!rendered.empty()) rendered += "|";
      rendered += span.is_english ? "EN:" + span.raw : "JA:" + span.kana;
    }
    ReportEq("baggu の区間分け (有声促音の外来語)", "JA:ばっぐ", rendered);
  }
  {
    const std::vector<ScoredSegmenter::Segment> spans =
        segmenter.SegmentInput("cchautteppoi", ScoredSegmenter::Options{});
    std::string rendered;
    for (const auto& span : spans) {
      if (!rendered.empty()) rendered += "|";
      rendered += span.is_english ? "EN:" + span.raw : "JA:" + span.kana;
    }
    ReportEq("cchautteppoi の区間分け", "JA:っちゃうってっぽい", rendered);
  }
  // [回帰検証] 促音の安全ルールを英語の保護より先に見る (原本 CompositionDetector
  // の順序)。短い英単語の末尾子音が次の文字と重なって促音になるなら日本語の途中。
  // 以前は保護が先に走り、辞書のマイナー語 teat が安全ルールをすり抜けて
  // kaiteatta が kai + teat + ta に引き裂かれていた。
  for (const auto& [romaji, expected] :
       std::vector<std::pair<std::string, std::string>>{
           {"kaiteatta", "JA:かいてあった"},
           {"kaiteattayo", "JA:かいてあったよ"},
           // 安全ルールは日本語を打っている途中の区間だけに効く。入力の先頭や
           // 英文の途中で、綴りが偶然促音の形になっても英単語のまま。
           {"goodday", "EN:goodday"},
           {"meetthe", "EN:meetthe"},
       }) {
    const std::vector<ScoredSegmenter::Segment> spans =
        segmenter.SegmentInput(romaji, ScoredSegmenter::Options{});
    std::string rendered;
    for (const auto& span : spans) {
      if (!rendered.empty()) rendered += "|";
      rendered += span.is_english ? "EN:" + span.raw : "JA:" + span.kana;
    }
    ReportEq(romaji + " の区間分け (促音の安全ルールが先)", expected, rendered);
  }
  // [回帰検証] 大文字始まりの固有名詞 Todo の直後に助詞が続いても、全体を
  // 英語にしない。propernouns.txt に無かったときは Todoniirete... が 1 つの
  // 英単語に化けていた。
  //
  // Note: 「Todoni」だけを打ったときは、両方とも実在の英字の並びなので
  // まだ 1 語として残る (既存の振る舞い)。分割が確定するのは日本語が続く場合。
  for (const auto& [romaji, expected] :
       std::vector<std::pair<std::string, std::string>>{
           {"Todoniirete", "EN:Todo|JA:にいれて"},
           {"Todowo", "EN:Todo|JA:を"},
           {"sorewoTodoniireteoitekudasai",
            "JA:それを|EN:Todo|JA:にいれておいてください"},
       }) {
    const std::vector<ScoredSegmenter::Segment> spans =
        segmenter.SegmentInput(romaji, ScoredSegmenter::Options{});
    std::string rendered;
    for (const auto& span : spans) {
      if (!rendered.empty()) rendered += "|";
      rendered += span.is_english ? "EN:" + span.raw : "JA:" + span.kana;
    }
    ReportEq(romaji + " の区間分け (Todo + 助詞)", expected, rendered);
  }

  // [回帰検証] 大文字の頭字語 (WSL, CPU, GPU, API) + 日本語。大文字の塊の
  // 途中 (SL, PU, PI) で区切ると全体がローマ字のまま残っていた。
  for (const auto& [romaji, expected] : std::vector<std::pair<std::string, std::string>>{
           {"WSLkarakidoushite", "EN:WSL|JA:からきどうして"},
           {"CPUkarakidoushite", "EN:CPU|JA:からきどうして"},
           {"GPUkarakidoushite", "EN:GPU|JA:からきどうして"},
           {"APIkarakidoushite", "EN:API|JA:からきどうして"},
           {"kyouhaWSLkarakidoushite", "JA:きょうは|EN:WSL|JA:からきどうして"},
           {"APIwo", "EN:API|JA:を"},
           {"CPUga", "EN:CPU|JA:が"},
           {"GPUde", "EN:GPU|JA:で"},
           {"WSLkara", "EN:WSL|JA:から"},
       }) {
    const std::vector<ScoredSegmenter::Segment> spans =
        segmenter.SegmentInput(romaji, ScoredSegmenter::Options{});
    std::string rendered;
    for (const auto& span : spans) {
      if (!rendered.empty()) rendered += "|";
      rendered += span.is_english ? "EN:" + span.raw : "JA:" + span.kana;
    }
    ReportEq(romaji + " の区間分け (頭字語 + 日本語)", expected, rendered);
  }

  // [回帰検証] 小書きかなの x 綴り (xi = ぃ) と、英語の直後の だと (dato)。
  // xi が英語辞書にあったため aidexianara の xi だけが英語になり「アイデ xi アなら」
  // に化けていた。dato が英語辞書にあったため IME + dato が 1 語の英語になっていた。
  for (const auto& [romaji, expected] : std::vector<std::pair<std::string, std::string>>{
           {"aidexianara", "JA:あいでぃあなら"},
           {"aidexia", "JA:あいでぃあ"},
           {"IMEdato", "EN:IME|JA:だと"},
           {"Google IMEdato", "EN:Google IME|JA:だと"},
           {"Xi Jinping", "EN:Xi Jinping"},
           // 小書きかなを l で打つ綴りは kStrictTable に入れない (入れると like が
           // 「有効なローマ字」になり、英語確定の経路を通らなくなる)。
           {"like", "EN:like"},
           {"life", "EN:life"},
           {"late", "EN:late"},
           // 小書きかなの x 綴りを含む読みになる既知の英単語は英語のまま。x を
           // kStrictTable に入れた副作用 (taxi -> たぃ) をここで止める。
           {"taxi", "EN:taxi"},
           {"examine", "EN:examine"},
           {"mixture", "EN:mixture"},
           {"axe", "EN:axe"},
           {"oxide", "EN:oxide"},
           // 単体の x 綴りは小書きかなを打っているので日本語。
           {"xi", "JA:ぃ"},
           {"xu", "JA:ぅ"},
           // 末尾 x の英単語 + 助詞。strict の partial に x 系を入れると box の
           // 末尾 x が「綴りの途中」として valid のままになり、boxni が xn (ん) + i
           // と読めて「ぼんい」に化けていた。
           {"boxni", "EN:box|JA:に"},
           {"indexni", "EN:index|JA:に"},
           {"unixni", "EN:unix|JA:に"},
           {"boxde", "EN:box|JA:で"},
           // 2 文字の末尾 x 英単語 + 助詞 (xn = ん を strict に入れると あんい)。
           {"axni", "EN:ax|JA:に"},
           {"exni", "EN:ex|JA:に"},
           {"oxni", "EN:ox|JA:に"},
       }) {
    const std::vector<ScoredSegmenter::Segment> spans =
        segmenter.SegmentInput(romaji, ScoredSegmenter::Options{});
    std::string rendered;
    for (const auto& span : spans) {
      if (!rendered.empty()) rendered += "|";
      rendered += span.is_english ? "EN:" + span.raw : "JA:" + span.kana;
    }
    ReportEq(romaji + " の区間分け (小書きかな・dato)", expected, rendered);
  }


  // -----------------------------------------------------------------------
  // [2] Mozc ベースライン + 英語ピース保護 & SystemOne & 確定: 一気通貫
  // -----------------------------------------------------------------------
  std::cout << "\n[2] 一気通貫 (入力 -> Meltype ピース -> Mozc ベースライン "
               "+ 英語保護 -> SystemOne -> 確定)\n";

  const std::filesystem::path log_path =
      std::filesystem::temp_directory_path() / "mozkey_session_e2e_systemone.log";
  std::error_code remove_error;
  std::filesystem::remove(log_path, remove_error);

  const std::vector<PipelineCase> cases = {
      // フラッグシップ: 日英日。画面・確定は Mozc の多文節変換をベースに、
      // 英語区間だけがピンポイントで英字になる (グーグル → google)。
      {
          "sakkigoogledeshirabeta",
          {{"さっき", "さっき"}, {"ごおぐえ", "グーグル"},
           {"でしらべた", "で調べた"}},
          {{"さっき"}, {}, {"で調べた", "でしらべた"}},
          /*systemone=*/true,
          {"さっき", "で調べた"},
          "さっき google で調べた",
          {"読み: さっき",
           "左文脈: さっき google \n読み: でしらべた"},
          {{"さっき"}, {"で調べた", "でしらべた"}},
          "SystemOne ON: 各日本語ピースへ左文脈付きで問い合わせ",
      },
      // 同じ入力で SystemOne OFF: 問い合わせはゼロ、確定は Mozc ベースライン
      // + 英語保護だけで完結 (完全ローカル)。
      {
          "sakkigoogledeshirabeta",
          {{"さっき", "さっき"}, {"ごおぐえ", "グーグル"},
           {"でしらべた", "で調べた"}},
          {{"さっき"}, {}, {"で調べた", "でしらべた"}},
          /*systemone=*/false,
          {},
          "さっき google で調べた",
          {},
          {},
          "SystemOne OFF: Mozc ベースライン + 英語保護のみで即時",
      },
      // 英語が先頭: 文字列の端ではスペースを足さないので zenz パイプラン。
      {
          "zenzpaipuran",
          {{"zenz", "ゼンツ"}, {"ぱいぷらん", "パイプラン"}},
          {{}, {"パイプラン", "ぱいぷらん"}},
          /*systemone=*/true,
          {"パイプラン"},
          "zenz パイプラン",
          {"左文脈: zenz \n読み: ぱいぷらん"},
          {{"パイプラン", "ぱいぷらん"}},
          "SystemOne ON: 英語ピース直後の日本語ピース",
      },
      {
          "zenzpaipuran",
          {{"zenz", "ゼンツ"}, {"ぱいぷらん", "パイプラン"}},
          {{}, {"パイプラン", "ぱいぷらん"}},
          /*systemone=*/false,
          {},
          "zenz パイプラン",
          {},
          {},
          "SystemOne OFF: 英語が先頭でも同じ確定文字列",
      },
      // 英語区間は Mozc が kana に変換しようが英字に差し替わる: 漢字変換の
      // 結果 (グーグル) が画面に漏れないことが後おgぇの根絶。
      {
          "kyouhagoogledekensaku",
          {{"きょうは", "きょうは"}, {"ごおぐえ", "グーグル"},
           {"でけんさく", "で検索"}},
          {{"きょうは"}, {}, {"で検索", "でけんさく"}},
          /*systemone=*/true,
          {"きょうは", "で検索"},
          "きょうは google で検索",
          {},
          {},
          "英語区間の Mozc 変換結果 (グーグル) は英字に差し替えられる",
      },
      // Mozc の多文節ベースラインがそのまま出る (今日は を上書きしない)。
      {
          "kyouhagooglede",
          {{"きょうは", "今日は"}, {"ごおぐえ", "グーグル"}, {"で", "で"}},
          {{}, {}, {}},
          /*systemone=*/false,
          {},
          "今日は google で",
          {},
          {},
          "Mozc 多文節ベースライン維持 + 英語ピンポイント保護",
      },
      // 読みが合わない時は何も書き換えない (安全側に倒す)。
      {
          "kyouhagooglede",
          {{"きょうはね", "きょうはね"}},
          {{}, {}, {}},
          /*systemone=*/false,
          {},
          "きょうはね",
          {},
          {},
          "読みがピースと合わない時は Mozc のまま",
          /*skip_english_check=*/true,
      },
      // モデルの選択がベースラインと違うケース: しすてむ ではなく システム が
      // 採用されて初めて「選択」が効いていると言える。
      {
          "shisutemu",
          {{"しすてむ", "しすてむ"}},
          {{"しすてむ", "システム", "シスム"}},
          /*systemone=*/true,
          {"システム"},
          "システム",
          {"読み: しすてむ"},
          {{"しすてむ", "システム", "シスム"}},
          "SystemOne の choice がベースラインを置き換える",
      },
      // 未提示の surface を答えられたら IME はそれを通さず、ベースラインを保つ。
      {
          "shisutemu",
          {{"しすてむ", "しすてむ"}},
          {{"しすてむ", "システム", "シスム"}},
          /*systemone=*/true,
          {kRejectOffer},
          "しすてむ",
          {},
          {},
          "未提示の surface は拒否される",
      },
      // ---- タイポ補正 ("もしかして") の自動昇格 ----
      // odosan は お土産 (おみやげ) の誤読。Mozc は補正候補を6番目にしか
      // 出さず、画面は壊れた表記 おどさん のままだった。補正候補が辞書
      // 候補のどこにいても第1候補へ昇格し、画面も確定も自動で直る。
      // SystemOne OFF = ネットワーク0回でも 0ms で直ることの証明。
      {
          "odosan",
          {{"おどさん", "おどさん"}},
          {{"脅さん", "おどさん", "小戸産", "小戸山", "小渡さん",
            "お土産", "おドサン"}},
          /*systemone=*/false,
          {},
          "お土産",
          {},
          {},
          "タイポ補正が第1候補へ昇格 (SystemOne OFF でも画面・確定が直る)",
          /*skip_english_check=*/false,
          {"お土産"},
      },
      // 補正候補を提示された SystemOne が別の surface を選んだら、その
      // 回答が勝つ (昇格はあくまでベースラインであって決定ではない)。
      {
          "odosan",
          {{"おどさん", "おどさん"}},
          {{"脅さん", "おどさん", "小戸産", "小渡さん", "お土産"}},
          /*systemone=*/true,
          {"脅さん"},
          "脅さん",
          {},
          {},
          "補正候補より SystemOne の回答が優先される",
          /*skip_english_check=*/false,
          {"お土産"},
      },

      // ---- [修正検証] 英字保護が失敗していた全ローマ字の回帰ケース ----
      // 根本原因1: Mozc が辞書にない長い語を複数セグメントに細切れ
      // (facebook → ふぁせぼ + おk)。英語ピースが所有するセグメントは
      // 1個でも複数個でも 1つに畳まれて英字になる。
      {
          "anofacebook",
          {{"あの", "あの"}, {"ふぁせぼ", "ふぁせぼ"}, {"おk", "おk"}},
          {{"あの"}, {}, {}},
          /*systemone=*/true,
          {"あの"},
          "あの facebook",
          {"読み: あの"},
          {{"あの"}},
          "複数セグメントの英語ピース (facebook) をまとめて英字保護",
      },
      // googleakaunnto: unn が英語化せず google アカウント になる
      {
          "googleakaunnto",
          {{"ごおglえ", "google"}, {"あかうんと", "アカウント"}},
          {{}, {"アカウント"}},
          /*systemone=*/false,
          {},
          "google アカウント",
          {},
          {},
          "googleakaunnto: unn 誤爆防止で google アカウント",
      },
      // oregafacebookakaunnto: facebook + アカウント
      {
          "oregafacebookakaunnto",
          {{"おれが", "俺が"}, {"ふぁせぼ", "ふぁせぼ"}, {"おk", "おk"}, {"あかうんと", "アカウント"}},
          {{"俺が"}, {}, {}, {"アカウント"}},
          /*systemone=*/false,
          {},
          "俺が facebook アカウント",
          {},
          {},
          "oregafacebookakaunnto: facebook 複数セグメント + アカウント",
      },
      // discordsa-ba: discord + サーバ
      {
          "discordsa-ba",
          {{"ぢsこrd", "discord"}, {"さーば", "サーバ"}},
          {{}, {"サーバ"}},
          /*systemone=*/false,
          {},
          "discord サーバ",
          {},
          {},
          "discordsa-ba: discords 誤爆防止で discord サーバ",
      },

      // reddit → れっぢ + t。SystemOne OFF でも英字保護だけは効く。
      {
          "sakkireddit",
          {{"さっき", "さっき"}, {"れっぢ", "れっぢ"}, {"t", "t"}},
          {{"さっき"}, {}, {}},
          /*systemone=*/false,
          {},
          "さっき reddit",
          {},
          {},
          "複数セグメントの英語ピース (reddit) + SystemOne OFF",
      },
      // 根本原因2a: 助詞「は」が英語読みと1セグメントに合体
      // (はごおgぇ) → 英語ピース境界で事前スライスしてから保護。
      {
          "ashitahagoogle",
          {{"あした", "明日"}, {"はごおgぇ", "はごおgぇ"}},
          {{"明日は", "あしたは"}, {}},
          /*systemone=*/false,
          {},
          "明日は google",
          {},
          {},
          "英語ピース境界を跨ぐセグメント (は+ごおgぇ) を事前スライス",
      },
      // 根本原因2b (旗艦): 助詞「で」が巻き込まれ (ごおgぇで)、かつ
      // SystemOne の回答が で+しらべた を畳む。スライス → 英字保護 →
      // 回答反映が1つの入力で通ること。
      {
          "sakkigoogledeshirabeta",
          {{"さっき", "さっき"}, {"ごおgぇで", "後おgぇで"},
           {"しらべた", "しらべた"}},
          {{"さっき"}, {}, {"で調べた", "でしらべた"}},
          /*systemone=*/true,
          {"さっき", "で調べた"},
          "さっき google で調べた",
          {"読み: さっき", "左文脈: さっき google \n読み: でしらべた"},
          {{"さっき"}, {"で調べた", "でしらべた"}},
          "助詞巻き込みのスライス + 英字保護 + SystemOne 畳み込み",
      },
      // ---- [修正検証]2つの変換不良 (6164) ----
      // 根本原因1: 辞書に shita/sita が登録済みで stop+shita が英語同士に
      // マージされ sとpした に崩壊。辞書削除 + IsSuruForm 修正で stop|した
      // に分離する。ベースラインは Mozc が stopshita を読む (sとpした)。
      {
          "stopshita",
          {{"sとpした", "sとpした"}},
          {{}, {"した"}},
          /*systemone=*/false,
          {},
          "stop した",
          {},
          {},
          "stopshita: stop(英語) + した(日本語) に分離",
      },
      // 根本原因2: minute はローマ字 (みぬて) として読めるため文脈なしで
      // 英語に勝てず、末尾 t が辞書の minutest に吸われていた。引用の
      // 「って」直前の英単語として切り出す。
      {
          "minutetteiuego",
          {{"みぬてっていうえご", "みぬてっていうえご"}},
          {{}, {"っていう英語"}},
          /*systemone=*/true,
          {"っていう英語"},
          "minute っていう英語",
          {},
          {},
          "minutetteiuego: minute(英語) + っていう英語",
      },
      // ---- [回帰検証] 大文字の頭字語 + 日本語 (6170) ----
      // WSL/CPU/GPU/API は英単語辞書に無くても英語の 1 語として保護され、
      // 直後の「から起動して」は Mozc の変換のままになる。
      {
          "WSLkarakidoushite",
          {{"WSL", "WSL"}, {"から", "から"}, {"きどう", "起動"}, {"して", "して"}},
          {{}, {"起動して", "起動"}},
          /*systemone=*/false,
          {},
          "WSL から起動して",
          {},
          {},
          "WSLkarakidoushite: 頭字語 WSL が英字のまま保護される",
      },
      {
          "CPUkarakidoushite",
          {{"CPU", "CPU"}, {"から", "から"}, {"きどう", "起動"}, {"して", "して"}},
          {{}, {"起動して", "起動"}},
          /*systemone=*/false,
          {},
          "CPU から起動して",
          {},
          {},
          "CPUkarakidoushite: 頭字語 CPU が英字のまま保護される",
      },
      {
          "GPUkarakidoushite",
          {{"GPU", "GPU"}, {"から", "から"}, {"きどう", "起動"}, {"して", "して"}},
          {{}, {"起動して", "起動"}},
          /*systemone=*/false,
          {},
          "GPU から起動して",
          {},
          {},
          "GPUkarakidoushite: 頭字語 GPU が英字のまま保護される",
      },
      {
          "APIkarakidoushite",
          {{"API", "API"}, {"から", "から"}, {"きどう", "起動"}, {"して", "して"}},
          {{}, {"起動して", "起動"}},
          /*systemone=*/false,
          {},
          "API から起動して",
          {},
          {},
          "APIkarakidoushite: 頭字語 API が英字のまま保護される",
      },
      // ---- [回帰検証] CamelCase 複合語の自動スペース挿入 (6170) ----
      // 辞書に無い CamelCase の複合語は 2 つの英語ピースに分かれ、
      // ピースの境界スペースで「ChatGPT Plus」のように空いて確定する。
      {
          "ChatGPTPlus",
          {{"ChatGPT", "ChatGPT"}, {"Plus", "Plus"}},
          {{}, {}},
          /*systemone=*/false,
          {},
          "ChatGPT Plus",
          {},
          {},
          "ChatGPTPlus: CamelCase の語境界で半角スペースが入る",
      },
      {
          "GoogleCloud",
          {{"Google", "Google"}, {"Cloud", "Cloud"}},
          {{}, {}},
          /*systemone=*/false,
          {},
          "Google Cloud",
          {},
          {},
          "GoogleCloud: CamelCase の語境界で半角スペースが入る",
      },
      // ---- [回帰検証] 英語入力中の半角スペース (Space は空白であって変換ではない) ----
      // Mozc の古典キーマップは Composition Space を Convert に割り当てている。
      // 英語を打っている最中に Space を押すと変換が誤発火し、後続の
      // Cliwotsukatte が半角英数のまま流し込まれていた (Github の後ろの
      // スペースで変換モードに入り、以降が全部ローマ字で残る)。
      // Meltype は空白を独立したピースとして切り出し、英語ピースの後ろの
      // 境界スペースとして扱うので、画面も確定も「あの Github Cli を使って」
      // になる。
      {
          "anoGithub Cliwotsukatte",
          {{"あの", "あの"}, {"Github", "Github"}, {" ", " "},
           {"Cli", "Cli"}, {"をつかって", "を使って"}},
          {{"あの"}, {}, {}, {}, {"を使って"}},
          /*systemone=*/false,
          {},
          "あの Github Cli を使って",
          {},
          {},
          "anoGithub Cliwotsukatte: スペースありでも英語ピースが保護される",
      },
      // 同じ入力のスペースなし版。CamelCase の自動分離だけで同じ確定文字列に
      // なることを、スペースあり版と並べて固定する。
      {
          "anoGithubCliwotsukatte",
          {{"あの", "あの"}, {"Github", "Github"}, {"Cli", "Cli"},
           {"をつかって", "を使って"}},
          {{"あの"}, {}, {}, {"を使って"}},
          /*systemone=*/false,
          {},
          "あの Github Cli を使って",
          {},
          {},
          "anoGithubCliwotsukatte: CamelCase 自動分離で同じ確定文字列",
      },
  };

  for (const PipelineCase& test_case : cases) {
    SystemOneLogger logger(/*enabled=*/true);
    logger.SetLogFilePath(log_path.string());
    const PipelineResult result =
        RunPipeline(segmenter, test_case, &logger);

    std::cout << "  -- " << test_case.romaji << " (" << test_case.note << ")\n";
    ReportEq("  確定文字列", test_case.expected_commit,
             JoinProtectedValues(result.protected_segments));
    std::cout << "     preedit: keys=["
              << JoinProtectedKeys(result.protected_segments)
              << "] values=["
              << JoinProtectedValues(result.protected_segments) << "]\n";

    // 英語区間は英字で表示される (ベースラインの Mozc 変換結果は漏れない)。
    const std::vector<ScoredSegmenter::Segment> spans =
        segmenter.SegmentInput(test_case.romaji, ScoredSegmenter::Options{});
    const std::string protected_values =
        JoinProtectedValues(result.protected_segments);
    bool english_intact = true;
    std::string english_detail;
    for (std::size_t i = 0; i < spans.size(); ++i) {
      if (!spans[i].is_english) continue;
      if (protected_values.find(spans[i].raw) == std::string::npos) {
        english_intact = false;
        english_detail = "letters [" + spans[i].raw + "] missing from [" +
                         protected_values + "]";
      }
    }
    if (!test_case.skip_english_check) {
      Report(english_intact, "  英語区間は英字で表示される", english_detail);
    }

    const std::size_t japanese_pieces = static_cast<std::size_t>(
        std::count_if(spans.begin(), spans.end(),
                      [](const ScoredSegmenter::Segment& span) {
                        return !span.is_english;
                      }));
    const bool expect_reject =
        std::find(test_case.answers.begin(), test_case.answers.end(),
                  kRejectOffer) != test_case.answers.end();
    if (test_case.systemone) {
      Report(result.asked_states.size() == japanese_pieces,
             "  各日本語ピースが問い合わせられる",
             "asked=" + std::to_string(result.asked_states.size()) +
                 " japanese=" + std::to_string(japanese_pieces));
      if (!expect_reject) {
        Report(result.asked_states.size() == result.responses.size() &&
                   std::all_of(result.responses.begin(),
                               result.responses.end(),
                               [](const SystemOneResponse& r) { return r.ok; }),
               "  応答が受理される",
               result.responses.empty()
                   ? std::string()
                   : "choice=" + result.responses.back().choice +
                         " latency=" +
                         std::to_string(result.responses.back().latency_msec) +
                         "ms");
      }
      if (!test_case.expected_states.empty()) {
        Report(test_case.expected_states == result.asked_states,
               "  質問文 (左文脈+読み) が期待どおり",
               result.asked_states.empty()
                   ? std::string()
                   : result.asked_states.back());
        for (std::size_t i = 0;
             i < test_case.expected_states.size() && i < result.asked_states.size();
             ++i) {
          ReportEq("    state[" + std::to_string(i) + "]", test_case.expected_states[i],
                   result.asked_states[i]);
        }
      }
      if (!test_case.expected_options.empty()) {
        Report(test_case.expected_options == result.asked_options,
               "  提示する候補リストが辞書候補そのもの",
               "questions=" + std::to_string(result.asked_options.size()));
      }
      if (expect_reject) {
        Report(!result.responses.empty() && !result.responses.back().ok,
               "  未提示の surface は拒否される",
               result.responses.empty()
                   ? std::string()
                   : result.responses.back().error_message);
        Report(mozc::session::JoinPieceValues(result.plan) ==
                   test_case.expected_commit,
               "  拒否されたら第1候補のまま", "");
      }
    } else {
      Report(result.transport_calls == 0 && result.asked_states.empty(),
             "  SystemOne OFF: 一度も問い合わせない",
             "transport_calls=" + std::to_string(result.transport_calls));
    }
  }

  // -----------------------------------------------------------------------
  // [3] systemone.log: 判定結果とレイテンシが残る
  // -----------------------------------------------------------------------
  std::cout << "\n[3] systemone.log の記録\n";
  {
    const std::string log = ReadUtf16LeAsUtf8(log_path);

    Report(!log.empty(), "ログファイルが書かれている", log_path.string());
    Report(log.find("model:") != std::string::npos,
           "モデル名が記録されている", "");
    Report(log.find("reading:") != std::string::npos,
           "読み (質問文) が記録されている", "");
    Report(log.find("options:") != std::string::npos,
           "提示した選択肢の数が記録されている", "");
    Report(log.find("choice=") != std::string::npos,
           "選んだ surface が記録されている", "");
    Report(log.find("latency=") != std::string::npos,
           "レイテンシが記録されている", "");
    Report(log.find("->") != std::string::npos, "応答行が記録されている", "");
    Report(log.find("読み") != std::string::npos,
           "ログに日本語の読みが読める形で書かれている",
           "bytes=" + std::to_string(log.size()));
    // 左文脈は state の先頭。Logger は改行を空白に畳んで 1 行にしている。
    Report(log.find("左文脈: さっき google") != std::string::npos,
           "左文脈がログに残っている", "");
    Report(log.find("システム") != std::string::npos ||
               log.find("で調べた") != std::string::npos,
           "選んだ surface が日本語のまま読める", "");
  }

  // -----------------------------------------------------------------------
  // [4] フォント: 同梱フォントが実際にプロセスに登録される
  // -----------------------------------------------------------------------
  std::cout << "\n[4] 同梱フォントの登録 (Gen Interface JP Display)\n";
  {
    const std::filesystem::path fonts = BundledFontsSourceDirectory();
    for (const std::wstring& name :
         mozc::renderer::win32::BundledFontFileNames()) {
      const std::filesystem::path file =
          fonts / std::filesystem::path(name).filename();
      Report(std::filesystem::exists(file),
             "同梱フォントが存在する: " +
                 std::string(name.begin(), name.end()),
             file.string());
    }

    // パス解決: <server dir>\fonts。
    ReportEq("BundledFontsDirectory は <server dir>\\fonts",
             std::wstring(L"C:\\Program Files (x86)\\JustType\\fonts"),
             mozc::renderer::win32::BundledFontsDirectory(
                 L"C:\\Program Files (x86)\\JustType"));
    ReportEq("末尾の区切りは重ねない",
             std::wstring(L"C:\\JustType\\fonts"),
             mozc::renderer::win32::BundledFontsDirectory(L"C:\\JustType\\"));

    const int registered =
        mozc::renderer::win32::RegisterBundledFonts(fonts.wstring());
    Report(registered == 2, "AddFontResourceExW で 2 面が登録される",
           "registered=" + std::to_string(registered));

    // 登録した面が実際に引けること。これが「ユーザーフォント設定に依存せず
    // 適用される」ことの証拠で、未登録なら false のまま。
    const bool available = mozc::renderer::win32::IsFontFamilyAvailable(
        L"Gen Interface JP Display");
    Report(available, "登録後 family が引ける", "");

    // 存在しないディレクトリでは 0 を返し、呼び出し側は黙って平台フォントに
    // 戻る (renderer の旧動作と同じ)。
    const int missing =
        mozc::renderer::win32::RegisterBundledFonts(
            (fonts / "does-not-exist").wstring());
    Report(missing == 0, "存在しないディレクトリでは 0",
           "registered=" + std::to_string(missing));
  }

  // -----------------------------------------------------------------------
  // [5] キーマップ: Space は常に空白を入力する
  // -----------------------------------------------------------------------
  std::cout << "\n[5] キーマップ: Space は常に空白\n";
  {
    const std::filesystem::path keymap_directory = KeymapDirectory();
    const std::filesystem::path ms_ime = keymap_directory / "ms-ime.tsv";
    std::ifstream input(ms_ime);
    Report(input.good(), "ms-ime.tsv が読める", ms_ime.string());

    std::map<std::string, std::string> precomposition;
    std::map<std::string, std::string> composition;
    std::map<std::string, std::string> conversion;
    std::string line;
    while (std::getline(input, line)) {
      if (line.empty() || line[0] == '#') continue;
      const std::vector<std::string> fields = SplitRow(line);
      if (fields.size() < 3) continue;
      const std::string& state = fields[0];
      const std::string& key = fields[1];
      const std::string& command = fields[2];
      if (state == "Precomposition") {
        precomposition[key] = command;
      } else if (state == "Composition") {
        composition[key] = command;
      } else if (state == "Conversion") {
        conversion[key] = command;
      }
    }

    ReportEq("Precomposition Space のコマンド", "InsertSpace",
             precomposition.count("Space") ? precomposition["Space"] : "");

    // Shift Space は全角空白のまま (InsertSpace と対になる)。
    ReportEq("Precomposition Shift Space のコマンド", "InsertAlternateSpace",
             precomposition.count("Shift Space") ? precomposition["Shift Space"]
                                                 : "");

    // 変換中・候補絞り込み中の Space は変換のままで、これは変えない。
    ReportEq("Composition Space のコマンド", "Convert",
             composition.count("Space") ? composition["Space"] : "");
    ReportEq("Conversion Space のコマンド", "ConvertNext",
             conversion.count("Space") ? conversion["Space"] : "");

    // どのキーマップも Space に再変換を割り当てていないこと。
    bool any_reconvert_on_space = false;
    std::string offending;
    for (const auto& entry :
         std::filesystem::directory_iterator(keymap_directory)) {
      if (entry.path().extension() != ".tsv") continue;
      std::ifstream other(entry.path());
      std::string other_line;
      while (std::getline(other, other_line)) {
        if (other_line.empty() || other_line[0] == '#') continue;
        const std::vector<std::string> fields = SplitRow(other_line);
        if (fields.size() < 3) continue;
        if (fields[1] == "Space" &&
            fields[2].find("Reconvert") != std::string::npos) {
          any_reconvert_on_space = true;
          offending = entry.path().filename().string() + ": " + other_line;
        }
      }
    }
    Report(!any_reconvert_on_space,
           "どのキーマップも Space に再変換を割り当てていない", offending);

    // .tsv が名前を出しているコマンドは keymap.cc が登録しているものだけ。
    const std::vector<std::string> registered =
        RegisteredPrecompositionCommands(
            (std::filesystem::absolute(__FILE__).parent_path() / "keymap.cc")
                .string());
    const bool insert_space_registered =
        std::find(registered.begin(), registered.end(), "InsertSpace") !=
        registered.end();
    Report(insert_space_registered,
           "InsertSpace は keymap.cc の Precomposition コマンドとして登録済み",
           "登録済み " + std::to_string(registered.size()) + " 件");

    const bool reconvert_still_registered =
        std::find(registered.begin(), registered.end(),
                  "ReconvertSelectionOrInsertSpace") != registered.end();
    Report(reconvert_still_registered,
           "ReconvertSelectionOrInsertSpace は別キーマップ用に残している",
           "既定の ms-ime.tsv からは外した");
  }

  // -----------------------------------------------------------------------
  // [6] 辞書: 頻度データで直した 3 つのことを固定する
  //
  // (a) 使役接尾辞パラダイムの幽霊漢字。id.def 488-543 の
  //     動詞,接尾,*,*,五段・サ行 は本来かな書き (させる / さす / させ) なのに、
  //     希少な漢字動詞 (剳す 扠す 扨す 箚す 點す 捺す の 6 語) が同じ表に
  //     活用され、接尾辞側のほぼ 0 のコストを継いだ。その結果 `させ` の
  //     候補が 剳せ (cost 0) や 捺せ (cost 24) で埋まっていた。
  //     判別条件は「接尾辞パラダイム内の漢字表記で、その表記自身の読み
  //     (パラダイム外) が key と連濁差以外で一致しないもの」。これで
  //     掛かる (がかる / かかる = 連濁) や 付ける (づける / つける) のような
  //     正当な接尾辞形を誤爆せず、6 語 x 7 活用形 = 42 件だけを捕捉できる。
  //     コスト 16000 に引き上げてあるので、実候補がある限り出てこない。
  //
  // (b) お任せ のコスト上書き。これは近差タイブレークでは直らない。
  //     辞書上の語コストは既に お任せ 1953 < おまかせ 3435 と正しい順序だが、
  //     変換器は おまかせ を先に出していた。理由は語コストではなく左文脈
  //     コスト: お任せ は lid 2597 (接頭詞,動詞接続)、おまかせ は lid 2601
  //     (接頭詞,名詞接続) で、実測の連接オーバーヘッドはそれぞれ 4521 と
  //     3037 -- お任せ が 1484 不利。語コスト差では埋まらないので、
  //     お任せ の語コストを 200 に下げて合計で上回らせている。
  //     JPDB (お任せ 3569 / おまかせ 21715) と YouTube (16883 / 26591) が
  //     どちらも お任せ を支持する。
  //
  // (c) Mozc の意図的なひらがな優先層は壊していないこと。
  //     わたし / いう / いま / はなし は Mozc があえてひらがなを最安に
  //     している語で、周波数データで漢字へ動かしてはいけない。
  //
  // 注意: ここで見ているのは辞書の見出しコストであり、実変換の 1 位とは
  // 一致しないことがある。変換結果は語コスト + 連接コスト + 文脈で決まる
  // ため、たとえば わたし は辞書コスト 0 で最安だが、文脈によっては 私 が
  // 1 位になる。実変換の検証は quality_regression と実機の変換結果が担う。
  // -----------------------------------------------------------------------
  std::cout << "\n[6] 辞書: 幽霊漢字の排除と お任せ のコスト上書き\n";
  {
    const std::filesystem::path dictionary_directory =
        std::filesystem::absolute(__FILE__).parent_path().parent_path().append(
            "data").append("dictionary_oss");

    // dictionary*.txt を 1 つの行リストにする。どのファイルが runfiles に
    // 入るかは //data/dictionary_oss:cost_repair_test_data が決める (フル
    // 辞書は 129 万行あり、全部読むと size="small" の予算を超える)。
    // ここではファイル名をハードコードせず、実際に届いたものだけを読む。
    // 必要な読みが届いていなければ、下の lid 存在チェックが落ちる。
    std::vector<std::filesystem::path> dictionary_files;
    for (const auto& entry : std::filesystem::directory_iterator(
             dictionary_directory)) {
      const std::string name = entry.path().filename().string();
      if (name.size() == 16 && name.starts_with("dictionary") &&
          name.ends_with(".txt")) {
        dictionary_files.push_back(entry.path());
      }
    }
    std::sort(dictionary_files.begin(), dictionary_files.end());

    std::vector<std::vector<std::string>> rows;
    for (const std::filesystem::path& file : dictionary_files) {
      std::ifstream input(file);
      if (!input) continue;
      std::string line;
      while (std::getline(input, line)) {
        if (line.empty()) continue;
        rows.push_back(SplitRow(line));
      }
    }
    Report(!rows.empty(), "辞書ファイルが読める",
           dictionary_directory.string() + " files=" +
               std::to_string(dictionary_files.size()) + " rows=" +
               std::to_string(rows.size()));

    // (a) 幽霊漢字 42 件が接尾辞パラダイムの中でコスト 16000 に隔離され、
    //     一つも漏れていないことを、実データの lid ごとすべて検証する。
    //
    //     42 = 6 漢字 (剳 扠 扨 箚 點 捺) x 7 活用形。lid は id.def の
    //     使役接尾辞パラダイムで、活用形ごとに 6 語分の id が連番で並ぶ:
    //
    //       490-495  仮定形      させ / 剳せ 扠せ 扨せ 捺せ 箚せ 點せ
    //       498-503  仮定縮約１  さしゃ / 剳しゃ 扠しゃ 扨しゃ 捺しゃ ...
    //       506-511  命令ｅ      させ / 剳せ 扠せ 扨せ 捺せ 箚せ 點せ
    //       514-519  基本形      さす / 剳す 扠す 扨す 捺す 箚す 點す
    //       522-527  未然ウ接続  さそ / 剳そ 扠そ 扨そ 捺そ 箚そ 點そ
    //       530-535  未然形      ささ / 剳さ 扠さ 扨さ 捺さ 箚さ 點さ
    //       538-543  連用形      さし / 剳し 扠し 扨し 捺し 箚し 點し
    //
    //     期待するのは「これら 42 lid がすべて存在し、それぞれの漢字表記が
    //     一つずつ 16000 以上であること」。存在確認も入れることで、将来
    //     id.def が変わって検証対象が 0 件になった時に空振り PASS しない。
    //     (実データは lid ごとに漢字表記が 1 件。捺せ など同じ表記が複数 lid
    //      に出るが、それは別 lid として数える。)
    const std::vector<int> ghost_lids = {
        490, 491, 492, 493, 494, 495, 498, 499, 500, 501, 502, 503,
        506, 507, 508, 509, 510, 511, 514, 515, 516, 517, 518, 519,
        522, 523, 524, 525, 526, 527, 530, 531, 532, 533, 534, 535,
        538, 539, 540, 541, 542, 543};
    const std::vector<std::string> ghost_kanji = {
        "剳", "扠", "扨", "捺", "箚", "點"};

    std::map<int, int> ghost_count_by_lid;
    int ghosts_isolated = 0;
    int ghosts_leaking = 0;
    std::string leak_detail;
    for (const std::vector<std::string>& row : rows) {
      if (row.size() < 5) continue;
      const int lid = std::atoi(row[1].c_str());
      if (std::find(ghost_lids.begin(), ghost_lids.end(), lid) ==
          ghost_lids.end()) {
        continue;
      }
      // 接尾辞パラダイムの漢字表記だけが対象。かな表記 (させ / さす) は
      // 正規の接尾辞形なので数えない。
      const std::string& value = row[4];
      if (value.empty()) continue;
      const std::string first_char = value.substr(0, 3);  // UTF-8 3 bytes
      if (std::find(ghost_kanji.begin(), ghost_kanji.end(), first_char) ==
          ghost_kanji.end()) {
        continue;
      }
      ++ghost_count_by_lid[lid];
      if (std::atoi(row[3].c_str()) >= 16000) {
        ++ghosts_isolated;
      } else {
        ++ghosts_leaking;
        if (leak_detail.empty()) {
          leak_detail = "lid=" + row[1] + " key=" + row[0] + " cost=" +
                        row[3] + " value=" + value;
        }
      }
    }

    // 42 個の lid すべてに漢字表記が存在するか (空振り PASS の防止)。
    std::string missing_lids;
    for (const int lid : ghost_lids) {
      if (ghost_count_by_lid[lid] == 0) {
        if (!missing_lids.empty()) missing_lids += ",";
        missing_lids += std::to_string(lid);
      }
    }
    Report(missing_lids.empty(),
           "幽霊漢字の 42 lid すべてに漢字表記が存在する",
           missing_lids.empty()
               ? "lids=" + std::to_string(ghost_count_by_lid.size())
               : "missing lids: " + missing_lids);

    Report(ghosts_isolated == 42 && ghosts_leaking == 0,
           "幽霊漢字 42 件すべてが接尾辞パラダイムで隔離されている",
           "isolated=" + std::to_string(ghosts_isolated) +
               " (expected 42) leaking=" + std::to_string(ghosts_leaking) +
               " " + leak_detail);

    // 42 lid がそれぞれちょうど 1 件ずつであること (重複や取りこぼしの検出)。
    std::string wrong_count;
    for (const int lid : ghost_lids) {
      if (ghost_count_by_lid[lid] != 1) {
        if (!wrong_count.empty()) wrong_count += ",";
        wrong_count += std::to_string(lid) + "=" +
                       std::to_string(ghost_count_by_lid[lid]);
      }
    }
    Report(wrong_count.empty(),
           "各 lid に漢字表記がちょうど 1 件ずつある",
           wrong_count.empty() ? "42 lids x 1 = 42 rows" : wrong_count);

    // (b) お任せ は おまかせ より安い (コスト上書きの結果)。
    //     実測値: お任せ = 200, おまかせ = 3435。
    //     単純な近差タイブレークでは直らない理由 (lid 2597 の連接コストが
    //     lid 2601 より 1484 高い) はセクション冒頭のコメントを参照。
    int omakase_cost = -1;
    int onomakase_cost = -1;
    for (const std::vector<std::string>& row : rows) {
      if (row.size() < 5 || row[0] != "おまかせ") continue;
      const int cost = std::atoi(row[3].c_str());
      if (row[4] == "おまかせ") {
        if (omakase_cost < 0 || cost < omakase_cost) omakase_cost = cost;
      } else if (row[4] == "お任せ") {
        if (onomakase_cost < 0 || cost < onomakase_cost) {
          onomakase_cost = cost;
        }
      }
    }
    Report(onomakase_cost >= 0 && omakase_cost >= 0 &&
               onomakase_cost < omakase_cost,
           "お任せ が おまかせ より優先される",
           "お任せ=" + std::to_string(onomakase_cost) + " おまかせ=" +
               std::to_string(omakase_cost));

    // (c) コスト上書きが Mozc の意図的なひらがな優先層を壊していないこと。
    //     わたし / いう / いま / はなし は Mozc があえてひらがなを最安に
    //     している語で、周波数データで漢字へ動かしてはいけない。
    //
    //     注意: ここで見ているのは辞書の見出しコストであり、実変換の 1 位
    //     とは一致しないことがある。変換結果は語コストだけでなく連接コスト
    //     と文脈で決まるため、たとえば わたし は辞書コスト 0 で最安だが、
    //     文脈によっては 私 が 1 位になる。実変換の検証は quality_regression
    //     と実機の変換結果が担う (このテストは辞書データの不変条件を守る)。
    //
    //     また あります / できます / ください / なります は辞書の見出しでは
    //     なく活用で生成されるので、ここではなく quality_regression と
    //     実機の変換結果で検証する。
    for (const std::string& reading : {"わたし", "いう", "いま", "はなし"}) {
      std::string cheapest_value;
      int cheapest_cost = -1;
      for (const std::vector<std::string>& row : rows) {
        if (row.size() < 5 || row[0] != reading) continue;
        const int cost = std::atoi(row[3].c_str());
        if (cheapest_cost < 0 || cost < cheapest_cost) {
          cheapest_cost = cost;
          cheapest_value = row[4];
        }
      }
      Report(!cheapest_value.empty() && cheapest_value == reading,
             "ひらがな優先層を保護: " + reading + " の最安表記はひらがな",
             "cheapest=" + cheapest_value + " cost=" +
                 std::to_string(cheapest_cost));
    }
  }

  // -----------------------------------------------------------------------
  // [7] 辞書: 3 大頻度辞書 (JPDB / BCCWJ / YouTubeFreq) から補った現代語彙
  //
  // tools/dictionary/extract_freq_vocab.py が JPDB の (表記, 読み) を背骨に
  // して、BCCWJ と YouTubeFreq に出現する現代語だけを words.tsv へ足して
  // いる。ここではその結果のうち、壊すと痛い 3 点を固定する:
  //
  // (a) 要件で名指しされた 3 つの変換 (かんはつ / かんはついれず /
  //     かんぱついれず)。JPDB は かんぱつ しか持たないので、これらは
  //     extract_freq_vocab.py の CURATED ブロックが供給している。
  //     表記 間髪 は dictionary04.txt に既にあるが、読み かんはつ が無い
  //     ため、補う前は かん + 初 に割れていた。
  //
  // (b) 当て字を足していないこと。抽出の素朴な問い合わせ (「JPDB にあって
  //     辞書に無い (表記, 読み)」) は 14,694 行返し、そのほぼ全部が
  //     為る(する) / 縡(こと) / 維(これ) / 鳥渡(ちょっと) のような当て字で、
  //     入れると する が 為る を出し始めて変換が悪くなる。
  //     extract_freq_vocab.py の V6 がこれを弾く: 同じ読みの「かな書き頻度」
  //     より漢字表記の頻度が低いものは当て字とみなす。
  //     実測 (JPDB):
  //       為る kanji 35871 / kana 12   -> 不採用 (かな が 2989 倍)
  //       縡   kanji 173338 / kana 14  -> 不採用
  //       一緒に kanji 183 / kana 5202 -> 採用 (漢字 が 28 倍)
  //     ここでは不採用側だけを固定する (採用側は変換結果 (a) で見ている)。
  //
  // (c) words.tsv に足した行が gen_aux_dictionary.py の契約を守っていること。
  //     3 列 (key / value / pos) で、pos は PosIds.pos_alias にある名前だけ。
  //     ここが崩れると aux_dictionary の genrule が strict モードで落ちる。
  // -----------------------------------------------------------------------
  std::cout << "\n[7] 辞書: 頻度辞書から補った現代語彙\n";
  {
    // (c) words.tsv の形式と、pos が gen_aux_dictionary.py の別名表にあること。
    //     別名表は同スクリプトの PosIds.pos_alias を写したもの。
    const std::filesystem::path words_tsv =
        std::filesystem::absolute(__FILE__).parent_path().parent_path().append(
            "data").append("dictionary_manual").append("words.tsv");
    std::ifstream words_input(words_tsv);
    Report(words_input.good(), "words.tsv が読める", words_tsv.string());

    const std::vector<std::string> allowed_pos = {
        "名詞",         "固有名詞",   "人名",       "姓",
        "名",           "組織",       "地名",       "名詞サ変",
        "名詞形動",     "副詞",       "連体詞",     "接続詞",
        "感動詞",       "接頭語",     "助数詞",     "接尾一般",
        "接尾人名",     "接尾地名",
    };
    std::map<std::string, std::string> manual_words;  // reading -> surface
    std::string bad_line;
    std::string bad_pos;
    int manual_rows = 0;
    std::string words_line;
    while (std::getline(words_input, words_line)) {
      if (words_line.empty() || words_line[0] == '#') continue;
      const std::vector<std::string> fields = SplitRow(words_line);
      if (fields.size() != 3) {
        if (bad_line.empty()) bad_line = words_line;
        continue;
      }
      ++manual_rows;
      manual_words[fields[0]] = fields[1];
      if (std::find(allowed_pos.begin(), allowed_pos.end(), fields[2]) ==
          allowed_pos.end()) {
        if (bad_pos.empty()) bad_pos = words_line;
      }
    }
    Report(bad_line.empty(), "words.tsv は全行 3 列", bad_line);
    Report(bad_pos.empty(),
           "words.tsv の pos はすべて gen_aux_dictionary.py の別名表にある",
           bad_pos);
    Report(manual_rows > 1500,
           "words.tsv に頻度辞書由来の語彙が入っている",
           "rows=" + std::to_string(manual_rows));

    // (a) 要件で名指しされた 3 つの読みが words.tsv にあること。
    //     値まで見るのは、読みだけ足して表記を間違える事故を防ぐため。
    for (const std::pair<std::string, std::string>& expected :
         std::vector<std::pair<std::string, std::string>>{
             {"かんはつ", "間髪"},
             {"かんはついれず", "間髪入れず"},
             {"かんぱついれず", "間髪入れず"},
         }) {
      const auto found = manual_words.find(expected.first);
      Report(found != manual_words.end() && found->second == expected.second,
             "words.tsv に " + expected.first + " -> " + expected.second,
             found == manual_words.end() ? "読みが無い" : found->second);
    }

    // (b) 当て字を足していないこと。words.tsv に 為る / 縡 / 維 / 鳥渡 が
    //     1 行も無ければよい。これは「V6 を外すと静かに壊れる」ことの
    //     検出器でもある。
    const std::vector<std::pair<std::string, std::string>> ateji = {
        {"する", "為る"},   {"こと", "縡"},   {"これ", "維"},
        {"ちょっと", "鳥渡"}, {"の", "乃"},     {"な", "汝"},
    };
    std::string added_ateji;
    for (const auto& entry : ateji) {
      const auto found = manual_words.find(entry.first);
      if (found != manual_words.end() && found->second == entry.second) {
        if (!added_ateji.empty()) added_ateji += ", ";
        added_ateji += entry.second + "(" + entry.first + ")";
      }
    }
    Report(added_ateji.empty(), "当て字を足していない", added_ateji);

    // (a') 間髪 の 3 経路が実データで引けること。dictionary04.txt の
    //      かんぱつ -> 間髪 と、words.tsv の かんはつ -> 間髪 が両方揃うと
    //      かんはつ / かんぱつ の両方が 間髪 になる。
    //      (変換そのものは quality_regression と実機が担う。ここはデータ側。)
    //
    //      [6] の rows はスコープが閉じているので、ここで自前に読む。
    //      runfiles に入るファイルは //data/dictionary_oss:cost_repair_test_data
    //      が決めているので、届いたものだけを見る (見つからなければ落とす)。
    const std::filesystem::path dictionary_directory =
        std::filesystem::absolute(__FILE__).parent_path().parent_path().append(
            "data").append("dictionary_oss");
    bool kanpatsu_in_dictionary = false;
    std::string kanpatsu_detail = "dictionary04.txt が見つからない";
    for (const auto& entry : std::filesystem::directory_iterator(
             dictionary_directory)) {
      const std::string name = entry.path().filename().string();
      if (name.size() != 16 || !name.starts_with("dictionary") ||
          !name.ends_with(".txt")) {
        continue;
      }
      std::ifstream input(entry.path());
      if (!input) continue;
      std::string line;
      while (std::getline(input, line)) {
        const std::vector<std::string> row = SplitRow(line);
        if (row.size() < 5) continue;
        if (row[0] == "かんぱつ" && row[4] == "間髪") {
          kanpatsu_in_dictionary = true;
          kanpatsu_detail = name + ": lid=" + row[1] + " cost=" + row[3];
          break;
        }
      }
      if (kanpatsu_in_dictionary) break;
    }
    Report(kanpatsu_in_dictionary,
           "かんぱつ -> 間髪 が base dictionary にある", kanpatsu_detail);
  }

  // -----------------------------------------------------------------------
  // [8] 英単語の直後の日本語: 実 Session を実 Engine で回す
  // -----------------------------------------------------------------------
  // 再現する不具合: 大文字で始まる英単語 ("Github", "Cli") を打つと
  // Composer は一時的な半角英数モードに入るが、Mozc の既存規則は
  // 「シフトした文字が 2 つ以上続いたあとの小文字」(AAa) でしか戻らない。
  // つまり 1 文字だけ大文字で始まる語では戻らず、続く助詞の wo が生の
  // アルファベットのまま蓄積され、画面全体がローマ字に巻き戻っていた。
  //
  // ここでは Session を本物の Engine (mock data) と本物のローマ字テーブルで
  // 構築し、キーイベントを Session::SendKey に流し込む。判定も復帰も
  // Session::InsertCharacter の本番経路がそのまま走るので、テスト側で規則を
  // 再現していない。
  std::cout << "\n[8] 英単語の直後の日本語 (Session 実配線)\n";
  {
    struct SessionCase {
      std::string keys;
      std::string expected_preedit;
      std::string note;
    };
    const std::vector<SessionCase> session_cases = {
        {"anoGithub Cliwotsukatte", "あのGithub Cliをつかって",
         "大文字始まりの英単語の直後に助詞 wo が続く (報告された不具合)"},
        {"anoGithubwotsukatte", "あのGithubをつかって",
         "空白なしでも同じ境界で戻る"},
        {"anoGithubwo", "あのGithubを", "助詞 を だけ"},
        {"anoGithubde", "あのGithubで", "助詞 で だけ"},
        {"anoGithubkara", "あのGithubから", "助詞 から"},
        {"anoGithub Client", "あのGithub Client",
         "Client はまだ英単語の途中なので戻らない"},
        {"anoGithub Clinic", "あのGithub Clinic", "Clinic も英単語のまま"},
        {"GoogleCloud", "GoogleCloud", "CamelCase の複合語は英語のまま"},
        {"anoGithub Clide", "あのGithub Clide", "Clide も英単語のまま"},
        {"anoAPIwo", "あのAPIを", "頭字語 API + を"},
        {"anoGPUwo", "あのGPUを", "頭字語 GPU + を"},
        {"anoWSLwo", "あのWSLを", "頭字語 WSL + を"},
        // Shift+大文字のあとも _A にロックされず、続く日本語が日本語として
        // 入力され続けること。TSF 側が一時モードをコンパートメントに書き込み、
        // それを次のキーイベントがサーバーへエコーすると、Composer が
        // comeback_input_mode_ を HALF_ASCII で上書きして復帰先を永久に失って
        // いた。ロックされていれば "sorewoTodonikaite" は
        // "それをTodonikaite" のように生のローマ字のままになる。
        //
        // 漢字化はこの e2e の変換器がモックなので起きない (mock 辞書に
        // かい->書い が無い)。ここで固定するのは「日本語入力に戻ること」
        // であり、漢字化は実辞書を使う経路の責務。
        {"sorewoTodonikaite", "それをTodoにかいて",
         "Shift+大文字のあとも日本語入力に戻る (報告された _A ロック)"},
    };

    for (const SessionCase& session_case : session_cases) {
      absl::StatusOr<std::unique_ptr<Engine>> engine_or =
          MockDataEngineFactory::Create();
      if (!engine_or.ok()) {
        Report(false, session_case.note, "mock engine を作れない");
        continue;
      }
      Session session(**engine_or);

      Config config;
      ConfigHandler::GetDefaultConfig(&config);
      config.set_shift_key_mode_switch(Config::ASCII_INPUT_MODE);
      config.set_preedit_method(Config::ROMAN);
      config.set_use_live_conversion(false);
      config.set_use_dictionary_suggest(false);
      session.SetConfig(config);

      Request request;
      session.SetRequest(request);

      std::shared_ptr<Table> table = std::make_shared<Table>();
      table->InitializeWithRequestAndConfig(request, config);
      session.SetTable(table);

      Command command;
      session.IMEOn(&command);

      // Drive the real key path exactly as the TSF client does: a letter key
      // carries its key code, and Space carries special_key = SPACE (which is
      // what the keymap binds to InsertSpace).
      for (const char key : session_case.keys) {
        command.Clear();
        mozc::commands::Input* input = command.mutable_input();
        input->set_type(mozc::commands::Input::SEND_KEY);
        mozc::commands::KeyEvent* key_event = input->mutable_key();
        if (key == ' ') {
          key_event->set_special_key(mozc::commands::KeyEvent::SPACE);
        } else {
          key_event->set_key_code(
              static_cast<uint32_t>(static_cast<unsigned char>(key)));
        }
        session.SendKey(&command);
      }

      ReportEq(session_case.note, session_case.expected_preedit,
               session.context().composer().GetStringForPreedit());

      // Also commit the composition. A locked _A surfaces here too: the commit
      // would carry raw romaji ("それをTodonikaite") instead of Japanese, and
      // the mode would stay HALF_ASCII, so the user would have to turn the IME
      // off and on again.
      command.Clear();
      mozc::commands::Input* commit_input = command.mutable_input();
      commit_input->set_type(mozc::commands::Input::SEND_KEY);
      commit_input->mutable_key()->set_special_key(
          mozc::commands::KeyEvent::ENTER);
      session.SendKey(&command);
      if (command.output().has_result()) {
        ReportEq(absl::StrCat(session_case.note, " (確定)"),
                 session_case.expected_preedit,
                 command.output().result().value());
      }
      // The mode must be recoverable: the comeback mode is the user's real one.
      Report(session.context().composer().GetComebackInputMode() != HALF_ASCII &&
                 session.context().composer().GetComebackInputMode() !=
                     FULL_ASCII,
             absl::StrCat(session_case.note, " (復帰先が日本語)"),
             "comeback mode is ASCII");
    }

    // Commit through the normal conversion path. Enter accepts whatever is on
    // screen, so it cannot turn kana into kanji by itself; Space is what asks
    // the converter for candidates. This case pins the reported end state
    // "それをTodoに書いて" after the user presses Space then Enter.
    {
      absl::StatusOr<std::unique_ptr<Engine>> engine_or =
          MockDataEngineFactory::Create();
      if (!engine_or.ok()) {
        Report(false, "sorewoTodonikaite を SPACE 変換して確定",
               "mock engine を作れない");
      } else {
        Session session(**engine_or);
        Config config;
        ConfigHandler::GetDefaultConfig(&config);
        config.set_shift_key_mode_switch(Config::ASCII_INPUT_MODE);
        config.set_preedit_method(Config::ROMAN);
        config.set_use_live_conversion(false);
        config.set_use_dictionary_suggest(false);
        session.SetConfig(config);
        Request request;
        session.SetRequest(request);
        std::shared_ptr<Table> table = std::make_shared<Table>();
        table->InitializeWithRequestAndConfig(request, config);
        session.SetTable(table);

        Command command;
        session.IMEOn(&command);
        for (const char key : std::string("sorewoTodonikaite")) {
          command.Clear();
          mozc::commands::Input* input = command.mutable_input();
          input->set_type(mozc::commands::Input::SEND_KEY);
          mozc::commands::KeyEvent* key_event = input->mutable_key();
          key_event->set_key_code(
              static_cast<uint32_t>(static_cast<unsigned char>(key)));
          session.SendKey(&command);
        }
        ReportEq("sorewoTodonikaite の preedit", "それをTodoにかいて",
                 session.context().composer().GetStringForPreedit());

        command.Clear();
        mozc::commands::Input* space_input = command.mutable_input();
        space_input->set_type(mozc::commands::Input::SEND_KEY);
        space_input->mutable_key()->set_special_key(
            mozc::commands::KeyEvent::SPACE);
        session.SendKey(&command);

        command.Clear();
        mozc::commands::Input* enter_input = command.mutable_input();
        enter_input->set_type(mozc::commands::Input::SEND_KEY);
        enter_input->mutable_key()->set_special_key(
            mozc::commands::KeyEvent::ENTER);
        session.SendKey(&command);

        const std::string committed = command.output().has_result()
                                          ? command.output().result().value()
                                          : std::string("<no result>");
        ReportEq("sorewoTodonikaite を SPACE 変換して確定",
                 "それをTodoに書いて", committed);
      }
    }

    // ---------------------------------------------------------------------
    // 英語が一言・二言挟まる網羅ケース。
    //
    // すべてのケースで「英語は打ったとおりの大文字小文字のまま残り、後続の
    // 日本語は日本語として入力され続ける」ことを固定する。ここが壊れると
    // Composer が一時 ASCII モードから戻れず、以後のキーが全部生ローマ字に
    // なる (「変換エンジンが不能になった」と見える症状)。
    // ---------------------------------------------------------------------
    struct MixedCase {
      std::string keys;
      std::string expected_preedit;
      std::string note;
    };
    const std::vector<MixedCase> mixed_cases = {
        {"sisutemuOnehanndanngata", "しすてむOneはんだんがた",
         "日本語→大文字英語 One→日本語"},
        {"sorewoTodoniirete", "それをTodoにいれて", "Todo + に"},
        {"sorewoTodonikaite", "それをTodoにかいて", "Todo + に (報告ケース)"},
        {"anoGithubCliwotsukatte", "あのGithubCliをつかって",
         "CamelCase 2 語 + を"},
        {"anoGithub Cliwotsukatte", "あのGithub Cliをつかって",
         "空白区切りの 2 語 + を"},
        {"kyouhaChatGPTtowotsukatta", "きょうはChatGPTとをつかった",
         "頭字語 ChatGPT + とを (日本語は小文字)"},
        {"AWSkaraGCPheikousuru", "AWSからGCPへいこうする",
         "頭字語 2 つ + から/へ (日本語は小文字)"},
        {"APIwoyobidashitekurenaika", "APIをよびだしてくれないか",
         "頭字語 API + 長い日本語文 (日本語は小文字)"},
        {"kanjigagojiruninaru", "かんじがごじるになる",
         "小文字 gojiru は普通の日本語入力 (英語スイッチなし)"},
        {"douiushikumika", "どういうしくみか", "英語なしの対照ケース"},
        {"juubunseidoga", "じゅうぶんせいどが", "英語なしの対照ケース"},
    };

    for (const MixedCase& mixed : mixed_cases) {
      absl::StatusOr<std::unique_ptr<Engine>> mixed_engine_or =
          MockDataEngineFactory::Create();
      if (!mixed_engine_or.ok()) {
        Report(false, mixed.note, "mock engine を作れない");
        continue;
      }
      Session session(**mixed_engine_or);
      Config config;
      ConfigHandler::GetDefaultConfig(&config);
      config.set_shift_key_mode_switch(Config::ASCII_INPUT_MODE);
      config.set_preedit_method(Config::ROMAN);
      config.set_use_live_conversion(false);
      config.set_use_dictionary_suggest(false);
      session.SetConfig(config);
      Request request;
      session.SetRequest(request);
      std::shared_ptr<Table> table = std::make_shared<Table>();
      table->InitializeWithRequestAndConfig(request, config);
      session.SetTable(table);

      Command command;
      session.IMEOn(&command);
      for (const char key : mixed.keys) {
        command.Clear();
        mozc::commands::Input* input = command.mutable_input();
        input->set_type(mozc::commands::Input::SEND_KEY);
        mozc::commands::KeyEvent* key_event = input->mutable_key();
        if (key == ' ') {
          key_event->set_special_key(mozc::commands::KeyEvent::SPACE);
        } else {
          key_event->set_key_code(
              static_cast<uint32_t>(static_cast<unsigned char>(key)));
        }
        session.SendKey(&command);
      }

      ReportEq(mixed.note, mixed.expected_preedit,
               session.context().composer().GetStringForPreedit());
      // 復帰できていれば入力モードはひらがなに戻っている。
      Report(session.context().composer().GetInputMode() == HIRAGANA,
             mixed.note + " (入力モードがひらがなに戻る)",
             "input mode is not HIRAGANA");
    }

    // A commit must end the temporary run in Hiragana on BOTH slots, and a
    // key event that echoes a mode (what the TSF client really does) must not
    // bring the old mode back. This drives the full Session path with the echo
    // attached, which is how the residue reached users: Composer::Reset()
    // restored the stale |comeback_input_mode_|, the client echoed it, and the
    // user kept typing katakana (or _A) after a commit.
    {
      absl::StatusOr<std::unique_ptr<Engine>> echo_engine_or =
          MockDataEngineFactory::Create();
      if (!echo_engine_or.ok()) {
        Report(false, "確定後のモードエコーでもひらがなを維持",
               "mock engine を作れない");
      } else {
        Session session(**echo_engine_or);
        Config config;
        ConfigHandler::GetDefaultConfig(&config);
        config.set_shift_key_mode_switch(Config::ASCII_INPUT_MODE);
        config.set_preedit_method(Config::ROMAN);
        config.set_use_live_conversion(false);
        config.set_use_dictionary_suggest(false);
        session.SetConfig(config);
        Request request;
        session.SetRequest(request);
        std::shared_ptr<Table> table = std::make_shared<Table>();
        table->InitializeWithRequestAndConfig(request, config);
        session.SetTable(table);

        Command command;
        session.IMEOn(&command);

        // Pin katakana, then layer the temporary Shift+letter ASCII run on it.
        command.Clear();
        session.CompositionModeFullKatakana(&command);
        command.Clear();
        mozc::commands::Input* shift_input = command.mutable_input();
        shift_input->set_type(mozc::commands::Input::SEND_KEY);
        shift_input->mutable_key()->set_key_code('A');
        session.SendKey(&command);
        Report(
            session.context().composer().GetComebackInputMode() ==
                FULL_KATAKANA,
            "一時モード中の復帰先はカタカナ",
            "comeback mode is not FULL_KATAKANA");

        // Commit. Both slots must be Hiragana now.
        command.Clear();
        mozc::commands::Input* enter_input = command.mutable_input();
        enter_input->set_type(mozc::commands::Input::SEND_KEY);
        enter_input->mutable_key()->set_special_key(
            mozc::commands::KeyEvent::ENTER);
        session.SendKey(&command);
        Report(session.context().composer().GetInputMode() == HIRAGANA,
               "確定後の input mode はひらがな",
               "input mode is not HIRAGANA");
        Report(session.context().composer().GetComebackInputMode() == HIRAGANA,
               "確定後の comeback mode はひらがな",
               "comeback mode is not HIRAGANA");

        // Now echo the mode the server itself reports, exactly as the TSF
        // client does: it reads status().comeback_mode() from the previous
        // output and puts it back on the next key event. Before the fix that
        // value was still the stale katakana, so the echo kept the old base
        // alive and the user stayed in katakana after the commit.
        const bool reported_comeback_is_hiragana =
            command.output().has_status() &&
            command.output().status().comeback_mode() == mozc::commands::HIRAGANA;
        Report(reported_comeback_is_hiragana,
               "確定後にサーバーが報告する comeback_mode はひらがな",
               "reported comeback_mode is not HIRAGANA");

        command.Clear();
        mozc::commands::Input* echo_input = command.mutable_input();
        echo_input->set_type(mozc::commands::Input::SEND_KEY);
        echo_input->mutable_key()->set_key_code('a');
        echo_input->mutable_key()->set_mode(mozc::commands::HIRAGANA);
        session.SendKey(&command);
        Report(session.context().composer().GetInputMode() == HIRAGANA,
               "エコー後も input mode はひらがな",
               "echoed mode brought katakana back");
        ReportEq("エコー後も後続はひらがな", "あ",
                 session.context().composer().GetStringForPreedit());
      }
    }

    // A user-chosen alphanumeric mode (Eisu) has no composition to protect, so
    // the revert must not fire there: を stays "wo".
    absl::StatusOr<std::unique_ptr<Engine>> engine_or =
        MockDataEngineFactory::Create();
    if (!engine_or.ok()) {
      Report(false, "英数モードを自分で選んだときは戻らない",
             "mock engine を作れない");
    } else {
      Session session(**engine_or);
      Config config;
      ConfigHandler::GetDefaultConfig(&config);
      config.set_shift_key_mode_switch(Config::ASCII_INPUT_MODE);
      config.set_preedit_method(Config::ROMAN);
      config.set_use_live_conversion(false);
      config.set_use_dictionary_suggest(false);
      session.SetConfig(config);
      Request request;
      session.SetRequest(request);
      std::shared_ptr<Table> table = std::make_shared<Table>();
      table->InitializeWithRequestAndConfig(request, config);
      session.SetTable(table);

      Command command;
      session.IMEOn(&command);
      command.Clear();
      session.CompositionModeHiragana(&command);
      command.Clear();
      session.CompositionModeHalfASCII(&command);

      for (const char key : std::string("Githubwo")) {
        command.Clear();
        mozc::commands::Input* input = command.mutable_input();
        input->set_type(mozc::commands::Input::SEND_KEY);
        mozc::commands::KeyEvent* key_event = input->mutable_key();
        key_event->set_key_code(
            static_cast<uint32_t>(static_cast<unsigned char>(key)));
        session.SendKey(&command);
      }
      ReportEq("英数モードを自分で選んだときは戻らない", "Githubwo",
               session.context().composer().GetStringForPreedit());
    }
  }

  // -----------------------------------------------------------------------
  std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " PASS"
            << (g_failures == 0 ? " (すべて通った)\n" : "\n");
  return g_failures == 0 ? 0 : 1;
}
