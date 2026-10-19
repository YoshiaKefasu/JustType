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

#include "session/meltype_piece_pipeline.h"

#include <cstddef>
#include <string>
#include <vector>

#include "testing/gunit.h"
#include "meltype/scored_segmenter.h"

namespace mozc {
namespace session {
namespace {

using Segment = mozkey::meltype::ScoredSegmenter::Segment;

Segment Ja(const std::string& raw, const std::string& kana) {
  Segment s;
  s.is_english = false;
  s.raw = raw;
  s.kana = kana;
  return s;
}

Segment En(const std::string& raw) {
  Segment s;
  s.is_english = true;
  s.raw = raw;
  return s;
}

// sakkigoogledeshirabeta: Japanese, English, Japanese.
std::vector<Segment> SakkigooglePieces() {
  return {Ja("sakki", "さっき"), En("google"), Ja("deshirabeta", "でしらべた")};
}

TEST(MeltypePiecePipelineTest, EnglishPieceKeepsTypedLetters) {
  const PiecePlan plan =
      BuildPiecePlan({En("google")}, {{}});
  ASSERT_EQ(plan.segments.size(), 1u);
  EXPECT_EQ(plan.segments[0].key, "google");
  EXPECT_EQ(plan.segments[0].value, "google");
  // An English piece never asks a question: it is never converted.
  EXPECT_TRUE(plan.questions.empty());
}

TEST(MeltypePiecePipelineTest, JapanesePieceUsesFirstCandidate) {
  const PiecePlan plan =
      BuildPiecePlan({Ja("deshirabeta", "でしらべた")},
                     {{"で調べた", "でしらべた"}});
  ASSERT_EQ(plan.segments.size(), 1u);
  EXPECT_EQ(plan.segments[0].key, "でしらべた");
  EXPECT_EQ(plan.segments[0].value, "で調べた");
  ASSERT_EQ(plan.questions.size(), 1u);
  EXPECT_EQ(plan.questions[0].reading, "でしらべた");
  EXPECT_EQ(plan.questions[0].left_context, "");
  EXPECT_EQ(plan.questions[0].options,
            (std::vector<std::string>{"で調べた", "でしらべた"}));
}

TEST(MeltypePiecePipelineTest, ReadingIsUsedWhenConverterOffersNothing) {
  const PiecePlan plan = BuildPiecePlan({Ja("deshi", "でし")}, {{}});
  ASSERT_EQ(plan.segments.size(), 1u);
  EXPECT_EQ(plan.segments[0].value, "でし");
  EXPECT_TRUE(plan.questions.empty());
}

TEST(MeltypePiecePipelineTest, BoundarySpacesFollowEnglishBoundaryRule) {
  // Japanese | English | Japanese: a space at both boundaries, none at the
  // string edges.
  const PiecePlan plan =
      BuildPiecePlan(SakkigooglePieces(),
                     {{"さっき"}, {}, {"で調べた", "でしらべた"}});
  EXPECT_EQ(JoinPieceValues(plan), "さっき google で調べた");
  EXPECT_EQ(JoinPieceKeys(plan), "さっき google でしらべた");

  // English first: no leading space at the string edge.
  const PiecePlan leading =
      BuildPiecePlan({En("zenz"), Ja("paipuran", "ぱいぷらん")}, {{}, {"パイプラン"}});
  EXPECT_EQ(JoinPieceValues(leading), "zenz パイプラン");

  // English last: no trailing space at the string edge.
  const PiecePlan trailing =
      BuildPiecePlan({Ja("kyouha", "きょうは"), En("google")}, {{"きょうは"}, {}});
  EXPECT_EQ(JoinPieceValues(trailing), "きょうは google");

  // Japanese only: no spaces anywhere.
  const PiecePlan japanese =
      BuildPiecePlan({Ja("konnnichiha", "こんにちは")}, {{"こんにちは"}});
  EXPECT_EQ(JoinPieceValues(japanese), "こんにちは");
}

TEST(MeltypePiecePipelineTest, BoundarySpaceIsNeverDoubled) {
  // The raw already carries the whitespace, so the rule must not add a
  // second one.
  const PiecePlan plan =
      BuildPiecePlan({Ja("kyouha", "きょうは"), En("google ")}, {{"きょうは"}, {}});
  EXPECT_EQ(JoinPieceValues(plan), "きょうは google ");

  // The raw opens with the whitespace instead: same rule, no doubling.
  const PiecePlan leading_space =
      BuildPiecePlan({Ja("kyouha", "きょうは"), En(" google")}, {{"きょうは"}, {}});
  EXPECT_EQ(JoinPieceValues(leading_space), "きょうは google");

  // Previous piece's raw already carries the closing space.
  const PiecePlan previous_space =
      BuildPiecePlan({En("google "), Ja("paipuran", "ぱいぷらん")}, {{}, {"パイプラン"}});
  EXPECT_EQ(JoinPieceValues(previous_space), "google パイプラン");
}

TEST(MeltypePiecePipelineTest, QuestionLeftContextIsDisplayedPrefixPlusSpace) {
  const PiecePlan plan =
      BuildPiecePlan(SakkigooglePieces(),
                     {{"さっき"}, {}, {"で調べた", "でしらべた"}});
  ASSERT_EQ(plan.questions.size(), 2u);
  EXPECT_EQ(plan.questions[0].piece_index, 0u);
  EXPECT_EQ(plan.questions[0].left_context, "");
  EXPECT_EQ(plan.questions[1].piece_index, 2u);
  EXPECT_EQ(plan.questions[1].left_context, "さっき google ");
  EXPECT_EQ(plan.questions[1].reading, "でしらべた");
}

TEST(MeltypePiecePipelineTest, ApplyPieceChoiceValidatesAndRefreshesLeft) {
  PiecePlan plan =
      BuildPiecePlan(SakkigooglePieces(),
                     {{"さっき"}, {}, {"で調べた", "でしらべた"}});

  // A choice that was never offered changes nothing.
  EXPECT_FALSE(ApplyPieceChoice(&plan, 2, "未提示の表記"));
  EXPECT_EQ(plan.segments[2].value, "で調べた");

  // A stale first piece answer would poison every later left context, so
  // changing the earlier piece refreshes the later question's text.
  EXPECT_TRUE(ApplyPieceChoice(&plan, 0, "さっき"));
  EXPECT_EQ(plan.segments[0].value, "さっき");

  // The offered choice lands and the later left context still reads what is
  // on screen.
  EXPECT_TRUE(ApplyPieceChoice(&plan, 2, "でしらべた"));
  EXPECT_EQ(plan.segments[2].value, "でしらべた");
  ASSERT_EQ(plan.questions.size(), 2u);
  EXPECT_EQ(plan.questions[1].left_context, "さっき google ");
  EXPECT_EQ(JoinPieceValues(plan), "さっき google でしらべた");
}

TEST(MeltypePiecePipelineTest, ApplyPieceChoiceRejectsEnglishPiece) {
  PiecePlan plan =
      BuildPiecePlan(SakkigooglePieces(),
                     {{"さっき"}, {}, {"で調べた"}});
  EXPECT_FALSE(ApplyPieceChoice(&plan, 1, "Google"));
  EXPECT_EQ(plan.segments[1].value, "google");
}

TEST(MeltypePiecePipelineTest, QuestionCarriesLeftContextAndReading) {
  const PiecePlan plan =
      BuildPiecePlan(SakkigooglePieces(),
                     {{"さっき"}, {}, {"で調べた"}});
  ASSERT_EQ(plan.questions.size(), 2u);
  EXPECT_EQ(PieceQuestionState(plan.questions[0]), "読み: さっき");
  EXPECT_EQ(PieceQuestionState(plan.questions[1]),
            "左文脈: さっき google \n読み: でしらべた");
}

TEST(MeltypePiecePipelineTest, LeadingSpaceMatchesJoin) {
  const PiecePlan plan =
      BuildPiecePlan(SakkigooglePieces(), {{"さっき"}, {}, {"で調べた"}});
  EXPECT_EQ(PieceLeadingSpace(plan, 0), "");
  EXPECT_EQ(PieceLeadingSpace(plan, 1), " ");
  EXPECT_EQ(PieceLeadingSpace(plan, 2), " ");
  EXPECT_EQ(PieceLeadingSpace(plan, 3), "");
}

// ---------------------------------------------------------------------------
// Baseline + protection: reading-range alignment and the overlay itself.
// ---------------------------------------------------------------------------

std::string JoinValues(const std::vector<PieceSegment>& segments) {
  std::string out;
  for (const PieceSegment& segment : segments) out += segment.value;
  return out;
}

std::string JoinKeys(const std::vector<PieceSegment>& segments) {
  std::string out;
  for (const PieceSegment& segment : segments) out += segment.key;
  return out;
}

PieceSegment Mozc(const std::string& key, const std::string& value) {
  return PieceSegment{key, value};
}

std::string ReadingSub(const std::string& reading,
                        const PieceReadingRange& range) {
  return reading.substr(range.begin, range.end - range.begin);
}

TEST(MeltypePieceReadingRangesTest, SlicesJapaneseAnchorsAndEnglishBetween) {
  // reading = きょうは + ごおぐえ + で — the English contribution is whatever
  // sits between the two Japanese anchors.
  const std::string reading = "きょうはごおぐえで";
  std::vector<PieceReadingRange> ranges;
  ASSERT_TRUE(ComputePieceReadingRanges(
      {Ja("kyouha", "きょうは"), En("google"), Ja("de", "で")}, reading,
      &ranges));
  ASSERT_EQ(ranges.size(), 3u);
  EXPECT_EQ(reading.substr(ranges[0].begin,
                           ranges[0].end - ranges[0].begin),
            "きょうは");
  EXPECT_EQ(reading.substr(ranges[1].begin,
                           ranges[1].end - ranges[1].begin),
            "ごおぐえ");
  EXPECT_EQ(reading.substr(ranges[2].begin,
                           ranges[2].end - ranges[2].begin),
            "で");
}

TEST(MeltypePieceReadingRangesTest, LeadingAndTrailingEnglishAnchorOnEdges) {
  std::vector<PieceReadingRange> ranges;
  const std::string leading = "zenzぱいぷらん";
  ASSERT_TRUE(ComputePieceReadingRanges(
      {En("zenz"), Ja("paipuran", "ぱいぷらん")}, leading, &ranges));
  ASSERT_EQ(ranges.size(), 2u);
  EXPECT_EQ(ReadingSub(leading, ranges[0]), "zenz");

  const std::string trailing = "きょうはgoogle";
  ASSERT_TRUE(ComputePieceReadingRanges(
      {Ja("kyouha", "きょうは"), En("google")}, trailing, &ranges));
  ASSERT_EQ(ranges.size(), 2u);
  EXPECT_EQ(ReadingSub(trailing, ranges[1]), "google");
}

TEST(MeltypePieceReadingRangesTest, RefusesWhenKanaDoesNotMatchReading) {
  std::vector<PieceReadingRange> ranges;
  EXPECT_FALSE(ComputePieceReadingRanges(
      {Ja("kyouha", "きょうは")}, "きょうはね", &ranges));
  EXPECT_TRUE(ranges.empty());
  // A piece kana that is absent from the reading: refuse, never guess.
  EXPECT_FALSE(ComputePieceReadingRanges(
      {Ja("ni", "に"), En("google")}, "でごおぐ", &ranges));
  EXPECT_TRUE(ranges.empty());
}

TEST(MeltypePieceProtectionTest, EnglishPieceKeepsMozcBaselineAroundIt) {
  // kyouhagooglede -> Mozc's own multi-clause baseline, English pinpointed.
  const std::string reading = "きょうはごおぐえで";
  const std::vector<PieceSegment> baseline = {
      Mozc("きょうは", "今日は"), Mozc("ごおぐえ", "グーグル"),
      Mozc("で", "で")};
  ASSERT_EQ(JoinKeys(baseline), reading);

  const std::vector<PieceSegment> protected_segments =
      ProtectSegmentsWithPieces(
          baseline, {Ja("kyouha", "きょうは"), En("google"), Ja("de", "で")},
          {"", "", ""});

  // Japanese segments keep Mozc's conversion; only the English segment turns
  // into the typed letters with the boundary spaces.
  EXPECT_EQ(JoinValues(protected_segments), "今日は google で");
  EXPECT_EQ(JoinKeys(protected_segments), reading);
}

TEST(MeltypePieceProtectionTest, JapaneseBaselineIsNeverRewritten) {
  // A long Japanese run with Mozc's natural clause split: without a choice
  // the overlay is a no-op, clauses and all.
  const std::vector<PieceSegment> baseline = {
      Mozc("この", "この"), Mozc("ふたつ", "２つ"), Mozc("は", "は")};
  const std::string reading = JoinKeys(baseline);
  std::vector<PieceReadingRange> ranges;
  ASSERT_TRUE(ComputePieceReadingRanges(
      {Ja("konofutatsuha", reading)}, reading, &ranges));

  const std::vector<PieceSegment> protected_segments =
      ProtectSegmentsWithPieces(baseline,
                                {Ja("konofutatsuha", reading)}, {""});
  EXPECT_EQ(protected_segments, baseline);
  EXPECT_EQ(JoinValues(protected_segments), "この２つは");
}

TEST(MeltypePieceProtectionTest, SystemOneChoiceCollapsesItsPieceRun) {
  // Mozc segmented the piece into two clauses; the model's answer replaces
  // the whole piece under the concatenated keys.
  const std::vector<PieceSegment> baseline = {
      Mozc("で", "で"), Mozc("しらべた", "調べた")};
  const std::vector<PieceSegment> protected_segments =
      ProtectSegmentsWithPieces(baseline, {Ja("deshirabeta", "でしらべた")},
                                {"で調べた"});
  ASSERT_EQ(protected_segments.size(), 1u);
  EXPECT_EQ(protected_segments[0].key, "でしらべた");
  EXPECT_EQ(protected_segments[0].value, "で調べた");
}

TEST(MeltypePieceProtectionTest, SegmentStraddlingPieceBoundaryIsKept) {
  // The first segment spans the Japanese|English boundary, and its value
  // (今日はごお, 5 chars) does not mirror its key (きょうはごお, 6 chars),
  // so the boundary slice would be a guess: alignment refuses and Mozc's
  // text stays for every segment.
  const std::vector<PieceSegment> baseline = {
      Mozc("きょうはごお", "今日はごお"), Mozc("ぐえで", "グルで")};
  const std::vector<PieceSegment> protected_segments =
      ProtectSegmentsWithPieces(
          baseline, {Ja("kyouha", "きょうは"), En("google"), Ja("de", "で")},
          {"", "", ""});
  EXPECT_EQ(protected_segments, baseline);
}

TEST(MeltypePieceProtectionTest, MisalignedReadingLeavesEverythingUntouched) {
  // The keys do not carry the piece's kana: alignment refuses and nothing is
  // rewritten.
  const std::vector<PieceSegment> baseline = {
      Mozc("きょうはね", "今日はね")};
  const std::vector<PieceSegment> protected_segments =
      ProtectSegmentsWithPieces(baseline, {Ja("kyouha", "きょうは")}, {""});
  EXPECT_EQ(protected_segments, baseline);
}

TEST(MeltypePieceProtectionTest, EmptyKeySegmentRefusesAlignment) {
  const std::vector<PieceSegment> baseline = {{"", "今日は"},
                                                {"ごおぐえ", "グーグル"}};
  const std::vector<PieceSegment> protected_segments =
      ProtectSegmentsWithPieces(
          baseline, {Ja("kyouha", "きょうは"), En("google")}, {"", ""});
  EXPECT_EQ(protected_segments, baseline);
}

TEST(MeltypePieceProtectionTest, EnglishPieceAcrossTwoSegmentsCollapses) {
  // Mozc split the letters themselves (ごお + ぐえ): the owned run collapses
  // into one segment with the typed letters, because half raw / half Mozc
  // would be worse than either.  The keys concatenate back to the piece's
  // reading, and the boundary spaces follow the edge rule as before.
  const std::vector<PieceSegment> baseline = {
      Mozc("きょうは", "今日は"), Mozc("ごお", "ごお"), Mozc("ぐえ", "ぐえ")};
  const std::vector<PieceSegment> protected_segments =
      ProtectSegmentsWithPieces(
          baseline, {Ja("kyouha", "きょうは"), En("google")}, {"", ""});
  ASSERT_EQ(protected_segments.size(), 2u);
  EXPECT_EQ(JoinKeys(protected_segments), "きょうはごおぐえ");
  EXPECT_EQ(JoinValues(protected_segments), "今日は google");
}

// ---------------------------------------------------------------------------
// The two root causes found by the desktop probes, replayed as unit cases:
// an English piece Mozc split into several segments, and a particle merged
// into the English reading across the piece boundary.
// ---------------------------------------------------------------------------

TEST(MeltypePieceProtectionTest, AnofacebookEnglishRunsAcrossTwoSegments) {
  // anofacebook -> あの facebook.  Mozc chunks the unknown word itself into
  // ふぁせぼ + おk; both segments live inside the English piece, so the run
  // collapses to the typed letters instead of leaking the kana.
  const std::string reading = "あのふぁせぼおk";
  const std::vector<PieceSegment> baseline = {
      Mozc("あの", "あの"), Mozc("ふぁせぼ", "ふぁせぼ"), Mozc("おk", "おk")};
  ASSERT_EQ(JoinKeys(baseline), reading);

  const std::vector<PieceSegment> protected_segments =
      ProtectSegmentsWithPieces(
          baseline, {Ja("ano", "あの"), En("facebook")}, {"", ""});
  ASSERT_EQ(protected_segments.size(), 2u);
  EXPECT_EQ(JoinKeys(protected_segments), reading);
  EXPECT_EQ(JoinValues(protected_segments), "あの facebook");
}

TEST(MeltypePieceProtectionTest, SakkiredditEnglishRunsAcrossTwoSegments) {
  // sakkireddit -> さっき reddit.  Same shape: れっぢ + t both belong to the
  // English piece and merge back into one letters-only segment.
  const std::string reading = "さっきれっぢt";
  const std::vector<PieceSegment> baseline = {
      Mozc("さっき", "さっき"), Mozc("れっぢ", "れっぢ"), Mozc("t", "t")};
  ASSERT_EQ(JoinKeys(baseline), reading);

  const std::vector<PieceSegment> protected_segments =
      ProtectSegmentsWithPieces(
          baseline, {Ja("sakki", "さっき"), En("reddit")}, {"", ""});
  ASSERT_EQ(protected_segments.size(), 2u);
  EXPECT_EQ(JoinKeys(protected_segments), reading);
  EXPECT_EQ(JoinValues(protected_segments), "さっき reddit");
}

TEST(MeltypePieceProtectionTest, AshitahagoogleParticleStraddleIsSliced) {
  // ashitahagoogle -> 明日は google.  Mozc merged は with the English
  // reading: はごおgぇ as one segment spanning the piece boundary.  The
  // slice at the boundary turns it into は | ごおgぇ, so the Japanese
  // baseline (明日 + は) survives and only the letters are rewritten.
  const std::string reading = "あしたはごおgぇ";
  const std::vector<PieceSegment> baseline = {
      Mozc("あした", "明日"), Mozc("はごおgぇ", "はごおgぇ")};
  ASSERT_EQ(JoinKeys(baseline), reading);

  const std::vector<PieceSegment> protected_segments =
      ProtectSegmentsWithPieces(
          baseline, {Ja("ashitaha", "あしたは"), En("google")}, {"", ""});
  ASSERT_EQ(protected_segments.size(), 3u);
  EXPECT_EQ(JoinKeys(protected_segments), reading);
  EXPECT_EQ(JoinValues(protected_segments), "明日は google");
}

TEST(MeltypePieceProtectionTest,
     SakkigoogledeshirabetaSlicesParticleAndProtects) {
  // sakkigoogledeshirabeta -> さっき google で調べた.  The flagship shape
  // from the probe: Mozc merged the particle into the English reading
  // (ごおgぇで as one segment).  The slice separates で, the letters are
  // protected, and SystemOne's answer collapses で + しらべた under the
  // piece's full reading.
  const std::string reading = "さっきごおgぇでしらべた";
  const std::vector<PieceSegment> baseline = {
      Mozc("さっき", "さっき"), Mozc("ごおgぇで", "後おgぇで"),
      Mozc("しらべた", "しらべた")};
  ASSERT_EQ(JoinKeys(baseline), reading);

  const std::vector<PieceSegment> protected_segments =
      ProtectSegmentsWithPieces(
          baseline,
          {Ja("sakki", "さっき"), En("google"),
           Ja("deshirabeta", "でしらべた")},
          {"さっき", "", "で調べた"});
  ASSERT_EQ(protected_segments.size(), 3u);
  EXPECT_EQ(JoinKeys(protected_segments), reading);
  EXPECT_EQ(JoinValues(protected_segments), "さっき google で調べた");
}

TEST(MeltypePieceProtectionTest, EdgeSpacesFollowEnglishBoundaryRule) {
  // English first: trailing space only.
  const std::vector<PieceSegment> leading = ProtectSegmentsWithPieces(
      {Mozc("zenz", "ゼンツ"), Mozc("ぱいぷらん", "パイプラン")},
      {En("zenz"), Ja("paipuran", "ぱいぷらん")}, {"", ""});
  EXPECT_EQ(JoinValues(leading), "zenz パイプラン");

  // English last: leading space only.
  const std::vector<PieceSegment> trailing = ProtectSegmentsWithPieces(
      {Mozc("きょうは", "きょうは"), Mozc("ごおぐえ", "グーグル")},
      {Ja("kyouha", "きょうは"), En("google")}, {"", ""});
  EXPECT_EQ(JoinValues(trailing), "きょうは google");
}

// ---------------------------------------------------------------------------
// Mozc's own typo correction ("もしかして").
//
// The correction is promoted only when Mozc itself produced it for the
// piece's reading; nothing here invents a surface.
// ---------------------------------------------------------------------------

TEST(MeltypePieceTypoCorrectionTest, CorrectionIsPromotedToFirstOption) {
  // "odosan" is a mis-reading of "お土産" (おみやげ); Mozc puts the correction
  // at index 5, so the promotion has to move it, not just keep it.
  const OrderedPieceCandidates ordered = OrderPieceCandidates({
      {"脅さん", false},
      {"おどさん", false},
      {"小戸産", false},
      {"小戸山", false},
      {"小渡さん", false},
      {"お土産", true},
      {"おドサン", false},
  });
  ASSERT_FALSE(ordered.surfaces.empty());
  EXPECT_EQ(ordered.surfaces.front(), "お土産");
  EXPECT_TRUE(ordered.typo_correction);
  // Mozc's own order survives behind it, and nothing is added or dropped.
  EXPECT_EQ(ordered.surfaces,
            (std::vector<std::string>{"お土産", "脅さん", "おどさん",
                                      "小戸産", "小戸山", "小渡さん",
                                      "おドサン"}));
}

TEST(MeltypePieceTypoCorrectionTest, NoCorrectionKeepsMozcOrder) {
  const OrderedPieceCandidates ordered = OrderPieceCandidates({
      {"環境", false},
      {"カンキョウ", false},
  });
  EXPECT_EQ(ordered.surfaces,
            (std::vector<std::string>{"環境", "カンキョウ"}));
  EXPECT_FALSE(ordered.typo_correction);
}

TEST(MeltypePieceTypoCorrectionTest, EmptyInputStaysEmpty) {
  const OrderedPieceCandidates ordered = OrderPieceCandidates({});
  EXPECT_TRUE(ordered.surfaces.empty());
  EXPECT_FALSE(ordered.typo_correction);
}

TEST(MeltypePieceTypoCorrectionTest, CorrectionAlreadyFirstIsNotDuplicated) {
  // "ふいんき" is corrected to "雰囲気", which Mozc already ranks first.
  const OrderedPieceCandidates ordered = OrderPieceCandidates({
      {"雰囲気", true},
      {"フインキ", false},
  });
  EXPECT_EQ(ordered.surfaces, (std::vector<std::string>{"雰囲気", "フインキ"}));
  EXPECT_TRUE(ordered.typo_correction);
}

TEST(MeltypePieceTypoCorrectionTest, PlanBaselineIsTheCorrection) {
  // The screen at 0ms is the plan's baseline value, so the correction has to
  // be that value — not Mozc's own first candidate.
  const PiecePlan plan =
      BuildPiecePlan({Ja("odosan", "おどさん")},
                     {{"お土産", "脅さん", "おどさん"}}, {true});
  ASSERT_EQ(plan.segments.size(), 1u);
  EXPECT_EQ(plan.segments[0].value, "お土産");
  ASSERT_EQ(plan.questions.size(), 1u);
  EXPECT_TRUE(plan.questions[0].typo_correction);
  EXPECT_EQ(plan.questions[0].options.front(), "お土産");
}

TEST(MeltypePieceTypoCorrectionTest, DisplayChoiceUsesCorrectionWithoutAnswer) {
  const PiecePlan plan =
      BuildPiecePlan({Ja("odosan", "おどさん")},
                     {{"お土産", "脅さん"}}, {true});
  // No SystemOne answer yet: the correction is what the piece displays.
  const std::vector<std::string> choices =
      ResolvePieceDisplayChoices(plan, {""});
  ASSERT_EQ(choices.size(), 1u);
  EXPECT_EQ(choices[0], "お土産");
}

TEST(MeltypePieceTypoCorrectionTest, SystemOneAnswerBeatsTheCorrection) {
  const PiecePlan plan =
      BuildPiecePlan({Ja("odosan", "おどさん")},
                     {{"お土産", "脅さん"}}, {true});
  // The model saw the correction as an option and chose otherwise: its answer
  // wins, otherwise the question would be pointless.
  const std::vector<std::string> choices =
      ResolvePieceDisplayChoices(plan, {"脅さん"});
  ASSERT_EQ(choices.size(), 1u);
  EXPECT_EQ(choices[0], "脅さん");
}

TEST(MeltypePieceTypoCorrectionTest, NoCorrectionLeavesMozcBaseline) {
  const PiecePlan plan =
      BuildPiecePlan({Ja("kannkyou", "かんきょう")},
                     {{"環境", "カンキョウ"}});
  const std::vector<std::string> choices =
      ResolvePieceDisplayChoices(plan, {""});
  ASSERT_EQ(choices.size(), 1u);
  // "" means "keep Mozc's own multi-clause baseline on screen".
  EXPECT_TRUE(choices[0].empty());
}

TEST(MeltypePieceTypoCorrectionTest, CorrectionCollapsesThePieceOnScreen) {
  // The protection overlay only rewrites a piece when it carries a choice, so
  // the promotion reaches the screen through this path: Mozc's mis-converted
  // "おどさん" must become "お土産".
  const std::vector<PieceSegment> baseline = {Mozc("おどさん", "おどさん")};
  const std::vector<Segment> pieces = {Ja("odosan", "おどさん")};
  const PiecePlan plan =
      BuildPiecePlan(pieces, {{"お土産", "おどさん"}}, {true});
  const std::vector<std::string> choices =
      ResolvePieceDisplayChoices(plan, {""});

  const std::vector<PieceSegment> protected_segments =
      ProtectSegmentsWithPieces(baseline, pieces, choices);
  ASSERT_EQ(protected_segments.size(), 1u);
  EXPECT_EQ(JoinValues(protected_segments), "お土産");
  // The reading is untouched: only the surface changes.
  EXPECT_EQ(JoinKeys(protected_segments), "おどさん");
}

TEST(MeltypePieceTypoCorrectionTest, CorrectionInsideAJapaneseSentence) {
  // Only the corrected piece changes; the other Japanese clauses stay Mozc's.
  const std::vector<Segment> pieces = {Ja("kyouha", "きょうは"),
                                       Ja("odosan", "おどさん"),
                                       Ja("datta", "だった")};
  const std::vector<PieceSegment> baseline = {Mozc("きょうは", "今日は"),
                                             Mozc("おどさん", "おどさん"),
                                             Mozc("だった", "だった")};
  const PiecePlan plan =
      BuildPiecePlan(pieces, {{"今日は"}, {"お土産", "おどさん"}, {"だった"}},
                     {false, true, false});
  const std::vector<std::string> choices =
      ResolvePieceDisplayChoices(plan, {"", "", ""});

  const std::vector<PieceSegment> protected_segments =
      ProtectSegmentsWithPieces(baseline, pieces, choices);
  EXPECT_EQ(JoinValues(protected_segments), "今日はお土産だった");
}

}  // namespace
}  // namespace session
}  // namespace mozc
