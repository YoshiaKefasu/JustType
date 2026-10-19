#include "rewriter/zenz_feedback_candidate_rewriter.h"

#include <string>
#include <utility>

#include "base/file/temp_dir.h"
#include "base/system_util.h"
#include "converter/attribute.h"
#include "converter/candidate.h"
#include "converter/segments.h"
#include "protocol/config.pb.h"
#include "request/conversion_request.h"
#include "session/zenz_feedback_store.h"
#include "testing/gunit.h"
#include "testing/mozctest.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace mozc {
namespace {
#if defined(_WIN32)

// Pins the profile directory through SystemUtil instead of relying on the
// LocalLow layout.  The product directory name is resolved dynamically
// (JustType for a fresh install, Mozc for a pre-rename one).
class ScopedUserProfileForZenzFeedbackCandidateRewriterTest {
 public:
  ScopedUserProfileForZenzFeedbackCandidateRewriterTest()
      : old_profile_dir_(SystemUtil::GetUserProfileDirectory()),
        profile_dir_(mozc::testing::MakeTempDirectoryOrDie()) {
    if (profile_dir_.path().empty()) {
      return;
    }
    SystemUtil::SetUserProfileDirectory(profile_dir_.path());
    ok_ = true;
  }

  ~ScopedUserProfileForZenzFeedbackCandidateRewriterTest() {
    SystemUtil::SetUserProfileDirectory(old_profile_dir_);
  }

  bool ok() const { return ok_; }

