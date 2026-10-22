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

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "base/util.h"

namespace mozc {
namespace session {

namespace {

using Segment = mozkey::meltype::ScoredSegmenter::Segment;

// True when |s| ends with the whitespace the boundary rule would add, so
// the join cannot produce two spaces in a row.
bool EndsWithSpace(const std::string& s) {
  return !s.empty() && s.back() == ' ';
}

// True when |s| already opens with the whitespace, same no-doubling rule.
bool StartsWithSpace(const std::string& s) {
  return !s.empty() && s.front() == ' ';
}

}  // namespace

std::string PieceLeadingSpace(const PiecePlan& plan, const std::size_t index) {
  if (index == 0 || index >= plan.pieces.size()) return std::string();
  const Segment& previous = plan.pieces[index - 1];
  const Segment& current = plan.pieces[index];
  if (!previous.is_english && !current.is_english) return std::string();
  // The raw may already carry the whitespace (an English piece typed with a
  // leading or trailing space); never double it.
  if (EndsWithSpace(previous.raw)) return std::string();
  if (StartsWithSpace(current.raw)) return std::string();
  return " ";
}

namespace {

// The left context of |index|: every preceding segment's value, joined with
// the boundary rule, plus the space that opens this piece.  For
// "sakkigoogledeshirabeta" the third piece sees "さっき google ".
std::string BuildLeftContext(const PiecePlan& plan, const std::size_t index) {
  std::string left;
  for (std::size_t i = 0; i < index && i < plan.segments.size(); ++i) {
    left += PieceLeadingSpace(plan, i);
    left += plan.segments[i].value;
  }
  left += PieceLeadingSpace(plan, index);
  return left;
}

}  // namespace

OrderedPieceCandidates OrderPieceCandidates(
    const std::vector<PieceCandidate>& candidates) {
  OrderedPieceCandidates ordered;
  ordered.surfaces.reserve(candidates.size());
  if (candidates.empty()) return ordered;

  // Mozc's own correction is the reading's most likely intent, so it leads.
  // Only the first one is promoted: a reading with two corrections (rare, and
  // only for the katakana dictionary entries) has no meaningful tie-break, and
  // promoting both would just reshuffle Mozc's list.
  for (const PieceCandidate& candidate : candidates) {
    if (candidate.typo_correction && !candidate.surface.empty()) {
      ordered.surfaces.push_back(candidate.surface);
      ordered.typo_correction = true;
      break;
    }
  }
  for (const PieceCandidate& candidate : candidates) {
    if (candidate.surface.empty()) continue;
    if (ordered.typo_correction &&
        candidate.surface == ordered.surfaces.front()) {
      continue;  // the promoted correction is already in place
    }
    ordered.surfaces.push_back(candidate.surface);
  }
  return ordered;
}

PiecePlan BuildPiecePlan(
    const std::vector<Segment>& pieces,
    const std::vector<std::vector<std::string>>& options,
    const std::vector<bool>& typo_corrections) {
  PiecePlan plan;
  plan.pieces = pieces;
  plan.segments.reserve(pieces.size());

  for (std::size_t i = 0; i < pieces.size(); ++i) {
    PieceSegment segment;
    if (pieces[i].is_english) {
      // Typed letters, key and value alike; the kanji converter never runs on
      // them, so they reach the screen and the commit exactly as typed.
      segment.key = pieces[i].raw;
      segment.value = pieces[i].raw;
    } else {
      segment.key = pieces[i].kana;
      const std::vector<std::string>* piece_options =
          i < options.size() ? &options[i] : nullptr;
      // Baseline (0ms) value: the dictionary's first candidate — which is
      // Mozc's typo correction when it offered one — or the reading itself
      // when the converter offered nothing.
      segment.value = (piece_options != nullptr && !piece_options->empty())
                          ? piece_options->front()
                          : pieces[i].kana;
    }
    plan.segments.push_back(std::move(segment));
  }

  for (std::size_t i = 0; i < pieces.size(); ++i) {
    if (pieces[i].is_english) continue;
    if (i >= options.size() || options[i].empty()) continue;
    PieceQuestion question;
    question.piece_index = i;
    question.reading = pieces[i].kana;
    question.options = options[i];
    question.typo_correction =
        i < typo_corrections.size() && typo_corrections[i];
    question.left_context = BuildLeftContext(plan, i);
    plan.questions.push_back(std::move(question));
  }
  return plan;
}

bool ApplyPieceChoice(PiecePlan* plan, const std::size_t piece_index,
                      const std::string& choice) {
  if (plan == nullptr || choice.empty()) return false;
  if (piece_index >= plan->segments.size()) return false;

  const auto question =
      std::find_if(plan->questions.begin(), plan->questions.end(),
                   [piece_index](const PieceQuestion& q) {
                     return q.piece_index == piece_index;
                   });
  if (question == plan->questions.end()) return false;
  if (std::find(question->options.begin(), question->options.end(), choice) ==
      question->options.end()) {
    return false;
  }

  plan->segments[piece_index].value = choice;

  // This piece's value is part of every later question's left context.  Keep
  // the remaining questions honest about what is actually on screen.
  for (PieceQuestion& later : plan->questions) {
    if (later.piece_index > piece_index) {
      later.left_context = BuildLeftContext(*plan, later.piece_index);
    }
  }
  return true;
}

std::string PieceQuestionState(const PieceQuestion& question) {
  if (question.left_context.empty()) {
    return "読み: " + question.reading;
  }
  return "左文脈: " + question.left_context + "\n読み: " + question.reading;
}

std::vector<std::string> ResolvePieceDisplayChoices(
    const PiecePlan& plan, const std::vector<std::string>& systemone_choices) {
  std::vector<std::string> choices(plan.pieces.size());
  for (const PieceQuestion& question : plan.questions) {
    if (question.piece_index >= choices.size()) continue;
    if (question.piece_index < systemone_choices.size() &&
        !systemone_choices[question.piece_index].empty()) {
      // A proven answer beats the dictionary baseline, typo correction
      // included: the model saw the correction as an option and picked
      // otherwise.
      choices[question.piece_index] = systemone_choices[question.piece_index];
      continue;
    }
    if (question.typo_correction && !question.options.empty()) {
      choices[question.piece_index] = question.options.front();
    }
  }
  return choices;
}

std::string JoinPieceKeys(const PiecePlan& plan) {
  std::string out;
  for (std::size_t i = 0; i < plan.segments.size(); ++i) {
    out += PieceLeadingSpace(plan, i);
    out += plan.segments[i].key;
  }
  return out;
}

std::string JoinPieceValues(const PiecePlan& plan) {
  std::string out;
  for (std::size_t i = 0; i < plan.segments.size(); ++i) {
    out += PieceLeadingSpace(plan, i);
    out += plan.segments[i].value;
  }
  return out;
}

bool ComputePieceReadingRanges(
    const std::vector<Segment>& pieces, const std::string& reading,
    std::vector<PieceReadingRange>* ranges) {
  if (ranges == nullptr) return false;
  ranges->clear();
  if (pieces.empty()) return false;

  // All offsets are byte offsets into the UTF-8 reading; Japanese kana and
  // segment keys are compared as whole byte sequences, so a UTF-8 character
  // can never be sliced in half.
  std::size_t pos = 0;
  for (std::size_t i = 0; i < pieces.size(); ++i) {
    const Segment& piece = pieces[i];
    std::size_t end = 0;
    if (piece.is_english) {
      if (i + 1 < pieces.size() && !pieces[i + 1].is_english) {
        // Anchor the English reading on the next Japanese piece's kana: the
        // English contribution is whatever sits between the previous anchor
        // and this one.
        const Segment& next = pieces[i + 1];
        if (next.kana.empty()) {
          ranges->clear();
          return false;
        }
        const std::size_t anchor = reading.find(next.kana, pos);
        if (anchor == std::string::npos) {
          ranges->clear();
          return false;
        }
        end = anchor;
      } else if (i + 1 < pieces.size()) {
        // Adjacent English pieces (ChatGPT|Plus, API|Gateway): the reading
        // carries each piece's typed letters in order, so the boundary is
        // the piece's own raw length.  Anything else (Mozc read the run as
        // kana) is not a boundary this overlay can trust: refuse.
        if (reading.compare(pos, piece.raw.size(), piece.raw) != 0) {
          ranges->clear();
          return false;
        }
        end = pos + piece.raw.size();
      } else {
        // Trailing English takes the tail of the reading.
        end = reading.size();
      }
    } else {
      if (piece.kana.empty() || pos > reading.size() ||
          reading.size() - pos < piece.kana.size() ||
          reading.compare(pos, piece.kana.size(), piece.kana) != 0) {
        // The reading does not carry the piece's kana here: Meltype and the
        // composer disagree about this span.  Refuse the whole alignment so
        // no region is rewritten against a wrong boundary.
        ranges->clear();
        return false;
      }
      end = pos + piece.kana.size();
    }
    if (end < pos) {
      ranges->clear();
      return false;
    }
    ranges->push_back(PieceReadingRange{pos, end});
    pos = end;
  }

  if (pos != reading.size()) {
    // The pieces did not explain the whole reading (a trailing Japanese
    // piece consumed less than the composer produced): refuse.
    ranges->clear();
    return false;
  }
  return true;
}

std::vector<PieceSegment> ProtectSegmentsWithPieces(
    const std::vector<PieceSegment>& mozc_segments,
    const std::vector<Segment>& pieces,
    const std::vector<std::string>& choices) {
  if (mozc_segments.empty() || pieces.empty()) return mozc_segments;
  if (choices.size() != pieces.size()) return mozc_segments;

  std::string reading;
  for (const PieceSegment& segment : mozc_segments) {
    if (segment.key.empty()) {
      // Without keys there is no reading to align against: show Mozc.
      return mozc_segments;
    }
    reading += segment.key;
  }

  std::vector<PieceReadingRange> ranges;
  if (!ComputePieceReadingRanges(pieces, reading, &ranges)) {
    return mozc_segments;
  }

  // Cut points at every edge of a non-empty English reading range.  Mozc
  // happily merges the English reading with the particle that follows it
  // ("ごおgぇで" as one segment), and a segment straddling that boundary
  // would own nobody — dropping the protection for the whole English piece.
  // Slicing such a segment at the cut first is what makes the letters' fate
  // independent of Mozc's segmentation.
  std::vector<std::size_t> cut_points;
  for (std::size_t p = 0; p < pieces.size(); ++p) {
    if (!pieces[p].is_english || ranges[p].begin == ranges[p].end) continue;
    cut_points.push_back(ranges[p].begin);
    cut_points.push_back(ranges[p].end);
  }
  std::sort(cut_points.begin(), cut_points.end());
  cut_points.erase(std::unique(cut_points.begin(), cut_points.end()),
                   cut_points.end());

  // Pre-slice the segments that straddle an English cut point.  The value is
  // split at the mirrored character position, which requires it to mirror the
  // key character for character; a value that does not (a real conversion of
  // part of the run) would make the split a guess, so the whole overlay
  // refuses and Mozc keeps the screen — the alignment never fails toward
  // wrong text.
  std::vector<PieceSegment> segments;
  segments.reserve(mozc_segments.size());
  {
    std::size_t at = 0;
    for (const PieceSegment& segment : mozc_segments) {
      const std::size_t begin = at;
      at += segment.key.size();
      const std::size_t end = at;

      // Cut points strictly inside (begin, end): everything else already
      // aligns and the segment passes through untouched.
      const std::size_t first_cut = static_cast<std::size_t>(
          std::lower_bound(cut_points.begin(), cut_points.end(), begin + 1) -
          cut_points.begin());
      bool sliced = false;
      std::size_t slice_at = begin;
      std::size_t value_chars = 0;  // value chars consumed by earlier slices
      for (std::size_t c = first_cut;
           c < cut_points.size() && cut_points[c] < end; ++c) {
        if (!sliced) {
          sliced = true;
          if (Util::CharsLen(segment.key) != Util::CharsLen(segment.value)) {
            return mozc_segments;  // the value cannot follow the key split
          }
        }
        const std::size_t cut = cut_points[c];
        PieceSegment head;
        head.key = segment.key.substr(slice_at - begin, cut - slice_at);
        const std::size_t head_chars = Util::CharsLen(head.key);
        head.value =
            std::string(Util::Utf8SubString(segment.value, value_chars,
                                            head_chars));
        segments.push_back(std::move(head));
        slice_at = cut;
        value_chars += head_chars;
      }
      if (sliced) {
        PieceSegment tail;
        tail.key = segment.key.substr(slice_at - begin);
        tail.value =
            std::string(Util::Utf8SubString(segment.value, value_chars));
        segments.push_back(std::move(tail));
      } else {
        segments.push_back(segment);
      }
    }
    if (at != reading.size()) return mozc_segments;  // defensive; cannot happen
  }

  // Ownership: which piece's range fully contains each segment's key range.
  // After the English-boundary slicing above, an English range is covered by
  // segments fully inside it; a segment straddling any other boundary (two
  // Japanese pieces) still belongs to nobody and keeps Mozc's text — that is
  // the safety story of this step.
  std::vector<std::ptrdiff_t> owner(segments.size(), -1);
  {
    std::size_t at = 0;
    for (std::size_t s = 0; s < segments.size(); ++s) {
      const std::size_t begin = at;
      at += segments[s].key.size();
      const std::size_t end = at;
      for (std::size_t p = 0; p < ranges.size(); ++p) {
        if (begin >= ranges[p].begin && end <= ranges[p].end) {
          owner[s] = static_cast<std::ptrdiff_t>(p);
          break;
        }
      }
    }
    if (at != reading.size()) return mozc_segments;  // defensive; cannot happen
  }

  std::vector<PieceSegment> out;
  out.reserve(segments.size());
  std::size_t s = 0;
  while (s < segments.size()) {
    const std::ptrdiff_t p = owner[s];
    if (p >= 0 && pieces[static_cast<std::size_t>(p)].is_english) {
      const std::size_t pi = static_cast<std::size_t>(p);
      // Meltype's boundary spaces live on the English piece's edge segments:
      // a space before the piece when it is not first, after it when it is
      // not last — never doubled against whitespace the raw already carries.
      // Two adjacent English pieces (ChatGPT|Plus) must not both add a space
      // at their shared edge: the second piece's leading space already opens
      // the gap, so the first piece adds no trailing one there.
      const bool leading =
          pi > 0 && !StartsWithSpace(pieces[pi].raw) &&
          !EndsWithSpace(pieces[pi - 1].raw);
      const bool trailing =
          pi + 1 < pieces.size() && !EndsWithSpace(pieces[pi].raw) &&
          !StartsWithSpace(pieces[pi + 1].raw) &&
          !(pieces[pi + 1].is_english && pieces[pi].is_english);
      // The run the English piece owns — one segment or Mozc's own
      // fine-grained split of the letters — collapses into one segment under
      // the concatenated keys.  Reaching the screen half raw, half Mozc
      // would be worse than no protection at all, so the segment count never
      // gates this: whatever the piece owns becomes the typed letters.
      PieceSegment merged;
      std::size_t e = s;
      while (e < segments.size() &&
             owner[e] == static_cast<std::ptrdiff_t>(pi)) {
        merged.key += segments[e].key;
        ++e;
      }
      merged.value =
          (leading ? " " : "") + pieces[pi].raw + (trailing ? " " : "");
      out.push_back(std::move(merged));
      s = e;
      continue;
    }
    if (p >= 0) {
      const std::size_t pi = static_cast<std::size_t>(p);
      const std::string& choice = choices[pi];
      if (!choice.empty()) {
        // SystemOne answered for this Japanese piece: its owned run of
        // segments collapses to the chosen surface under the concatenated
        // keys.  Boundary spaces are not added — a Japanese piece's edges
        // touch English pieces whose own segments already carry them.
        PieceSegment merged;
        std::size_t e = s;
        while (e < segments.size() &&
               owner[e] == static_cast<std::ptrdiff_t>(pi)) {
          merged.key += segments[e].key;
          ++e;
        }
        merged.value = choice;
        out.push_back(std::move(merged));
        s = e;
        continue;
      }
    }
    // Japanese baseline and everything unaligned: Mozc, untouched.
    out.push_back(segments[s]);
    ++s;
  }
  return out;
}

}  // namespace session
}  // namespace mozc
