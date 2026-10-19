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

#ifndef MOZC_SESSION_MELTYPE_PIECE_PIPELINE_H_
#define MOZC_SESSION_MELTYPE_PIECE_PIPELINE_H_

#include <cstddef>
#include <string>
#include <vector>

#include "meltype/scored_segmenter.h"

namespace mozc {
namespace session {

// Baseline + pinpoint protection pipeline.
//
// The screen is Mozc's own multi-clause conversion: the Session runs
// context_->converter()->Convert(...) and shows whatever Mozc segmented and
// converted, so long Japanese input keeps Mozc's natural clause boundaries
// and accuracy.  Meltype is consulted only to protect the spans the user
// typed in English:
//
//   English piece -> the segment covering its reading is rewritten to the
//                    typed letters ("google"), with Meltype's boundary
//                    spaces.  Mozc's kanji conversion can never mangle an
//                    English run, because the letters are restored on top of
//                    whatever Mozc produced.
//   Japanese piece -> Mozc's baseline segments stay untouched, unless
//                    SystemOne answered for that piece, in which case the
//                    segments inside the piece's reading range collapse to
//                    the chosen surface.
//
// The piece boundaries are aligned against the reading (the concatenation of
// the preedit segment keys): Japanese piece kana are matched as content, the
// English reading around them is sliced out.  A segment straddling an English
// piece's boundary is sliced there first (its value must mirror the key to
// follow); anything else that does not align exactly - a kana mismatch, an
// empty key, a segment spanning two Japanese pieces - keeps Mozc's own text.
// The alignment can only fail toward "show Mozc", never toward wrong text.
//
// Boundary spaces follow Meltype's English/Japanese boundary rule: a space
// separates the pieces around every boundary that touches an English piece,
// and nothing is added at the string edges.  "sakkigoogledeshirabeta" reads
// as "さっき google で調べた"; "zenzpaipuran" as "zenz パイプラン".

// One segment: a Mozc preedit segment (key = its reading slice, value = the
// converted surface) or a protection output (English letters / collapsed
// SystemOne choice for that reading slice).  In PiecePlan the value carries
// the per-piece baseline used for the questions' left contexts, and the
// boundary spaces are still added at join time.
struct PieceSegment {
  std::string key;    // reading slice (piece.kana / typed letters / Mozc key)
  std::string value;  // surface: Mozc text / typed letters / choice

  bool operator==(const PieceSegment&) const = default;
};

// The reading range a Meltype piece covers, as byte offsets into the
// concatenated UTF-8 preedit segment keys.
struct PieceReadingRange {
  std::size_t begin = 0;
  std::size_t end = 0;
};

// One question a Japanese piece puts to SystemOne: the text to the left of
// the piece, the piece's reading, and the Mozc dictionary candidates the
// model picks from.  English pieces never produce a question — they are never
// converted, so there is nothing to choose.
struct PieceQuestion {
  // Index into PiecePlan::segments / PiecePlan::pieces.
  std::size_t piece_index = 0;

  // The displayed values of all preceding pieces plus the boundary space that
  // opens this piece, e.g. "さっき google " for the third piece of
  // "sakkigoogledeshirabeta".  Empty for the first piece.
  std::string left_context;

  // The piece's kana, e.g. "でしらべた".
  std::string reading;

  // Mozc's dictionary candidates for |reading|, best first.  Never empty:
  // a question with no options would give the model nothing to choose.
  std::vector<std::string> options;
};

// The whole per-piece build for one Meltype segmentation, used to ask the
// SystemOne questions: per-piece baselines and their left contexts.  The
// screen itself is Mozc's baseline (see ProtectSegmentsWithPieces).
struct PiecePlan {
  // The segmentation this plan was built from; kept because the boundary
  // space rule needs each piece's is_english flag.
  std::vector<mozkey::meltype::ScoredSegmenter::Segment> pieces;

  // Parallel to |pieces|: segments[i] is the display for pieces[i], before
  // the boundary space is added.
  std::vector<PieceSegment> segments;