 private:
  bool ok_ = false;
  std::string old_profile_dir_;
  TempDirectory profile_dir_;
};

void AddSegment(absl::string_view key,
                absl::string_view value,
                Segments* segments) {
  Segment* segment = segments->push_back_segment();
  segment->set_key(key);

  converter::Candidate* candidate = segment->add_candidate();
  candidate->key = std::string(key);
  candidate->content_key = std::string(key);
  candidate->value = std::string(value);
  candidate->content_value = std::string(value);
  candidate->attributes = converter::Attribute::BEST_CANDIDATE;
}

ConversionRequest CreateZenzFeedbackConversionRequest(
    bool enable_minimum_key_length = true) {
  config::Config config;
  config.set_use_zenz_feedback_learning(true);
  config.set_use_zenz_feedback_reuse(true);
  config.set_use_zenz_feedback_min_key_length(enable_minimum_key_length);
  config.set_history_learning_level(config::Config::DEFAULT_HISTORY);

  ConversionRequest::Options options;
  options.request_type = ConversionRequest::CONVERSION;
  options.enable_user_history_for_conversion = true;
  options.incognito_mode = false;

  return ConversionRequestBuilder()
      .SetConfig(config)
      .SetOptions(std::move(options))
      .SetRequestType(ConversionRequest::CONVERSION)
      .SetKey("かれはてんてきです")
      .Build();
}

TEST(ZenzFeedbackCandidateRewriterTest,
     DoesNotRewriteMultiSegmentConversionEvenWhenFullKeyHasAcceptedFeedback) {
  ScopedUserProfileForZenzFeedbackCandidateRewriterTest profile;
  ASSERT_TRUE(profile.ok());

  session::ZenzFeedbackStore store;
  // Feedback learned for the full key must not collapse Mozc's phrase
  // boundaries.  Phrase boundaries are owned by the converter, not by this
  // feedback rewriter.
  store.RecordAccepted("かれはてんてきです", "japanese_only", "彼は天敵です");

  Segments segments;
  AddSegment("かれは", "彼は", &segments);
  AddSegment("てんてきです", "点滴です", &segments);

  ASSERT_EQ(segments.conversion_segments_size(), 2);
  ASSERT_EQ(segments.conversion_segment(0).candidate(0).value, "彼は");
  ASSERT_EQ(segments.conversion_segment(1).candidate(0).value, "点滴です");

  const ConversionRequest request = CreateZenzFeedbackConversionRequest();

  ZenzFeedbackCandidateRewriter rewriter;
  EXPECT_EQ(rewriter.capability(request), RewriterInterface::CONVERSION);
  EXPECT_FALSE(rewriter.Rewrite(request, &segments));

  ASSERT_EQ(segments.conversion_segments_size(), 2);
  EXPECT_EQ(segments.conversion_segment(0).key(), "かれは");
  EXPECT_EQ(segments.conversion_segment(0).candidate(0).value, "彼は");
  EXPECT_EQ(segments.conversion_segment(1).key(), "てんてきです");
  EXPECT_EQ(segments.conversion_segment(1).candidate(0).value, "点滴です");
}

TEST(ZenzFeedbackCandidateRewriterTest,
     ShortReadingDoesNotReuseFeedbackWithDefaultSevenCharacterPolicy) {
  ScopedUserProfileForZenzFeedbackCandidateRewriterTest profile;
  ASSERT_TRUE(profile.ok());
  session::ZenzFeedbackStore store;
  store.RecordAccepted("はし", "empty", "橋");

  Segments segments;
  AddSegment("はし", "箸", &segments);
  const ConversionRequest request = CreateZenzFeedbackConversionRequest();
  ZenzFeedbackCandidateRewriter rewriter;
  EXPECT_FALSE(rewriter.Rewrite(request, &segments));
  ASSERT_EQ(segments.conversion_segment(0).candidates_size(), 1);
  EXPECT_EQ(segments.conversion_segment(0).candidate(0).value, "箸");
}

TEST(ZenzFeedbackCandidateRewriterTest,
     InsertsSingleSegmentConversionByFeedbackAdjustedCost) {
  ScopedUserProfileForZenzFeedbackCandidateRewriterTest profile;
  ASSERT_TRUE(profile.ok());

  session::ZenzFeedbackStore store;
  // The rewriter queries feedback with context_class="empty".  Feedback learned
  // while safe left context existed, such as japanese_only, must still promote
  // the same full-key correction in ordinary conversion.
  store.RecordAccepted("かれはてんてきです", "japanese_only", "彼は天敵です");

  Segments segments;
  AddSegment("かれはてんてきです", "彼は点滴です", &segments);

  ASSERT_EQ(segments.conversion_segments_size(), 1);
  ASSERT_EQ(segments.conversion_segment(0).candidate(0).value, "彼は点滴です");

  const ConversionRequest request = CreateZenzFeedbackConversionRequest();

  ZenzFeedbackCandidateRewriter rewriter;
  EXPECT_EQ(rewriter.capability(request), RewriterInterface::CONVERSION);
  EXPECT_TRUE(rewriter.Rewrite(request, &segments));

  ASSERT_EQ(segments.conversion_segments_size(), 1);

  const Segment& segment = segments.conversion_segment(0);
  EXPECT_EQ(segment.key(), "かれはてんてきです");
  ASSERT_GE(segment.candidates_size(), 2);

  // With the default zero-cost test candidate, one accepted Zenz feedback
  // entry is not allowed to hard-promote itself above the existing Mozc top
  // candidate.  It participates in the same candidate set instead.
  EXPECT_EQ(segment.candidate(0).key, "かれはてんてきです");
  EXPECT_EQ(segment.candidate(0).content_key, "かれはてんてきです");
  EXPECT_EQ(segment.candidate(0).value, "彼は点滴です");
  EXPECT_EQ(segment.candidate(0).content_value, "彼は点滴です");
  EXPECT_TRUE(segment.candidate(0).attributes &
              converter::Attribute::BEST_CANDIDATE);

  EXPECT_EQ(segment.candidate(1).key, "かれはてんてきです");
  EXPECT_EQ(segment.candidate(1).content_key, "かれはてんてきです");
  EXPECT_EQ(segment.candidate(1).value, "彼は天敵です");
  EXPECT_EQ(segment.candidate(1).content_value, "彼は天敵です");
  EXPECT_TRUE(segment.candidate(1).attributes & converter::Attribute::RERANKED);
  EXPECT_TRUE(segment.candidate(1).attributes &
              converter::Attribute::USER_SEGMENT_HISTORY_REWRITER);
  EXPECT_FALSE(segment.candidate(1).attributes &
               converter::Attribute::BEST_CANDIDATE);
}

TEST(ZenzFeedbackCandidateRewriterTest,
     PromotesStrongFeedbackWhenAdjustedCostBecomesBest) {
  ScopedUserProfileForZenzFeedbackCandidateRewriterTest profile;
  ASSERT_TRUE(profile.ok());

  session::ZenzFeedbackStore store;
  store.RecordAccepted("かれはてんてきです", "japanese_only", "彼は天敵です");
  store.RecordAccepted("かれはてんてきです", "japanese_only", "彼は天敵です");

  Segments segments;
  AddSegment("かれはてんてきです", "彼は点滴です", &segments);
  segments.mutable_conversion_segment(0)->mutable_candidate(0)->cost = 1000;
  segments.mutable_conversion_segment(0)->mutable_candidate(0)->wcost = 1000;

  const ConversionRequest request = CreateZenzFeedbackConversionRequest();

  ZenzFeedbackCandidateRewriter rewriter;
  EXPECT_TRUE(rewriter.Rewrite(request, &segments));

  const Segment& segment = segments.conversion_segment(0);
  ASSERT_GE(segment.candidates_size(), 2);
  EXPECT_EQ(segment.candidate(0).value, "彼は天敵です");
  EXPECT_TRUE(segment.candidate(0).attributes & converter::Attribute::RERANKED);
  EXPECT_TRUE(segment.candidate(0).attributes &
              converter::Attribute::USER_SEGMENT_HISTORY_REWRITER);
  EXPECT_TRUE(segment.candidate(0).attributes &
              converter::Attribute::BEST_CANDIDATE);
  EXPECT_EQ(segment.candidate(1).value, "彼は点滴です");
  EXPECT_FALSE(segment.candidate(1).attributes &
               converter::Attribute::BEST_CANDIDATE);
}

TEST(ZenzFeedbackCandidateRewriterTest,
     DoesNotRewriteMultiSegmentConversionWithoutFeedback) {
  ScopedUserProfileForZenzFeedbackCandidateRewriterTest profile;
  ASSERT_TRUE(profile.ok());

  // Multi-segment conversion is outside this rewriter's responsibility.
  // Phrase boundaries must be preserved regardless of feedback availability.

  Segments segments;
  AddSegment("かれは", "彼は", &segments);
  AddSegment("てんてきです", "点滴です", &segments);

  const ConversionRequest request = CreateZenzFeedbackConversionRequest();

  ZenzFeedbackCandidateRewriter rewriter;
  EXPECT_FALSE(rewriter.Rewrite(request, &segments));

  ASSERT_EQ(segments.conversion_segments_size(), 2);
  EXPECT_EQ(segments.conversion_segment(0).key(), "かれは");
  EXPECT_EQ(segments.conversion_segment(1).key(), "てんてきです");
}

TEST(ZenzFeedbackCandidateRewriterTest,
     RepositionsExistingCandidateByFeedbackAdjustedCost) {
  ScopedUserProfileForZenzFeedbackCandidateRewriterTest profile;
  ASSERT_TRUE(profile.ok());

  session::ZenzFeedbackStore store;
  store.RecordAccepted("かれはてんてきです", "japanese_only", "彼は天敵です");

  Segments segments;
  AddSegment("かれはてんてきです", "彼は点滴です", &segments);
  Segment* segment = segments.mutable_conversion_segment(0);
  segment->mutable_candidate(0)->cost = 0;
  segment->mutable_candidate(0)->wcost = 0;

  converter::Candidate* existing = segment->add_candidate();
  existing->key = "かれはてんてきです";
  existing->content_key = "かれはてんてきです";
  existing->value = "彼は天敵です";
  existing->content_value = "彼は天敵です";
  existing->cost = 1600;
  existing->wcost = 1600;

  const ConversionRequest request = CreateZenzFeedbackConversionRequest();

  ZenzFeedbackCandidateRewriter rewriter;
  EXPECT_TRUE(rewriter.Rewrite(request, &segments));

  ASSERT_EQ(segment->candidates_size(), 2);
  EXPECT_EQ(segment->candidate(0).value, "彼は点滴です");
  EXPECT_TRUE(segment->candidate(0).attributes &
              converter::Attribute::BEST_CANDIDATE);
  EXPECT_EQ(segment->candidate(1).value, "彼は天敵です");
  EXPECT_EQ(segment->candidate(1).cost, 1100);
  EXPECT_TRUE(segment->candidate(1).attributes & converter::Attribute::RERANKED);
  EXPECT_TRUE(segment->candidate(1).attributes &
              converter::Attribute::USER_SEGMENT_HISTORY_REWRITER);
  EXPECT_FALSE(segment->candidate(1).attributes &
               converter::Attribute::BEST_CANDIDATE);
}

TEST(ZenzFeedbackCandidateRewriterTest,
     DoesNotPromoteSingleSegmentConversionWithSensitiveLikeFeedbackOnly) {
  ScopedUserProfileForZenzFeedbackCandidateRewriterTest profile;
  ASSERT_TRUE(profile.ok());

  session::ZenzFeedbackStore store;
  store.RecordAccepted("かれはてんてきです", "sensitive_like", "彼は天敵です");
  store.RecordAccepted("かれはてんてきです", "sensitive_like", "彼は天敵です");
  store.RecordAccepted("かれはてんてきです", "sensitive_like", "彼は天敵です");

  Segments segments;
  AddSegment("かれはてんてきです", "彼は点滴です", &segments);

  const ConversionRequest request = CreateZenzFeedbackConversionRequest();

  ZenzFeedbackCandidateRewriter rewriter;
  EXPECT_FALSE(rewriter.Rewrite(request, &segments));

  ASSERT_EQ(segments.conversion_segments_size(), 1);

  const Segment& segment = segments.conversion_segment(0);
  ASSERT_EQ(segment.candidates_size(), 1);
  EXPECT_EQ(segment.key(), "かれはてんてきです");
  EXPECT_EQ(segment.candidate(0).key, "かれはてんてきです");
  EXPECT_EQ(segment.candidate(0).value, "彼は点滴です");
}

TEST(ZenzFeedbackCandidateRewriterTest,
     KeepsSoftRejectedFeedbackCandidateButReducesItsBonus) {
  ScopedUserProfileForZenzFeedbackCandidateRewriterTest profile;
  ASSERT_TRUE(profile.ok());

  session::ZenzFeedbackStore store;
  store.RecordAccepted("かれはてんてきです", "japanese_only", "彼は天敵です");
  store.RecordRejected("かれはてんてきです", "japanese_only", "彼は天敵です",
                       "space_revert_zenz_to_mozc");

  Segments segments;
  AddSegment("かれはてんてきです", "彼は点滴です", &segments);
  Segment* segment = segments.mutable_conversion_segment(0);
  segment->mutable_candidate(0)->cost = 0;
  segment->mutable_candidate(0)->wcost = 0;

  converter::Candidate* existing = segment->add_candidate();
  existing->key = "かれはてんてきです";
  existing->content_key = "かれはてんてきです";
  existing->value = "彼は天敵です";
  existing->content_value = "彼は天敵です";
  existing->cost = 1600;
  existing->wcost = 1600;

  const ConversionRequest request = CreateZenzFeedbackConversionRequest();

  ZenzFeedbackCandidateRewriter rewriter;
  EXPECT_TRUE(rewriter.Rewrite(request, &segments));

  ASSERT_EQ(segment->candidates_size(), 2);
  EXPECT_EQ(segment->candidate(0).value, "彼は点滴です");
  EXPECT_TRUE(segment->candidate(0).attributes &
              converter::Attribute::BEST_CANDIDATE);

  // A Space-derived rejection is a soft negative signal.  It reduces the
  // accepted feedback bonus, but it must not delete or hard-suppress the
  // candidate while the total feedback score remains positive.
  EXPECT_EQ(segment->candidate(1).value, "彼は天敵です");
  EXPECT_EQ(segment->candidate(1).cost, 1175);
  EXPECT_EQ(segment->candidate(1).wcost, 1175);
  EXPECT_TRUE(segment->candidate(1).attributes & converter::Attribute::RERANKED);
  EXPECT_TRUE(segment->candidate(1).attributes &
              converter::Attribute::USER_SEGMENT_HISTORY_REWRITER);
  EXPECT_FALSE(segment->candidate(1).attributes &
               converter::Attribute::BEST_CANDIDATE);
}

TEST(ZenzFeedbackCandidateRewriterTest,
     DoesNotInsertOrPromoteHardRejectedFeedbackCandidate) {
  ScopedUserProfileForZenzFeedbackCandidateRewriterTest profile;
  ASSERT_TRUE(profile.ok());

  session::ZenzFeedbackStore store;
  store.RecordAccepted("かれはてんてきです", "japanese_only", "彼は天敵です");
  store.RecordRejected("かれはてんてきです", "japanese_only", "彼は天敵です",
                       "hard_reject");

  Segments segments;
  AddSegment("かれはてんてきです", "彼は点滴です", &segments);

  const ConversionRequest request = CreateZenzFeedbackConversionRequest();

  ZenzFeedbackCandidateRewriter rewriter;
  EXPECT_FALSE(rewriter.Rewrite(request, &segments));

  ASSERT_EQ(segments.conversion_segments_size(), 1);
  const Segment& segment = segments.conversion_segment(0);
  ASSERT_EQ(segment.candidates_size(), 1);
  EXPECT_EQ(segment.candidate(0).value, "彼は点滴です");
  EXPECT_TRUE(segment.candidate(0).attributes &
              converter::Attribute::BEST_CANDIDATE);
}

TEST(ZenzFeedbackCandidateRewriterTest,
     DoesNotInsertOrPromoteAutoBlockedFeedbackCandidate) {
  ScopedUserProfileForZenzFeedbackCandidateRewriterTest profile;
  ASSERT_TRUE(profile.ok());

  session::ZenzFeedbackStore store;
  store.RecordAccepted("かれはてんてきです", "empty", "彼は天敵です");
  store.RecordAccepted("かれはてんてきです", "empty", "彼は天敵です");
  store.RecordAccepted("かれはてんてきです", "empty", "彼は天敵です");
  store.RecordRejected("かれはてんてきです", "empty", "彼は天敵です",
                       "space_revert_zenz_to_mozc");
  store.RecordRejected("かれはてんてきです", "empty", "彼は天敵です",
                       "space_revert_zenz_to_mozc");

  Segments segments;
  AddSegment("かれはてんてきです", "彼は点滴です", &segments);

  config::Config config;
  config.set_use_zenz_feedback_learning(true);
  config.set_use_zenz_feedback_reuse(true);
  config.set_history_learning_level(config::Config::DEFAULT_HISTORY);
  config.set_use_zenz_auto_block_rejected_correction(true);
  config.set_zenz_auto_block_reject_threshold(2);
  config.set_zenz_auto_block_minimum_reject_percentage(40);

  ConversionRequest::Options options;
  options.request_type = ConversionRequest::CONVERSION;
  options.enable_user_history_for_conversion = true;
  options.incognito_mode = false;

  const ConversionRequest request =
      ConversionRequestBuilder()
          .SetConfig(config)
          .SetOptions(std::move(options))
          .SetRequestType(ConversionRequest::CONVERSION)
          .SetKey("かれはてんてきです")
          .Build();

  ZenzFeedbackCandidateRewriter rewriter;
  EXPECT_FALSE(rewriter.Rewrite(request, &segments));

  ASSERT_EQ(segments.conversion_segments_size(), 1);
  const Segment& segment = segments.conversion_segment(0);
  ASSERT_EQ(segment.candidates_size(), 1);
  EXPECT_EQ(segment.candidate(0).value, "彼は点滴です");
  EXPECT_TRUE(segment.candidate(0).attributes &
              converter::Attribute::BEST_CANDIDATE);
}

TEST(ZenzFeedbackCandidateRewriterTest,
     DoesNotInsertOrPromoteRejectDominantFeedbackCandidate) {
  ScopedUserProfileForZenzFeedbackCandidateRewriterTest profile;
  ASSERT_TRUE(profile.ok());

  session::ZenzFeedbackStore store;
  store.RecordAccepted("かれはてんてきです", "empty", "彼は天敵です");
  store.RecordRejected("かれはてんてきです", "empty", "彼は天敵です",
                       "space_revert_zenz_to_mozc");
  store.RecordRejected("かれはてんてきです", "empty", "彼は天敵です",
                       "space_revert_zenz_to_mozc");

  Segments segments;
  AddSegment("かれはてんてきです", "彼は点滴です", &segments);

  const ConversionRequest request = CreateZenzFeedbackConversionRequest();

  ZenzFeedbackCandidateRewriter rewriter;
  EXPECT_FALSE(rewriter.Rewrite(request, &segments));

  ASSERT_EQ(segments.conversion_segments_size(), 1);
  const Segment& segment = segments.conversion_segment(0);
  ASSERT_EQ(segment.candidates_size(), 1);
  EXPECT_EQ(segment.candidate(0).value, "彼は点滴です");
  EXPECT_TRUE(segment.candidate(0).attributes &
              converter::Attribute::BEST_CANDIDATE);
}

TEST(ZenzFeedbackCandidateRewriterTest,
     DoesNotDuplicateFeedbackWhenMozcTopAlreadyMatches) {
  ScopedUserProfileForZenzFeedbackCandidateRewriterTest profile;
  ASSERT_TRUE(profile.ok());

  session::ZenzFeedbackStore store;
  store.RecordAccepted("かれはてんてきです", "japanese_only", "彼は天敵です");

  Segments segments;
  AddSegment("かれはてんてきです", "彼は天敵です", &segments);

  const ConversionRequest request = CreateZenzFeedbackConversionRequest();

  ZenzFeedbackCandidateRewriter rewriter;
  EXPECT_FALSE(rewriter.Rewrite(request, &segments));

  ASSERT_EQ(segments.conversion_segments_size(), 1);
  const Segment& segment = segments.conversion_segment(0);
  ASSERT_EQ(segment.candidates_size(), 1);
  EXPECT_EQ(segment.candidate(0).value, "彼は天敵です");
  EXPECT_TRUE(segment.candidate(0).attributes &
              converter::Attribute::BEST_CANDIDATE);
}

TEST(ZenzFeedbackCandidateRewriterTest,
     IsNotAvailableForIncognitoOrConvertWithoutHistory) {
  config::Config config;
  config.set_use_zenz_feedback_learning(true);
  config.set_use_zenz_feedback_reuse(true);
  config.set_history_learning_level(config::Config::DEFAULT_HISTORY);

  ConversionRequest::Options options;
  options.request_type = ConversionRequest::CONVERSION;
  options.enable_user_history_for_conversion = true;
  options.incognito_mode = true;

  ZenzFeedbackCandidateRewriter rewriter;
  EXPECT_EQ(rewriter.capability(ConversionRequestBuilder()
                                    .SetConfig(config)
                                    .SetOptions(options)
                                    .SetRequestType(ConversionRequest::CONVERSION)
                                    .SetKey("かれはてんてきです")
                                    .Build()),
            RewriterInterface::NOT_AVAILABLE);

  options.incognito_mode = false;
  options.enable_user_history_for_conversion = false;
  EXPECT_EQ(rewriter.capability(ConversionRequestBuilder()
                                    .SetConfig(config)
                                    .SetOptions(options)
                                    .SetRequestType(ConversionRequest::CONVERSION)
                                    .SetKey("かれはてんてきです")
                                    .Build()),
            RewriterInterface::NOT_AVAILABLE);
}

TEST(ZenzFeedbackCandidateRewriterTest,
     IsAvailableAcrossMozcHistoryLearningLevels) {
  config::Config config;
  config.set_use_zenz_feedback_learning(true);
  config.set_use_zenz_feedback_reuse(true);

  ConversionRequest::Options options;
  options.request_type = ConversionRequest::CONVERSION;
  options.enable_user_history_for_conversion = true;
  options.incognito_mode = false;

  const config::Config::HistoryLearningLevel levels[] = {
      config::Config::READ_ONLY, config::Config::NO_HISTORY};

  ZenzFeedbackCandidateRewriter rewriter;
  for (const config::Config::HistoryLearningLevel level : levels) {
    config.set_history_learning_level(level);
    EXPECT_EQ(rewriter.capability(ConversionRequestBuilder()
                                      .SetConfig(config)
                                      .SetOptions(options)
                                      .SetRequestType(
                                          ConversionRequest::CONVERSION)
                                      .SetKey("かれはてんてきです")
                                      .Build()),
              RewriterInterface::CONVERSION)
        << "history_learning_level=" << level;
  }
}

TEST(ZenzFeedbackCandidateRewriterTest,
     ReusesFeedbackAcrossMozcHistoryLearningLevels) {
  ScopedUserProfileForZenzFeedbackCandidateRewriterTest profile;
  ASSERT_TRUE(profile.ok());

  session::ZenzFeedbackStore store;
  store.RecordAccepted("かれはてんてきです", "japanese_only",
                       "彼は天敵です");

  config::Config config;
  config.set_use_zenz_feedback_learning(true);
  config.set_use_zenz_feedback_reuse(true);

  ConversionRequest::Options options;
  options.request_type = ConversionRequest::CONVERSION;
  options.enable_user_history_for_conversion = true;
  options.incognito_mode = false;

  const config::Config::HistoryLearningLevel levels[] = {
      config::Config::READ_ONLY, config::Config::NO_HISTORY};

  ZenzFeedbackCandidateRewriter rewriter;
  for (const config::Config::HistoryLearningLevel level : levels) {
    config.set_history_learning_level(level);

    Segments segments;
    AddSegment("かれはてんてきです", "彼は点滴です", &segments);

    const ConversionRequest request =
        ConversionRequestBuilder()
            .SetConfig(config)
            .SetOptions(options)
            .SetRequestType(ConversionRequest::CONVERSION)
            .SetKey("かれはてんてきです")
            .Build();

    EXPECT_TRUE(rewriter.Rewrite(request, &segments))
        << "history_learning_level=" << level;

    bool found_feedback_candidate = false;
    const Segment& segment = segments.conversion_segment(0);
    for (size_t i = 0; i < segment.candidates_size(); ++i) {
      if (segment.candidate(i).value == "彼は天敵です") {
        found_feedback_candidate = true;
        break;
      }
    }
    EXPECT_TRUE(found_feedback_candidate)
        << "history_learning_level=" << level;
  }
}

TEST(ZenzFeedbackCandidateRewriterTest,
     IsNotAvailableWhenZenzFeedbackReuseIsDisabled) {
  ScopedUserProfileForZenzFeedbackCandidateRewriterTest profile;
  ASSERT_TRUE(profile.ok());

  config::Config config;
  config.set_use_zenz_feedback_learning(true);
  config.set_use_zenz_feedback_reuse(false);
  config.set_history_learning_level(config::Config::DEFAULT_HISTORY);

  ConversionRequest::Options options;
  options.request_type = ConversionRequest::CONVERSION;
  options.enable_user_history_for_conversion = true;

  const ConversionRequest request =
      ConversionRequestBuilder()
          .SetConfig(config)
          .SetOptions(std::move(options))
          .SetRequestType(ConversionRequest::CONVERSION)
          .SetKey("かれはてんてきです")
          .Build();

  ZenzFeedbackCandidateRewriter rewriter;
  EXPECT_EQ(rewriter.capability(request), RewriterInterface::NOT_AVAILABLE);
}

TEST(ZenzFeedbackCandidateRewriterTest,
     ExplicitReuseWorksWithLegacyLearningDisabled) {
  ScopedUserProfileForZenzFeedbackCandidateRewriterTest profile;
  ASSERT_TRUE(profile.ok());
  session::ZenzFeedbackStore store;
  store.RecordAccepted("かれはてんてきです", "empty", "彼は天敵です");

  config::Config config;
  config.set_use_zenz_feedback_learning(false);
  config.set_use_zenz_feedback_reuse(true);
  config.set_history_learning_level(config::Config::DEFAULT_HISTORY);
  ConversionRequest::Options options;
  options.request_type = ConversionRequest::CONVERSION;
  options.enable_user_history_for_conversion = true;

  const ConversionRequest request =
      ConversionRequestBuilder()
          .SetConfig(config)
          .SetOptions(std::move(options))
          .SetRequestType(ConversionRequest::CONVERSION)
          .SetKey("かれはてんてきです")
          .Build();

  Segments segments;
  AddSegment("かれはてんてきです", "彼は点滴です", &segments);
  ZenzFeedbackCandidateRewriter rewriter;
  EXPECT_EQ(rewriter.capability(request), RewriterInterface::CONVERSION);
  EXPECT_TRUE(rewriter.Rewrite(request, &segments));
  bool found = false;
  for (const auto& candidate : segments.conversion_segment(0).candidates()) {
    found |= candidate->value == "彼は天敵です";
  }
  EXPECT_TRUE(found);
}

TEST(ZenzFeedbackCandidateRewriterTest,
     LegacyLearningOnDoesNotOverrideExplicitReuseOff) {
  config::Config config;
  config.set_use_zenz_feedback_learning(true);
  config.set_use_zenz_feedback_reuse(false);
  ConversionRequest::Options options;
  options.request_type = ConversionRequest::CONVERSION;
  options.enable_user_history_for_conversion = true;
  const ConversionRequest request =
      ConversionRequestBuilder()
          .SetConfig(config)
          .SetOptions(std::move(options))
          .SetRequestType(ConversionRequest::CONVERSION)
          .SetKey("かれはてんてきです")
          .Build();
  ZenzFeedbackCandidateRewriter rewriter;
  EXPECT_EQ(rewriter.capability(request), RewriterInterface::NOT_AVAILABLE);
}

TEST(ZenzFeedbackCandidateRewriterTest,
     RepairsUnrequestedTrailingPunctuationInLearnedFeedback) {
  ScopedUserProfileForZenzFeedbackCandidateRewriterTest profile;
  ASSERT_TRUE(profile.ok());

  session::ZenzFeedbackStore store;
  store.RecordAccepted("あしたはあめ", "japanese_only", "明日は雨!");

  Segments segments;
  AddSegment("あしたはあめ", "明日は飴", &segments);

  // Verify the original safety/repair behavior independently of the
  // newly enabled default minimum-key-length gate.
  const ConversionRequest request = CreateZenzFeedbackConversionRequest(false);

  ZenzFeedbackCandidateRewriter rewriter;
  EXPECT_TRUE(rewriter.Rewrite(request, &segments));

  const Segment& segment = segments.conversion_segment(0);
  ASSERT_GE(segment.candidates_size(), 2);
  EXPECT_EQ(segment.candidate(0).value, "明日は飴");
  EXPECT_EQ(segment.candidate(1).value, "明日は雨");
}

TEST(ZenzFeedbackCandidateRewriterTest,
     SkipsLearnedFeedbackThatBecomesSameAsMozcAfterSymbolRepair) {
  ScopedUserProfileForZenzFeedbackCandidateRewriterTest profile;
  ASSERT_TRUE(profile.ok());

  session::ZenzFeedbackStore store;
  store.RecordAccepted("するな", "japanese_only", "するな!");

  Segments segments;
  AddSegment("するな", "するな", &segments);

  // Verify the original safety/repair behavior independently of the
  // newly enabled default minimum-key-length gate.
  const ConversionRequest request = CreateZenzFeedbackConversionRequest(false);

  ZenzFeedbackCandidateRewriter rewriter;
  EXPECT_FALSE(rewriter.Rewrite(request, &segments));

  const Segment& segment = segments.conversion_segment(0);
  ASSERT_EQ(segment.candidates_size(), 1);
  EXPECT_EQ(segment.candidate(0).value, "するな");
}

TEST(ZenzFeedbackCandidateRewriterTest,
     DoesNotReintroduceMutatedTechnicalTokenFeedback) {
  ScopedUserProfileForZenzFeedbackCandidateRewriterTest profile;
  ASSERT_TRUE(profile.ok());

  session::ZenzFeedbackStore store;
  store.RecordAccepted("しーぷらすぷらす", "empty", "C#");

  Segments segments;
  AddSegment("しーぷらすぷらす", "C++", &segments);

  const ConversionRequest request = CreateZenzFeedbackConversionRequest();

  ZenzFeedbackCandidateRewriter rewriter;
  EXPECT_FALSE(rewriter.Rewrite(request, &segments));

  ASSERT_EQ(segments.conversion_segments_size(), 1);
  const Segment& segment = segments.conversion_segment(0);
  ASSERT_EQ(segment.candidates_size(), 1);
  EXPECT_EQ(segment.candidate(0).value, "C++");
  EXPECT_TRUE(segment.candidate(0).attributes &
              converter::Attribute::BEST_CANDIDATE);
}

TEST(ZenzFeedbackCandidateRewriterTest,
     DoesNotReintroduceUnlicensedAlphabeticFeedback) {
  ScopedUserProfileForZenzFeedbackCandidateRewriterTest profile;
  ASSERT_TRUE(profile.ok());

  session::ZenzFeedbackStore store;
  store.RecordAccepted("とうきょう", "empty", "Tokyo");

  Segments segments;
  AddSegment("とうきょう", "東京", &segments);

  // Verify the original safety/repair behavior independently of the
  // newly enabled default minimum-key-length gate.
  const ConversionRequest request = CreateZenzFeedbackConversionRequest(false);

  ZenzFeedbackCandidateRewriter rewriter;
  EXPECT_FALSE(rewriter.Rewrite(request, &segments));

  ASSERT_EQ(segments.conversion_segments_size(), 1);
  const Segment& segment = segments.conversion_segment(0);
  ASSERT_EQ(segment.candidates_size(), 1);
  EXPECT_EQ(segment.candidate(0).value, "東京");
  EXPECT_TRUE(segment.candidate(0).attributes &
              converter::Attribute::BEST_CANDIDATE);
}

#else  // defined(_WIN32)

TEST(ZenzFeedbackCandidateRewriterTest, SkippedOnNonWindows) {
  GTEST_SKIP() << "Zenz feedback candidate rewriter uses LocalLow on Windows.";
}

#endif  // defined(_WIN32)

}  // namespace
}  // namespace mozc