  // One entry per Japanese piece that has candidates.  Indices into
  // |segments| are not contiguous (English pieces have none), so each
  // question carries its own piece_index.
  std::vector<PieceQuestion> questions;
};

// Builds the per-piece baseline plan: English pieces contribute the typed
// letters, Japanese pieces their first dictionary candidate (or the reading
// when the converter offered nothing), boundary spaces applied - which is
// exactly what the SystemOne questions' left contexts reason about.
//
// |options| is parallel to |pieces|: the Mozc candidates for each Japanese
// piece's reading, empty for English pieces and empty when the converter had
// no answer.
PiecePlan BuildPiecePlan(
    const std::vector<mozkey::meltype::ScoredSegmenter::Segment>& pieces,
    const std::vector<std::vector<std::string>>& options);

// Applies SystemOne's answer to one piece: |segments[piece_index].value|
// becomes |choice| and the left contexts of every later question are
// recomputed against the new value.
//
// Returns false — changing nothing — when the piece has no question (not a
// Japanese piece, or no candidates were offered) or when |choice| is not one
// of the offered options.  A choice outside the options can only come from a
// stale answer, and writing it would replace text the model was never asked
// about.
bool ApplyPieceChoice(PiecePlan* plan, std::size_t piece_index,
                      const std::string& choice);

// The `state` string handed to SystemOne for one question: left context (when
// there is one) and reading.  The endpoint judges `state` against `options`,
// so this is the full question text.
std::string PieceQuestionState(const PieceQuestion& question);

// Concatenates the segments with the Meltype boundary-space rule.  These two
// are the commit string and the preedit key line.
std::string JoinPieceKeys(const PiecePlan& plan);
std::string JoinPieceValues(const PiecePlan& plan);

// The space that opens segment |index| under the boundary rule: one ASCII
// space when the boundary before it touches an English piece and neither
// side already carries the whitespace, nothing otherwise.  Exposed so the
// session can write preedit segments that match the joins exactly.
std::string PieceLeadingSpace(const PiecePlan& plan, std::size_t index);

// Slices |reading| (the concatenation of the preedit segment keys) into one
// range per piece: Japanese pieces match their kana as content at the current
// position, the English reading between two Japanese anchors is whatever
// sits between them, and a trailing English piece takes the tail.
//
// Returns false on any misalignment - a kana that does not match, an English
// piece that cannot be anchored, unconsumed trailing characters - and leaves
// |ranges| empty.  Callers treat false as "show Mozc unchanged".
bool ComputePieceReadingRanges(
    const std::vector<mozkey::meltype::ScoredSegmenter::Segment>& pieces,
    const std::string& reading, std::vector<PieceReadingRange>* ranges);

// Overlays Meltype's opinion on Mozc's own segments - the protection step of
// the pipeline.  |mozc_segments| is exactly what Mozc showed (multi-clause,
// naturally converted); |choices| is parallel to |pieces| and carries the
// cached SystemOne answer per piece (empty string = none).
//
// Returns the segments to display and commit:
//   * the run of segments an English piece owns collapses into one segment
//     whose value is the typed letters, with the boundary spaces on the
//     piece's edges.  Mozc often splits a long unknown-word reading into
//     several segments ("れっぢ" + "t", "ふぁせぼ" + "おk"); the keys
//     concatenate back to the piece's reading, so a multi-segment run is
//     exactly as protectable as a single one and never reaches the screen
//     half raw, half Mozc;
//   * a segment straddling an English piece's boundary is sliced there
//     first, so a particle merged into the English reading ("ごおgぇで")
//     does not take the whole piece's protection down with it.  The value
//     is split at the mirrored character position and only when it mirrors
//     the key character for character — anything else would be a guess, so
//     the overlay refuses instead;
//   * consecutive segments entirely inside a Japanese piece that carries a
//     choice collapse into one segment (concatenated keys, the chosen
//     surface);
//   * everything else - Japanese baseline, segments straddling two Japanese
//     pieces, any alignment or slicing failure - comes back identical to
//     |mozc_segments|.
//
// The baseline path is therefore a no-op by construction: a Japanese-only
// composition without a SystemOne answer returns Mozc's segments unchanged.
std::vector<PieceSegment> ProtectSegmentsWithPieces(
    const std::vector<PieceSegment>& mozc_segments,
    const std::vector<mozkey::meltype::ScoredSegmenter::Segment>& pieces,
    const std::vector<std::string>& choices);

}  // namespace session
}  // namespace mozc

#endif  // MOZC_SESSION_MELTYPE_PIECE_PIPELINE_H_
