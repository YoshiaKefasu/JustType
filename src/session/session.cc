// Copyright 2010-2021, Google Inc.
// All rights reserved.
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
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// Session class of Mozc server.

#include "session/session.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "base/clock.h"
#include "base/util.h"
#include "composer/composer.h"
#include "composer/key_event_util.h"
#include "composer/table.h"
#include "engine/engine_converter_interface.h"
#include "engine/engine_interface.h"
#include "protocol/commands.pb.h"
#include "protocol/config.pb.h"
#include "session/ime_context.h"
#include "session/key_event_transformer.h"
#include "session/keymap.h"
#include "session/meltype_bridge.h"
#include "session/meltype_piece_pipeline.h"
#include "session/systemone_client.h"
#include "session/systemone_logger.h"
#include "session/systemone_worker.h"
#include "session/trace_logger.h"
#include "session/vertical_writing_key_transform.h"
#include "session/zenz_output_validator.h"
#include "transliteration/transliteration.h"

#ifdef __APPLE__
#include <TargetConditionals.h>  // for TARGET_OS_IPHONE
#endif                           // __APPLE__

#if defined(_WIN32)
#include <windows.h>
#endif

namespace mozc {
namespace session {
namespace {

using ::mozc::engine::ConversionPreferences;
using ::mozc::engine::EngineConverterInterface;

#if defined(_WIN32) && defined(MOZC_LEFT_CONTEXT_DEBUG)
std::wstring Utf8ToWideForDebug(absl::string_view s) {
  if (s.empty()) {
    return std::wstring();
  }

  const int input_size = static_cast<int>(s.size());
  const int wide_size =
      ::MultiByteToWideChar(CP_UTF8, 0, s.data(), input_size, nullptr, 0);
  if (wide_size <= 0) {
    return L"<invalid utf8>";
  }

  std::wstring w(wide_size, L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), input_size, w.data(), wide_size);
  return w;
}

void MozcLeftContextDebugOutput(absl::string_view message) {
  std::wstring w = Utf8ToWideForDebug(message);
  w.push_back(L'\n');
  ::OutputDebugStringW(w.c_str());
}
#endif  // defined(_WIN32) && defined(MOZC_LEFT_CONTEXT_DEBUG)

#if defined(_WIN32)
std::wstring Utf8ToWideForDebug(absl::string_view s) {
  if (s.empty()) {
    return std::wstring();
  }

  const int input_size = static_cast<int>(s.size());
  const int wide_size =
      ::MultiByteToWideChar(CP_UTF8, 0, s.data(), input_size, nullptr, 0);
  if (wide_size <= 0) {
    return L"<invalid utf8>";
  }

  std::wstring w(wide_size, L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), input_size, w.data(), wide_size);
  return w;
}

void SessionDebugOutput(absl::string_view message) {
  std::wstring w = Utf8ToWideForDebug(message);
  w.push_back(L'\n');
  ::OutputDebugStringW(w.c_str());
}
#else
void SessionDebugOutput(absl::string_view) {}
#endif

bool IsValidDirectCommitTriggerKey(const config::Config& config,
                                   const commands::KeyEvent& key_event);
std::string RedactedTextStats(absl::string_view label,
                                  absl::string_view text) {
  return absl::StrCat(
      label, "_bytes=", text.size(),
      " ", label, "_chars=", Util::CharsLen(text));
}

std::string DebugBool(bool value) {
  return value ? "true" : "false";
}


// Maximum size of multiple undo stack.
const size_t kMultipleUndoMaxSize = 10;

// Avoid teaching one-character particles or accidental raw commits as strong
// user segment history when the user cancels a conversion and immediately
// commits the restored hiragana preedit.
constexpr size_t kMinRerankedPreeditCommitCharsAfterConvertCancel = 2;

// The keymap uses integer milliseconds, like the normal live conversion UI.
// Delay(0) is a no-op; all positive delays suspend the command sequence.
std::optional<uint32_t> ParseKeymapDelay(absl::string_view command) {
  if (!command.starts_with("Delay(") || !command.ends_with(")")) {
    return std::nullopt;
  }
  const absl::string_view digits = command.substr(6, command.size() - 7);
  if (digits.empty() || digits.size() > 4 ||
      !std::all_of(digits.begin(), digits.end(),
                   [](char c) { return c >= '0' && c <= '9'; })) {
    return std::nullopt;
  }
  uint32_t delay = 0;
  if (!absl::SimpleAtoi(digits, &delay) || delay > 5000) {
    return std::nullopt;
  }
  return delay;
}

// Default live conversion debounce delay. The user-visible value is stored in
// Config::live_conversion_delay_msec.
constexpr uint32_t kDefaultLiveConversionDelayMillisec = 228;
constexpr uint32_t kMaxLiveConversionDelayMillisec = 1000;

// Default minimum composition length before live conversion is allowed.
// Single-character input is often a particle such as 「に」「を」「が」,
// and converting it to a kanji such as 「二」 too eagerly is noisy.
// The value is user-configurable via Config::live_conversion_min_key_length.
constexpr uint32_t kDefaultLiveConversionMinKeyLength = 2;
constexpr uint32_t kMinLiveConversionMinKeyLength = 1;
constexpr uint32_t kMaxLiveConversionMinKeyLength = 20;

// Poll interval for the async SystemOne (Jev / Laya) answer: the answer lands
// whenever the worker thread finishes, so the delayed callback chain polls
// instead of blocking the key-event path.
constexpr uint32_t kDefaultSystemOnePollMsec = 24;

bool IsLiveConversionTrailingDecorativeSymbol(char32_t c) {
  switch (c) {
    case 0x007E:  // ~
    case 0xFF5E:  // ～
    case 0x301C:  // 〜
    case 0x30FC:  // ー
    case 0x2015:  // ―
    case 0x2025:  // ‥
    case 0x2026:  // …
    case 0x0021:  // !
    case 0xFF01:  // ！
    case 0x003F:  // ?
    case 0xFF1F:  // ？
    case 0x3002:  // 。
    case 0x3001:  // 、
    case 0x002C:  // ,
    case 0xFF0C:  // ，
    case 0x002E:  // .
    case 0xFF0E:  // ．
      return true;
    default:
      return false;
  }
}

bool ContainsStringView(std::initializer_list<absl::string_view> values,
                        absl::string_view target) {
  for (absl::string_view value : values) {
    if (value == target) {
      return true;
    }
  }
  return false;
}

bool ShouldSkipLiveConversionForCompositionKey(
    absl::string_view key, size_t min_key_length) {
  if (Util::CharsLen(key) < min_key_length) {
    return true;
  }

  absl::string_view core = key;
  bool has_decorative_tail = false;

  while (!core.empty()) {
    absl::string_view rest;
    char32_t last = 0;
    if (!Util::SplitLastChar32(core, &rest, &last)) {
      break;
    }
    if (!IsLiveConversionTrailingDecorativeSymbol(last)) {
      break;
    }
    has_decorative_tail = true;
    core = rest;
  }

  if (!has_decorative_tail) {
    return false;
  }

  // Pure symbol sequences such as "~~" do not need live conversion.
  if (core.empty()) {
    return true;
  }

  // "え~", "へー", "ん？" should stay as kana while typing.
  // But longer readings such as "きょう~" may still be live-converted.
  if (Util::CharsLen(core) >= min_key_length) {
    return false;
  }

  return Util::IsScriptType(core, Util::HIRAGANA);
}

bool ShouldKeepPendingLiveConversionForTransientSokuon(absl::string_view key) {
  if (key.empty() || !Util::IsScriptType(key, Util::HIRAGANA)) {
    return false;
  }

  absl::string_view rest;
  char32_t last = 0;
  if (!Util::SplitLastChar32(key, &rest, &last) || last != U'っ') {
    return false;
  }

  if (Util::CharsLen(rest) < 2) {
    return false;
  }

  absl::string_view before_rest;
  char32_t previous = 0;
  if (Util::SplitLastChar32(rest, &before_rest, &previous) &&
      previous == U'っ') {
    return false;
  }

  // This is aimed at transient sokuon forms that commonly recover on the
  // next kana, e.g. 「おもっ」→「おもって」 and 「くさっ」→「くさって」.
  // Keep this list narrow so expressive atoms such as 「やっっ」 and 「ふんっ」
  // continue to use the existing composition-only behavior.
  const auto has_transient_stem_suffix = [&](absl::string_view stem) {
    if (!absl::EndsWith(rest, stem)) {
      return false;
    }

    const absl::string_view prefix = rest.substr(0, rest.size() - stem.size());
    if (prefix.empty()) {
      return true;
    }

    // Allow common short functional prefixes within the same composition,
    // e.g. 「とおもっ」→「と思って」 and 「をさわっ」→「を触って」.
    return ContainsStringView({
        "と", "を", "に", "が", "は", "も", "で", "へ",
        "から", "まで", "より",
    }, prefix);
  };

  return has_transient_stem_suffix("おも") ||  // 思っ...
         has_transient_stem_suffix("くさ") ||  // 腐っ...
         has_transient_stem_suffix("さわ") ||  // 触っ...
         has_transient_stem_suffix("かえ") ||  // 帰っ...
         has_transient_stem_suffix("わら") ||  // 笑っ...
         has_transient_stem_suffix("おこ") ||  // 怒っ...
         has_transient_stem_suffix("まよ") ||  // 迷っ...
         has_transient_stem_suffix("のこ") ||  // 残っ...
         has_transient_stem_suffix("ひろ") ||  // 拾っ...
         has_transient_stem_suffix("ふと") ||  // 太っ...
         has_transient_stem_suffix("あた") ||  // 当たっ...
         has_transient_stem_suffix("あら") ||  // 洗っ...
         has_transient_stem_suffix("うつ") ||  // 打っ...
         has_transient_stem_suffix("うた") ||  // 歌っ...
         has_transient_stem_suffix("けず") ||  // 削っ...
         has_transient_stem_suffix("しぼ") ||  // 絞っ...
         has_transient_stem_suffix("すわ") ||  // 座っ...
         has_transient_stem_suffix("とま") ||  // 止まっ...
         has_transient_stem_suffix("なお") ||  // 直っ...
         has_transient_stem_suffix("のぼ") ||  // 登っ...
         has_transient_stem_suffix("はし") ||  // 走っ...
         has_transient_stem_suffix("まわ");    // 回っ...
}

bool CandidateWordHasAttribute(const commands::CandidateWord& candidate,
                               commands::CandidateAttribute attribute) {
  for (int i = 0; i < candidate.attributes_size(); ++i) {
    if (candidate.attributes(i) == attribute) {
      return true;
    }
  }
  return false;
}

bool IsAsciiIdentityChar(const unsigned char c) {
  return (('0' <= c) && (c <= '9')) || (('A' <= c) && (c <= 'Z')) ||
         (('a' <= c) && (c <= 'z')) || c == '_' || c == '-' || c == '.' ||
         c == '+' || c == '#';
}

bool HasAsciiLetterOrDigit(absl::string_view value) {
  for (const unsigned char c : value) {
    if ((('0' <= c) && (c <= '9')) || (('A' <= c) && (c <= 'Z')) ||
        (('a' <= c) && (c <= 'z'))) {
      return true;
    }
  }
  return false;
}

std::vector<std::string> ExtractAsciiIdentitySurfaces(
    absl::string_view value) {
  std::vector<std::string> surfaces;
  size_t start = absl::string_view::npos;

  auto flush = [&](size_t end) {
    if (start == absl::string_view::npos || end <= start) {
      start = absl::string_view::npos;
      return;
    }
    const absl::string_view surface = value.substr(start, end - start);
    if (HasAsciiLetterOrDigit(surface)) {
      surfaces.emplace_back(surface);
    }
    start = absl::string_view::npos;
  };

  for (size_t i = 0; i < value.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    if (IsAsciiIdentityChar(c)) {
      if (start == absl::string_view::npos) {
        start = i;
      }
    } else {
      flush(i);
    }
  }
  flush(value.size());

  return surfaces;
}

std::string FindPreeditKeyForValue(const commands::Preedit& preedit,
                                   absl::string_view value) {
  for (int i = 0; i < preedit.segment_size(); ++i) {
    const commands::Preedit::Segment& segment = preedit.segment(i);
    if (segment.value() == value) {
      return segment.key();
    }
  }
  return std::string();
}

uint32_t GetLiveConversionDelayMillisec(const config::Config& config) {
  if (!config.has_live_conversion_delay_msec()) {
    return kDefaultLiveConversionDelayMillisec;
  }
  return std::min(config.live_conversion_delay_msec(),
                  kMaxLiveConversionDelayMillisec);
}

size_t GetLiveConversionMinKeyLength(const config::Config& config) {
  const uint32_t value = config.has_live_conversion_min_key_length()
                             ? config.live_conversion_min_key_length()
                             : kDefaultLiveConversionMinKeyLength;
  return static_cast<size_t>(
      std::clamp(value,
                 kMinLiveConversionMinKeyLength,
                 kMaxLiveConversionMinKeyLength));
}

bool ShouldExposePreLiveConversionReading(const ImeContext& context) {
  const config::Config& config = context.GetConfig();
  if (!config.use_live_conversion() ||
      context.state() != ImeContext::COMPOSITION) {
    return false;
  }

  if (context.composer().GetInputFieldType() == commands::Context::PASSWORD) {
    return false;
  }

  const transliteration::TransliterationType input_mode =
      context.composer().GetInputMode();
  if (input_mode == transliteration::HALF_ASCII ||
      input_mode == transliteration::FULL_ASCII) {
    return false;
  }

  const size_t length = context.composer().GetLength();
  if (length != context.composer().GetCursor()) {
    return false;
  }

  const std::string key = context.composer().GetQueryForConversion();
  const size_t configured_min_key_length =
      GetLiveConversionMinKeyLength(config);

  // This state exists only while the configured minimum length is the reason
  // live conversion is being skipped.  If the same composition would still be
  // skipped with the minimum supported value (1), do not expose the reading.
  if (!ShouldSkipLiveConversionForCompositionKey(
          key, configured_min_key_length)) {
    return false;
  }
  return !ShouldSkipLiveConversionForCompositionKey(
      key, kMinLiveConversionMinKeyLength);
}













bool StartsWithString(absl::string_view text, absl::string_view prefix) {
  return text.size() >= prefix.size() &&
         text.substr(0, prefix.size()) == prefix;
}


;

void AddPreeditSegment(absl::string_view key,
                       absl::string_view value,
                       commands::Preedit::Segment::Annotation annotation,
                       commands::Preedit* preedit) {
  commands::Preedit::Segment* segment = preedit->add_segment();
  segment->set_annotation(annotation);
  segment->set_key(std::string(key));
  segment->set_value(std::string(value));
  segment->set_value_length(Util::CharsLen(value));
}

void AddPreeditSegmentWithPendingRomanTail(
    absl::string_view key, absl::string_view value,
    size_t pending_roman_length, commands::Preedit* preedit) {
  const size_t value_length = Util::CharsLen(value);
  if (pending_roman_length == 0 || pending_roman_length > value_length) {
    AddPreeditSegment(key, value, commands::Preedit::Segment::UNDERLINE,
                      preedit);
    return;
  }

  const size_t stable_length = value_length - pending_roman_length;
  if (stable_length == 0) {
    AddPreeditSegment(key, value, commands::Preedit::Segment::UNDERLINE,
                      preedit);
    preedit->mutable_segment(preedit->segment_size() - 1)
        ->set_is_pending_roman(true);
    return;
  }

  const absl::string_view stable_value =
      Util::Utf8SubString(value, 0, stable_length);
  const absl::string_view pending_value =
      Util::Utf8SubString(value, stable_length, pending_roman_length);

  // This split is presentation-only, so it must never synthesize segment keys
  // from values. If the supplied key cannot preserve a non-empty stable prefix
  // plus the pending suffix exactly, keep the original segment unsplit.
  const size_t key_length = Util::CharsLen(key);
  if (key_length <= pending_roman_length) {
    AddPreeditSegment(key, value, commands::Preedit::Segment::UNDERLINE,
                      preedit);
    return;
  }

  const size_t stable_key_length = key_length - pending_roman_length;
  const absl::string_view stable_key =
      Util::Utf8SubString(key, 0, stable_key_length);
  const absl::string_view pending_key =
      Util::Utf8SubString(key, stable_key_length, pending_roman_length);

  AddPreeditSegment(stable_key, stable_value,
                    commands::Preedit::Segment::UNDERLINE, preedit);
  AddPreeditSegment(pending_key, pending_value,
                    commands::Preedit::Segment::UNDERLINE, preedit);
  preedit->mutable_segment(preedit->segment_size() - 1)
      ->set_is_pending_roman(true);
}

void MarkPendingRomanTailOnPreedit(const composer::Composer& composer,
                                   commands::Preedit* preedit) {
  if (preedit == nullptr || preedit->segment_size() == 0) {
    return;
  }

  const size_t pending_length = composer.GetPendingRomanDisplayLength();
  if (pending_length == 0) {
    return;
  }

  const std::string display = composer.GetStringForPreedit();
  const size_t display_length = Util::CharsLen(display);
  if (pending_length > display_length) {
    return;
  }

  const absl::string_view expected_display_suffix =
      Util::Utf8SubString(display, display_length - pending_length,
                          pending_length);

  const std::string query = composer.GetQueryForConversion();
  const size_t query_length = Util::CharsLen(query);
  if (pending_length > query_length) {
    return;
  }

  const absl::string_view expected_key_suffix =
      Util::Utf8SubString(query, query_length - pending_length,
                          pending_length);

  commands::Preedit::Segment* last =
      preedit->mutable_segment(preedit->segment_size() - 1);
  const size_t value_length = Util::CharsLen(last->value());
  const size_t key_length = Util::CharsLen(last->key());

  if (pending_length > value_length) {
    return;
  }

  const absl::string_view actual_value_suffix =
      Util::Utf8SubString(last->value(), value_length - pending_length,
                          pending_length);

  // A conversion candidate may preserve the unresolved roman either in the
  // visible preedit style (for example, full-width "ｔ") or in the conversion
  // query style (for example, ASCII "t").  Do not mark an unrelated converted
  // tail.
  if (actual_value_suffix != expected_display_suffix &&
      actual_value_suffix != expected_key_suffix) {
    return;
  }

  if (value_length == pending_length) {
    last->set_is_pending_roman(true);
    return;
  }

  // Splitting a conversion output segment is presentation-only.  Preserve the
  // concatenated key/value, cursor, highlighted position, annotation, and every
  // other segment field.  If the source key cannot be split at the same trailing
  // character count, leave the converter-owned presentation untouched.
  if (pending_length > key_length) {
    return;
  }

  const size_t stable_value_length = value_length - pending_length;
  const size_t stable_key_length = key_length - pending_length;

  const std::string stable_value(
      Util::Utf8SubString(last->value(), 0, stable_value_length));
  const std::string pending_value(actual_value_suffix);
  const std::string stable_key(
      Util::Utf8SubString(last->key(), 0, stable_key_length));
  const std::string pending_key(
      Util::Utf8SubString(last->key(), stable_key_length, pending_length));

  commands::Preedit::Segment pending_segment = *last;

  last->set_key(stable_key);
  last->set_value(stable_value);
  last->set_value_length(stable_value_length);
  last->set_is_pending_roman(false);

  pending_segment.set_key(pending_key);
  pending_segment.set_value(pending_value);
  pending_segment.set_value_length(pending_length);
  pending_segment.set_is_pending_roman(true);
  *preedit->add_segment() = std::move(pending_segment);
}
void RestorePreeditSegmentKeysForSymbolStyle(
    absl::string_view symbol_style_source,
    commands::Preedit* preedit) {
  if (symbol_style_source.empty() || preedit == nullptr ||
      preedit->segment_size() == 0) {
    return;
  }

  // Restore only display keys.  The converter-owned internal key, candidate
  // value, and feedback key remain unchanged.  Multi-segment preedit is handled
  // by restoring the concatenated display key and then distributing it back by
  // the original segment key character lengths.  This is safe for wave-dash
  // restoration because ASCII '~', FULLWIDTH TILDE '～', and WAVE DASH '〜' are
  // all single Unicode scalar values.
  std::string full_key;
  std::string full_value;
  std::vector<size_t> segment_key_char_lengths;
  segment_key_char_lengths.reserve(preedit->segment_size());

  for (int i = 0; i < preedit->segment_size(); ++i) {
    const commands::Preedit::Segment& segment = preedit->segment(i);
    const absl::string_view segment_key =
        segment.has_key() && !segment.key().empty()
            ? absl::string_view(segment.key())
            : absl::string_view(segment.value());

    full_key.append(segment_key.data(), segment_key.size());
    full_value.append(segment.value());
    segment_key_char_lengths.push_back(Util::CharsLen(segment_key));
  }

  const std::string restored_key =
      ZenzOutputValidator::RestoreUserVisibleSymbolStyle(
          symbol_style_source, full_value, full_key);

  if (restored_key == full_key ||
      Util::CharsLen(restored_key) != Util::CharsLen(full_key)) {
    return;
  }

  size_t offset = 0;
  for (int i = 0; i < preedit->segment_size(); ++i) {
    const size_t segment_chars = segment_key_char_lengths[i];
    preedit->mutable_segment(i)->set_key(
        std::string(Util::Utf8SubString(restored_key,
                                        offset,
                                        segment_chars)));
    offset += segment_chars;
  }
}

bool IsPlainBackspaceKey(const commands::KeyEvent& key) {
  return key.has_special_key() &&
         key.special_key() == commands::KeyEvent::BACKSPACE &&
         key.modifier_keys_size() == 0;
}


bool IsPendingDirectCommitLearningDiscardKey(
    const commands::KeyEvent& key) {
  if (!key.has_special_key()) {
    return false;
  }

  switch (key.special_key()) {
    case commands::KeyEvent::BACKSPACE:
    case commands::KeyEvent::ESCAPE:
      return true;
    default:
      return false;
  }
}

// The single ASCII character |key_event| types, or empty when it is anything
// else (a special key, a non-ASCII character, a shortcut, or a multi-character
// key string).  Mozc receives Shift+letter with the SHIFT modifier already
// folded into the key code, so the uppercase code itself is the character;
// CapsLock flips which case means "shifted".
std::string AsciiInputChar(const commands::KeyEvent& key_event) {
  if (key_event.input_style() != commands::KeyEvent::FOLLOW_MODE ||
      key_event.has_special_key()) {
    return std::string();
  }

  std::string input;
  if (!key_event.key_string().empty()) {
    if (key_event.key_string().size() != 1) return std::string();
    input = key_event.key_string();
  } else if (key_event.has_key_code()) {
    if (key_event.key_code() > 0x7f) return std::string();
    input.push_back(static_cast<char>(key_event.key_code()));
  } else {
    return std::string();
  }

  const uint32_t modifiers = KeyEventUtil::GetModifiers(key_event);
  if (KeyEventUtil::HasCtrl(modifiers) || KeyEventUtil::HasAlt(modifiers)) {
    return std::string();
  }
  return input;
}

// The single ASCII character a Shift+letter key types, or empty when
// |key_event| is anything else.  See AsciiInputChar() for the key-code rules.
std::string ShiftedAsciiInputChar(const commands::KeyEvent& key_event) {
  const std::string input = AsciiInputChar(key_event);
  if (input.empty()) return std::string();

  const uint32_t modifiers = KeyEventUtil::GetModifiers(key_event);
  const bool caps_locked = KeyEventUtil::HasCaps(modifiers);
  const char key = input[0];
  const bool shifted = (!caps_locked && ('A' <= key && key <= 'Z')) ||
                       (caps_locked && ('a' <= key && key <= 'z'));
  return shifted ? input : std::string();
}

bool ShouldCommitLiveConversionBeforeShiftAsciiInput(
    const config::Config& config,
    const composer::Composer& composer,
    const commands::KeyEvent& key_event) {
  if (config.shift_key_mode_switch() !=
      config::Config::ASCII_INPUT_MODE) {
    return false;
  }

  if (key_event.input_style() != commands::KeyEvent::FOLLOW_MODE) {
    return false;
  }

  if (key_event.has_special_key()) {
    return false;
  }

  if (composer.GetInputFieldType() == commands::Context::PASSWORD) {
    return false;
  }

  const transliteration::TransliterationType input_mode =
      composer.GetInputMode();
  if (input_mode == transliteration::HALF_ASCII ||
      input_mode == transliteration::FULL_ASCII) {
    return false;
  }

  const size_t length = composer.GetLength();
  if (length == 0 || length != composer.GetCursor()) {
    return false;
  }

  return !ShiftedAsciiInputChar(key_event).empty();
}

void ExtractPreeditKeyAndValue(const commands::Preedit& preedit,
                               std::string* key,
                               std::string* value) {
  key->clear();
  value->clear();

  for (int i = 0; i < preedit.segment_size(); ++i) {
    const commands::Preedit::Segment& segment = preedit.segment(i);
    value->append(segment.value());
    if (segment.has_key() && !segment.key().empty()) {
      key->append(segment.key());
    } else {
      key->append(segment.value());
    }
  }
}

// |text| truncated for the trace log; keeps one preedit line readable.
std::string TraceText(absl::string_view text) {
  constexpr size_t kMaxTraceChars = 64;
  if (Util::CharsLen(text) <= kMaxTraceChars) {
    return std::string(text);
  }
  return absl::StrCat(Util::Utf8SubString(text, 0, kMaxTraceChars), "...");
}

// One-line summary of the output a block ended with.
std::string TraceOutputSummary(const commands::Output& output) {
  std::string key;
  std::string value;
  if (output.has_preedit()) {
    ExtractPreeditKeyAndValue(output.preedit(), &key, &value);
  }
  return absl::StrCat("preedit=\"", TraceText(value),
                      "\" | window_visible=",
                      output.has_candidate_window() ? "true" : "false");
}

// Set input mode if the current input mode is not the given mode.
void SwitchInputMode(const transliteration::TransliterationType mode,
                     composer::Composer* composer) {
  if (composer->GetInputMode() != mode) {
    composer->SetInputMode(mode);
  }
  composer->SetNewInput();
}

// Set input mode to the |composer| if the input mode of |composer| is not
// the given |mode|.
void ApplyCompositionMode(const commands::CompositionMode mode,
                          composer::Composer* composer) {
  switch (mode) {
    case commands::HIRAGANA:
      SwitchInputMode(transliteration::HIRAGANA, composer);
      break;
    case commands::FULL_KATAKANA:
      SwitchInputMode(transliteration::FULL_KATAKANA, composer);
      break;
    case commands::HALF_KATAKANA:
      SwitchInputMode(transliteration::HALF_KATAKANA, composer);
      break;
    case commands::FULL_ASCII:
      SwitchInputMode(transliteration::FULL_ASCII, composer);
      break;
    case commands::HALF_ASCII:
      SwitchInputMode(transliteration::HALF_ASCII, composer);
      break;
    default:
      LOG(DFATAL) << "ime on with invalid mode";
  }
}

// Return true if the specified key event consists of any modifier key only.
bool IsPureModifierKeyEvent(const commands::KeyEvent& key) {
  if (key.has_key_code()) {
    return false;
  }
  if (key.has_special_key()) {
    return false;
  }
  if (key.modifier_keys_size() == 0) {
    return false;
  }
  return true;
}

bool IsPureSpaceKey(const commands::KeyEvent& key) {
  if (key.has_key_code()) {
    return false;
  }
  if (key.modifier_keys_size() > 0) {
    return false;
  }
  if (!key.has_special_key()) {
    return false;
  }
  if (key.special_key() != commands::KeyEvent::SPACE) {
    return false;
  }
  return true;
}

// Set session state to the given state and also update related status.
void SetSessionState(const ImeContext::State state, ImeContext* context) {
  const ImeContext::State prev_state = context->state();
  context->set_state(state);
  switch (state) {
    case ImeContext::DIRECT:
      // The IME is off; keep the mode the user chose so that turning the IME
      // back on resumes it.  Nothing is being typed in this state, so there is
      // no temporary mode to drop.
      context->mutable_composer()->Reset();
      break;
    case ImeContext::PRECOMPOSITION: {
      // Every commit (Enter, a direct commit, a committed suggestion) and every
      // cancel passes through here, so this is the one place that can make a
      // one-shot katakana / alphanumeric run end in Hiragana.
      //
      // A "temporary" mode is the one Composer layers on top of the mode the
      // user really chose: Shift+letter ASCII, the kana-type rotation, and the
      // KATAKANA_INPUT_MODE shift.  Composer keeps the chosen mode in
      // |comeback_input_mode_| and the layered one in |input_mode_|, so the two
      // differing is exactly the signal that the composition just ended was
      // typed in a temporary mode.  A mode the user or the application selected
      // explicitly (Eisu, SWITCH_COMPOSITION_MODE from an InputScope) has both
      // slots equal and must survive the commit unchanged.
      //
      // Reset() alone is not enough: it restores |comeback_input_mode_|, so a
      // temporary run would keep coming back to whatever base it was layered on
      // instead of returning to Hiragana.
      composer::Composer* composer = context->mutable_composer();
      const bool had_temporary_input_mode =
          composer->GetInputMode() != composer->GetComebackInputMode();
      // A password field is where raw ASCII has to keep working across commits,
      // so it is never moved out from under the user.
      const bool is_password_field =
          composer->GetInputFieldType() == commands::Context::PASSWORD;

      composer->Reset();

      if (had_temporary_input_mode && !is_password_field) {
        // SetInputMode() writes both slots.  SetTemporaryInputMode() here would
        // leave the previous base in |comeback_input_mode_|, and Composer::Reset()
        // restores that slot, so the very next key would come back to it.  The
        // TSF client also echoes whatever mode it believes is active, which made
        // the stale base stick across commits ("keeps typing アイウ" after a
        // katakana run, or _A after a Shift+letter run).  Aligning both slots on
        // Hiragana is what actually ends the temporary run.
        //
        // A mode the user or the application pinned explicitly never reaches
        // this branch: those have |input_mode_| == |comeback_input_mode_|, so
        // |had_temporary_input_mode| is false and the pinned mode survives.
        composer->SetInputMode(transliteration::HIRAGANA);
      }
      break;
    }
    case ImeContext::CONVERSION:
      context->mutable_composer()->ResetInputMode();
      break;
    case ImeContext::COMPOSITION:
      if (prev_state == ImeContext::PRECOMPOSITION) {
        // Notify the start of composition to the converter so that internal
        // state can be refreshed by the client context (especially by
        // preceding text).
        context->mutable_converter()->OnStartComposition(
            context->client_context());
      }
      break;
    default:
      // Do nothing.
      break;
  }
}

void SetStateToPredompositionAndCancel(ImeContext* context) {
  SetSessionState(ImeContext::PRECOMPOSITION, context);
  // mutable_converter's internal state should be updated by calling Cancel().
  // Internal state contains:
  // - candidate list
  // - result text to commit
  if (!context->mutable_converter()->CheckState(
          EngineConverterInterface::COMPOSITION)) {
    context->mutable_converter()->Cancel();
  }
}

commands::CompositionMode ToCompositionMode(
    mozc::transliteration::TransliterationType type) {
  commands::CompositionMode mode = commands::HIRAGANA;
  switch (type) {
    case transliteration::HIRAGANA:
      mode = commands::HIRAGANA;
      break;
    case transliteration::FULL_KATAKANA:
      mode = commands::FULL_KATAKANA;
      break;
    case transliteration::HALF_KATAKANA:
      mode = commands::HALF_KATAKANA;
      break;
    case transliteration::FULL_ASCII:
      mode = commands::FULL_ASCII;
      break;
    case transliteration::HALF_ASCII:
      mode = commands::HALF_ASCII;
      break;
    default:
      LOG(ERROR) << "Unknown input mode: " << type;
      // use HIRAGANA as a default.
  }
  return mode;
}

ImeContext::State GetEffectiveStateForTestSendKey(const commands::KeyEvent& key,
                                                  ImeContext::State state) {
  if (!key.has_activated()) {
    return state;
  }
  if (state == ImeContext::DIRECT && key.activated()) {
    // Indirect IME On found.
    return ImeContext::PRECOMPOSITION;
  }
  if (state != ImeContext::DIRECT && !key.activated()) {
    // Indirect IME Off found.
    return ImeContext::DIRECT;
  }
  return state;
}

std::string AbbreviateCandidateActionKeyToken(absl::string_view token) {
  if (token == "Control" || token == "control" || token == "Ctrl" ||
      token == "ctrl") {
    return "Ctrl";
  }
  if (token == "Delete" || token == "delete") {
    return "Del";
  }
  if (token == "Backspace" || token == "backspace") {
    return "Bksp";
  }
  if (token == "Escape" || token == "escape") {
    return "Esc";
  }
  if (token == "Insert" || token == "insert") {
    return "Ins";
  }
  if (token == "PageUp" || token == "pageup") {
    return "PgUp";
  }
  if (token == "PageDown" || token == "pagedown") {
    return "PgDn";
  }
  return std::string(token);
}

std::string FormatCandidateActionKeyLabel(absl::string_view key_name) {
  std::string result;
  size_t begin = 0;
  while (begin < key_name.size()) {
    while (begin < key_name.size() && key_name[begin] == ' ') {
      ++begin;
    }
    if (begin == key_name.size()) {
      break;
    }

    size_t end = key_name.find(' ', begin);
    if (end == absl::string_view::npos) {
      end = key_name.size();
    }

    if (!result.empty()) {
      result.push_back('+');
    }
    result.append(
        AbbreviateCandidateActionKeyToken(key_name.substr(begin, end - begin)));
    begin = end + 1;
  }
  return result;
}

void MergeCommandResult(const commands::Result& step_result,
                        commands::Result* accumulated_result) {
  if (!accumulated_result->has_key() && !accumulated_result->has_value()) {
    *accumulated_result = step_result;
    return;
  }

  if (step_result.has_key()) {
    accumulated_result->set_key(
        absl::StrCat(accumulated_result->key(), step_result.key()));
  }
  if (step_result.has_value()) {
    accumulated_result->set_value(
        absl::StrCat(accumulated_result->value(), step_result.value()));
  }
}

void AccumulateCommandOutput(const commands::Output& step_output,
                             bool* consumed,
                             bool* has_accumulated_result,
                             commands::Result* accumulated_result,
                             commands::Output* final_output) {
  *consumed = *consumed || step_output.consumed();
  *final_output = step_output;

  if (step_output.has_result()) {
    MergeCommandResult(step_output.result(), accumulated_result);
    *has_accumulated_result = true;
  }

  final_output->set_consumed(*consumed);
  if (*has_accumulated_result) {
    *final_output->mutable_result() = *accumulated_result;
  }
}

}  // namespace

Session::Session(const EngineInterface& engine)
    : context_(CreateContext(engine)),
      piece_converter_(engine.CreateEngineConverter()) {
  // The piece converter only ever answers one reading at a time; its window
  // is never shown (the session converter owns the candidate window).
  piece_converter_->SetCandidateListVisible(false);
}

std::unique_ptr<ImeContext> Session::CreateContext(
    const EngineInterface& engine) const {
  auto context = std::make_unique<ImeContext>(engine.CreateEngineConverter());
  context->set_create_time(Clock::GetAbslTime());

#ifdef _WIN32
  // On Windows session is started with direct mode.
  // FIXME(toshiyuki): Ditto for Mac after verifying on Mac.
  context->set_state(ImeContext::DIRECT);
#else   // _WIN32
  context->set_state(ImeContext::PRECOMPOSITION);
#endif  // _WIN32

  // TODO(team): Remove #if based behavior change for cascading window.
  // Tests for session layer (session_handler_scenario_test, etc) can be
  // unstable.
#if (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE) || defined(__linux__) || \
    defined(__wasm__)
  context->mutable_converter()->set_use_cascading_window(false);
#endif  // TARGET_OS_IPHONE || __linux__ || __wasm__

  return context;
}

void Session::PushUndoContext() {
  UndoEntry entry;
  entry.context = std::make_unique<ImeContext>(*context_);

  // A pending live-conversion preedit is partly owned by Session rather than
  // ImeContext: the converted stable prefix and the raw suffix are composed at
  // output time. Preserve that display state so Undo can restore exactly what
  // the user saw before submitting it. Generation numbers are intentionally not
  // stored; PopUndoContext() issues a fresh generation to keep old callbacks
  // stale.
  if (live_conversion_pending_ &&
      context_->state() == ImeContext::COMPOSITION) {
    PendingLiveConversionUndoState state;
    state.pending_key = pending_live_conversion_key_;
    state.pending_input = pending_live_conversion_input_;
    state.pending_presentation = pending_live_conversion_presentation_;
    state.pending_suggestion_candidate_window =
        pending_live_conversion_suggestion_candidate_window_;
    state.pending_suggestion_query = pending_live_conversion_suggestion_query_;
    if (pending_live_conversion_suggestion_context_ != nullptr) {
      state.pending_suggestion_context =
          std::make_unique<ImeContext>(
              *pending_live_conversion_suggestion_context_);
    }
    state.live_suggestion_candidate_window =
        live_conversion_suggestion_candidate_window_;
    state.live_suggestion_query = live_conversion_suggestion_query_;
    if (live_conversion_suggestion_context_ != nullptr) {
      state.live_suggestion_context =
          std::make_unique<ImeContext>(*live_conversion_suggestion_context_);
    }
    state.live_key = live_conversion_key_;
    state.live_preedit = live_conversion_preedit_;
    state.live_value = live_conversion_value_;
    state.live_preedit_output = live_conversion_preedit_output_;
    entry.pending_live_conversion = std::move(state);
  }

  undo_contexts_.push_back(std::move(entry));

  // If the stack size exceeds the limitation, purge the oldest entries.
  while (undo_contexts_.size() > kMultipleUndoMaxSize) {
    undo_contexts_.pop_front();
  }
  DCHECK_LE(undo_contexts_.size(), kMultipleUndoMaxSize);
}

void Session::PushDirectCommitUndoContext() {
  PushUndoContext();
  DCHECK(!undo_contexts_.empty());
  undo_contexts_.back().revert_converter_on_undo = false;
}

void Session::PopUndoContext() {
  if (!HasUndoContext()) {
    return;
  }

  UndoEntry entry = std::move(undo_contexts_.back());
  undo_contexts_.pop_back();
  context_ = std::move(entry.context);

  // Invalidate every callback issued before Undo. If the restored entry was
  // pending, it receives a new generation and a new callback from Undo().
  ClearLiveConversionState();

  if (!entry.pending_live_conversion.has_value() ||
      context_->state() != ImeContext::COMPOSITION) {
    return;
  }

  PendingLiveConversionUndoState& state =
      *entry.pending_live_conversion;
  const std::string current_key =
      context_->composer().GetQueryForConversion();
  if (state.pending_key.empty() || state.pending_key != current_key) {
    return;
  }

  live_conversion_pending_ = true;
  pending_live_conversion_generation_ = live_conversion_generation_;
  pending_live_conversion_key_ = std::move(state.pending_key);
  pending_live_conversion_input_ = std::move(state.pending_input);
  pending_live_conversion_suggestion_candidate_window_ =
      std::move(state.pending_suggestion_candidate_window);
  pending_live_conversion_suggestion_context_ =
      std::move(state.pending_suggestion_context);
  pending_live_conversion_suggestion_query_ =
      std::move(state.pending_suggestion_query);
  live_conversion_suggestion_candidate_window_ =
      std::move(state.live_suggestion_candidate_window);
  live_conversion_suggestion_context_ =
      std::move(state.live_suggestion_context);
  live_conversion_suggestion_query_ = std::move(state.live_suggestion_query);
  live_conversion_key_ = std::move(state.live_key);
  live_conversion_preedit_ = std::move(state.live_preedit);
  live_conversion_value_ = std::move(state.live_value);
  live_conversion_preedit_output_ =
      std::move(state.live_preedit_output);
  pending_live_conversion_presentation_ =
      std::move(state.pending_presentation);
}

bool Session::ShouldRevertConverterOnUndo() const {
  return HasUndoContext() &&
         undo_contexts_.back().revert_converter_on_undo;
}

void Session::ClearUndoContext() { undo_contexts_.clear(); }

bool Session::HasUndoContext() const { return !undo_contexts_.empty(); }

bool Session::IsCancelKeyForCompositionOrConversion(
    const commands::KeyEvent& key) const {
  const keymap::KeyMapManager* keymap = &context_->GetKeyMapManager();

  keymap::CompositionState::Commands composition_command;
  if (keymap->GetCommandComposition(key, &composition_command) &&
      composition_command == keymap::CompositionState::CANCEL) {
    return true;
  }

  keymap::ConversionState::Commands conversion_command;
  if (keymap->GetCommandConversion(key, &conversion_command) &&
      conversion_command == keymap::ConversionState::CANCEL) {
    return true;
  }

  return false;
}

void Session::MaybeSetUndoStatus(commands::Command* command) const {
  if (HasUndoContext()) {
    command->mutable_output()->mutable_status()->set_undo_available(true);
  }
}

void Session::EnsureIMEIsOn() {
  if (context_->state() == ImeContext::DIRECT) {
    SetSessionState(ImeContext::PRECOMPOSITION, context_.get());
  }
}

bool Session::SendCommand(commands::Command* command) {
  UpdateTime();
  UpdatePreferences(command);
  if (!command->input().has_command()) {
    return false;
  }
  TransformInput(command->mutable_input());

  const commands::SessionCommand& session_command = command->input().command();
  // SEND_COMMAND bypasses SendKey's generation invalidation. Candidate
  // selection/highlighting, cursor movement, and other client-side actions
  // must cancel a pending macro too, even when the reading stays unchanged.
  // Only query commands, timer callbacks, and the expected TSF reconversion
  // acknowledgement are allowed to preserve the suspended continuation.
  switch (session_command.type()) {
    case commands::SessionCommand::NONE:
    case commands::SessionCommand::GET_STATUS:
    case commands::SessionCommand::APPLY_LIVE_CONVERSION:
    case commands::SessionCommand::APPLY_SYSTEMONE_LIVE:
    case commands::SessionCommand::RESUME_COMMAND_SEQUENCE:
      break;
    case commands::SessionCommand::CONVERT_REVERSE:
      if (command_sequence_waits_for_reconversion_) break;
      [[fallthrough]];
    default:
      ++command_sequence_generation_;
      pending_command_sequence_.clear();
      pending_command_sequence_key_.clear();
      command_sequence_waits_for_reconversion_ = false;
      break;
  }
  HandlePendingDirectCommitLearningForSessionCommand(session_command.type());

  bool result = false;
  if (session_command.type() ==
      commands::SessionCommand::SWITCH_COMPOSITION_MODE) {
    if (!session_command.has_composition_mode()) {
      return false;
    }
    switch (session_command.composition_mode()) {
      case commands::DIRECT:
        // TODO(komatsu): Implement here.
        break;
      case commands::HIRAGANA:
        result = CompositionModeHiragana(command);
        break;
      case commands::FULL_KATAKANA:
        result = CompositionModeFullKatakana(command);
        break;
      case commands::HALF_ASCII:
        result = CompositionModeHalfASCII(command);
        break;
      case commands::FULL_ASCII:
        result = CompositionModeFullASCII(command);
        break;
      case commands::HALF_KATAKANA:
        result = CompositionModeHalfKatakana(command);
        break;
      default:
        LOG(ERROR) << "Unknown mode: " << session_command.composition_mode();
        break;
    }
    MaybeSetUndoStatus(command);
    return result;
  }

  DCHECK_EQ(false, result);
  switch (command->input().command().type()) {
    case commands::SessionCommand::NONE:
      result = DoNothing(command);
      break;
    case commands::SessionCommand::REVERT:
      result = Revert(command);
      break;
    case commands::SessionCommand::SUBMIT:
      result = Commit(command);
      break;
    case commands::SessionCommand::SELECT_CANDIDATE:
      result = SelectCandidate(command);
      break;
    case commands::SessionCommand::SUBMIT_CANDIDATE:
      result = CommitCandidate(command);
      break;
    case commands::SessionCommand::HIGHLIGHT_CANDIDATE:
      result = HighlightCandidate(command);
      break;
    case commands::SessionCommand::GET_STATUS:
      result = GetStatus(command);
      break;
    case commands::SessionCommand::CONVERT_REVERSE:
      result = ConvertReverse(command);
      break;
    case commands::SessionCommand::UNDO:
      result = Undo(command);
      break;
    case commands::SessionCommand::RESET_CONTEXT:
      result = ResetContext(command);
      break;
    case commands::SessionCommand::MOVE_CURSOR:
      result = MoveCursorTo(command);
      break;
    case commands::SessionCommand::SWITCH_INPUT_FIELD_TYPE:
      result = SwitchInputFieldType(command);
      break;
    case commands::SessionCommand::UNDO_OR_REWIND:
      result = UndoOrRewind(command);
      break;
    case commands::SessionCommand::COMMIT_RAW_TEXT:
      result = CommitRawText(command);
      break;
    case commands::SessionCommand::CONVERT_PREV_PAGE:
      result = ConvertPrevPage(command);
      break;
    case commands::SessionCommand::CONVERT_NEXT_PAGE:
      result = ConvertNextPage(command);
      break;
    case commands::SessionCommand::TURN_ON_IME:
      result = MakeSureIMEOn(command);
      break;
    case commands::SessionCommand::TURN_OFF_IME:
      result = MakeSureIMEOff(command);
      break;
    case commands::SessionCommand::DELETE_CANDIDATE_FROM_HISTORY:
      result = DeleteCandidateFromHistory(command);
      break;
    case commands::SessionCommand::STOP_KEY_TOGGLING:
      result = StopKeyToggling(command);
      break;
    case commands::SessionCommand::UPDATE_COMPOSITION:
      result = UpdateComposition(command);
      break;

    case commands::SessionCommand::APPLY_LIVE_CONVERSION:
      result = ApplyDelayedLiveConversion(command);
      break;

    case commands::SessionCommand::APPLY_SYSTEMONE_LIVE:
      result = ApplySystemOneLiveDecision(command);
      break;

    case commands::SessionCommand::RESUME_COMMAND_SEQUENCE:
      result = ResumeCommandSequence(command);
      break;

    case commands::SessionCommand::RECONVERT_SELECTION_OR_INSERT_SPACE:
      // This command is a client-side callback command.  It should normally be
      // handled by the TSF client from Output::Callback.  If it reaches the
      // server through SendCommand unexpectedly, consume it without changing the
      // current composition.
      result = DoNothing(command);
      break;

    case commands::SessionCommand::REQUEST_NWP: {
      ConversionPreferences conversion_preferences =
          context_->converter().conversion_preferences();
      conversion_preferences.request_suggestion =
          command->input().request_suggestion();
      // Resets converter's state (e.g. previous segments).
      // NWP will be generated from surrounding text given by the client.
      context_->mutable_converter()->Reset();
      result = context_->mutable_converter()->Suggest(
          context_->composer(), command->input().context(),
          conversion_preferences);
      if (result) {
        Output(command);
      }
      break;
    }
    default:
      LOG(WARNING) << "Unknown command" << *command;
      result = DoNothing(command);
      break;
  }
  if (context_->state() != ImeContext::CONVERSION) {
    live_conversion_active_ = false;
    live_conversion_from_composition_ = false;
  }

  MaybeSetUndoStatus(command);
  return result;
}

bool Session::TestSendKey(commands::Command* command) {
  UpdateTime();
  UpdatePreferences(command);
  TransformInput(command->mutable_input());

  if (context_->state() == ImeContext::NONE) {
    // This must be an error.
    LOG(ERROR) << "Invalid state: NONE";
    return false;
  }

  const commands::KeyEvent& key = command->input().key();

  // To support indirect IME on/off by using KeyEvent::activated, use effective
  // state instead of directly using context_->state().
  const ImeContext::State state =
      GetEffectiveStateForTestSendKey(key, context_->state());

  const keymap::KeyMapManager* keymap = &context_->GetKeyMapManager();

  // Direct input
  if (state == ImeContext::DIRECT) {
    keymap::DirectInputState::Commands key_command;
    if (!keymap->GetCommandDirect(key, &key_command) ||
        key_command == keymap::DirectInputState::NONE) {
      return EchoBack(command);
    }
    return DoNothing(command);
  }

  // Precomposition
  if (state == ImeContext::PRECOMPOSITION) {
    keymap::PrecompositionState::Commands key_command;
    const bool is_suggestion =
        context_->converter().CheckState(EngineConverterInterface::SUGGESTION);
    const bool result =
        is_suggestion ? keymap->GetCommandZeroQuerySuggestion(key, &key_command)
                      : keymap->GetCommandPrecomposition(key, &key_command);
    if (!result || key_command == keymap::PrecompositionState::NONE) {
      if (HasUndoContext() && IsCancelKeyForCompositionOrConversion(key)) {
        return Revert(command);
      }

      if (pending_direct_commit_learning_.pending &&
          IsCancelKeyForCompositionOrConversion(key)) {
        // A direct-commit punctuation/symbol may have already sent text to the
        // application without creating Mozc's undo context. A cancel-like key
        // such as Ctrl+Z should still cancel the pending learning, while the
        // key itself is echoed back so that the application can undo the
        // visible text.
        DiscardPendingDirectCommitLearning(
            "precomposition_cancel_key_after_direct_commit_learning");
        return EchoBackAndClearUndoContext(command);
      }

      // Clear undo context just in case. b/5529702.
      // Note that the undo context will not be cleared in
      // EchoBackAndClearUndoContext if the key event consists of modifier keys
      // only.
      return EchoBackAndClearUndoContext(command);
    }
    // If the input_style is DIRECT_INPUT, KeyEvent is not consumed
    // and done echo back.  It works only when key_string is equal to
    // key_code.  We should fix this limitation when the as_is flag is
    // used for rather than numpad characters.
    if (key_command == keymap::PrecompositionState::INSERT_CHARACTER &&
        key.input_style() == commands::KeyEvent::DIRECT_INPUT) {
      return EchoBack(command);
    }

    // TODO(komatsu): This is a hack to work around the problem with
    // the inconsistency between TestSendKey and SendKey.
    switch (key_command) {
      case keymap::PrecompositionState::INSERT_SPACE:
        if (!IsFullWidthInsertSpace(command->input()) && IsPureSpaceKey(key)) {
          return EchoBackAndClearUndoContext(command);
        }
        return DoNothing(command);
      case keymap::PrecompositionState::RECONVERT_SELECTION_OR_INSERT_SPACE:
        // TestSendKey must route this command to the client even when the
        // fallback space is half-width.  SendKey still keeps the normal
        // InsertSpace pass-through semantics so an unselected half-width Space
        // can ultimately be forwarded to the application.
        return DoNothing(command);
      case keymap::PrecompositionState::INSERT_ALTERNATE_SPACE:
        if (IsFullWidthInsertSpace(command->input()) && IsPureSpaceKey(key)) {
          return EchoBackAndClearUndoContext(command);
        }
        return DoNothing(command);
      case keymap::PrecompositionState::INSERT_HALF_SPACE:
        if (IsPureSpaceKey(key)) {
          return EchoBackAndClearUndoContext(command);
        }
        return DoNothing(command);
      case keymap::PrecompositionState::INSERT_FULL_SPACE:
        return DoNothing(command);
      default:
        // Do nothing.
        break;
    }

    if (key_command == keymap::PrecompositionState::REVERT) {
      return Revert(command);
    }

    // If undo context is empty, echoes back the key event so that it can be
    // handled by the application. b/5553298
    if (key_command == keymap::PrecompositionState::UNDO && !HasUndoContext()) {
      return EchoBack(command);
    }

    return DoNothing(command);
  }

  // Do nothing.
  return DoNothing(command);
}

bool Session::SendKey(commands::Command* command) {
  // A new physical key always invalidates a suspended macro, including a
  // reconversion continuation that has not reached the TSF callback yet.
  ++command_sequence_generation_;
  pending_command_sequence_.clear();
  pending_command_sequence_key_.clear();
  command_sequence_waits_for_reconversion_ = false;
  UpdateTime();
  UpdatePreferences(command);
  TransformInput(command->mutable_input());
  // To support indirect IME on/off by using KeyEvent::activated, use effective
  // state instead of directly using context_->state().
  HandleIndirectImeOnOff(command);

  TraceStart();

  bool result = false;
  switch (context_->state()) {
    case ImeContext::DIRECT:
      result = SendKeyDirectInputState(command);
      break;

    case ImeContext::PRECOMPOSITION:
      result = SendKeyPrecompositionState(command);
      break;

    case ImeContext::COMPOSITION:
      result = SendKeyCompositionState(command);
      break;

    case ImeContext::CONVERSION:
      result = SendKeyConversionState(command);
      break;

    case ImeContext::NONE:
      result = false;
      break;
  }

  if (context_->state() != ImeContext::CONVERSION) {
    live_conversion_active_ = false;
    live_conversion_from_composition_ = false;
  }

  MaybeSetUndoStatus(command);
  if (command->input().has_key()) {
    const std::string key_text =
        command->input().key().key_string().empty()
            ? std::string("(non-text key)")
            : command->input().key().key_string();
    TraceStep("Display Output", TraceOutputSummary(command->output()));
    TraceFlush(absl::StrCat("Key: '", TraceText(key_text), "'"));
  }
  return result;
}

bool Session::UpdateCompositionInternal(commands::Command* command) {
  command->mutable_output()->set_consumed(true);

  context_->mutable_composer()->Reset();
  // Use the top entry for now.
  context_->mutable_composer()->SetCompositionsForHandwriting(
      command->input().command().composition_events());
  ClearUndoContext();
  SetSessionState(ImeContext::COMPOSITION, context_.get());

  if (Suggest(command->input())) {
    Output(command);
    return true;
  }

  OutputComposition(command);
  return true;
}

bool Session::UpdateComposition(commands::Command* command) {
  bool result = false;
  switch (context_->state()) {
    case ImeContext::DIRECT:
      result = EchoBackAndClearUndoContext(command);
      break;

    case ImeContext::PRECOMPOSITION:
      [[fallthrough]];
    case ImeContext::COMPOSITION:
      result = UpdateCompositionInternal(command);
      break;

    case ImeContext::CONVERSION:
      result = false;
      break;

    case ImeContext::NONE:
      result = false;
      break;
  }
  return result;
}

bool Session::ExecuteCommandSequence(
    const keymap::CommandSequence& command_sequence,
    commands::Command* command) {
  return ExecuteCommandSequenceWithInitialOutput(command_sequence, nullptr,
                                                 command);
}

bool Session::ExecuteCommandSequenceWithInitialOutput(
    const keymap::CommandSequence& command_sequence,
    const commands::Output* initial_output,
    commands::Command* command) {
  bool executed = false;
  bool consumed = false;
  bool has_accumulated_result = false;
  commands::Result accumulated_result;
  commands::Output final_output;

  if (initial_output != nullptr) {
    AccumulateCommandOutput(*initial_output,
                            &consumed,
                            &has_accumulated_result,
                            &accumulated_result,
                            &final_output);
    executed = true;
  }

  for (size_t i = 0; i < command_sequence.size(); ++i) {
    const std::string& command_name = command_sequence[i];
    if (command_name.empty()) continue;

    const std::optional<uint32_t> delay = ParseKeymapDelay(command_name);
    if (delay.has_value()) {
      if (*delay == 0 || i + 1 == command_sequence.size()) continue;
      // An earlier command may already have installed an asynchronous
      // callback (live conversion, undo, etc.). Never overwrite it with
      // RESUME_COMMAND_SEQUENCE: preserving that callback is more important
      // than running unsupported chained steps. Malformed imported keymaps
      // can reach here even if the normal keymap validator rejects them.
      if (final_output.has_callback()) {
        LOG(WARNING) << "Skipping keymap Delay after asynchronous callback";
        final_output.set_consumed(consumed);
        if (has_accumulated_result) {
          *final_output.mutable_result() = accumulated_result;
        }
        *command->mutable_output() = final_output;
        return true;
      }
      pending_command_sequence_.assign(command_sequence.begin() + i + 1,
                                       command_sequence.end());
      command_sequence_waits_for_reconversion_ = false;
      pending_command_sequence_key_ = context_->composer().GetQueryForConversion();
      commands::Output::Callback* callback =
          final_output.mutable_callback();
      callback->mutable_session_command()->set_type(
          commands::SessionCommand::RESUME_COMMAND_SEQUENCE);
      callback->mutable_session_command()->set_command_sequence_generation(
          ++command_sequence_generation_);
      callback->set_delay_millisec(*delay);
      // Delaying the remainder must not change the original key's
      // pass-through semantics (e.g. a half-width Space).
      final_output.set_consumed(consumed);
      if (has_accumulated_result) {
        *final_output.mutable_result() = accumulated_result;
      }
      *command->mutable_output() = final_output;
      return true;
    }

    command->mutable_output()->Clear();

    if (!ExecuteCommandName(command_name, command)) {
      if (!executed) {
        return DoNothing(command);
      }

      final_output.set_consumed(consumed);
      if (has_accumulated_result) {
        *final_output.mutable_result() = accumulated_result;
      }
      *command->mutable_output() = final_output;
      return true;
    }

    executed = true;

    const commands::Output step_output = command->output();
    AccumulateCommandOutput(step_output,
                            &consumed,
                            &has_accumulated_result,
                            &accumulated_result,
                            &final_output);

    if (command_name == "ReconvertSelectionOrInsertSpace" &&
        i + 1 < command_sequence.size() &&
        step_output.has_callback() &&
        step_output.callback().has_session_command() &&
        step_output.callback().session_command().type() ==
            commands::SessionCommand::RECONVERT_SELECTION_OR_INSERT_SPACE) {
      pending_command_sequence_.assign(command_sequence.begin() + i + 1,
                                       command_sequence.end());
      command_sequence_waits_for_reconversion_ = true;
      pending_command_sequence_key_.clear();
      ++command_sequence_generation_;
      // The client still decides whether a selection is present. In the
      // unselected case the original half-width Space must pass through.
      final_output.set_consumed(consumed);
      *command->mutable_output() = final_output;
      return true;
    }
  }

  if (!executed) {
    return DoNothing(command);
  }

  final_output.set_consumed(consumed);
  if (has_accumulated_result) {
    *final_output.mutable_result() = accumulated_result;
  }
  *command->mutable_output() = final_output;
  return true;
}

bool Session::ResumeCommandSequence(commands::Command* command) {
  command->mutable_output()->set_consumed(true);
  const commands::SessionCommand& resume = command->input().command();
  if (!resume.has_command_sequence_generation() ||
      resume.command_sequence_generation() != command_sequence_generation_ ||
      pending_command_sequence_.empty()) {
    return IgnoreStaleDelayedLiveConversion(command);
  }
  if (!command_sequence_waits_for_reconversion_ &&
      context_->composer().GetQueryForConversion() !=
          pending_command_sequence_key_) {
    pending_command_sequence_.clear();
    pending_command_sequence_key_.clear();
    return IgnoreStaleDelayedLiveConversion(command);
  }
  if (command_sequence_waits_for_reconversion_) {
    // Never resume client reconversion until TSF has made a real preedit.
    if (context_->state() != ImeContext::CONVERSION) {
      pending_command_sequence_.clear();
      command_sequence_waits_for_reconversion_ = false;
      return IgnoreStaleDelayedLiveConversion(command);
    }
  }
  keymap::CommandSequence remaining = std::move(pending_command_sequence_);
  pending_command_sequence_.clear();
  command_sequence_waits_for_reconversion_ = false;
  pending_command_sequence_key_.clear();
  ++command_sequence_generation_;
  // A continuation may only install a new callback, not emit fresh preedit.
  // The baseline output must be carried forward, or the Windows TSF client
  // will interpret an empty output as EndComposition.
  OutputFromState(command);
  const commands::Output initial = command->output();
  return ExecuteCommandSequenceWithInitialOutput(remaining, &initial, command);
}

bool Session::ExecuteCommandName(const std::string& command_name,
                                 commands::Command* command) {
  const keymap::KeyMapManager* keymap = &context_->GetKeyMapManager();

  switch (context_->state()) {
    case ImeContext::DIRECT: {
      keymap::DirectInputState::Commands key_command;
      if (!keymap->ResolveDirectCommandName(command_name, &key_command)) {
        return false;
      }
      return ExecuteDirectInputCommand(key_command, command);
    }

    case ImeContext::PRECOMPOSITION: {
      keymap::PrecompositionState::Commands key_command;
      if (!keymap->ResolvePrecompositionCommandName(command_name,
                                                    &key_command)) {
        return false;
      }
      return ExecutePrecompositionCommand(key_command, command);
    }

    case ImeContext::COMPOSITION: {
      keymap::CompositionState::Commands key_command;
      if (!keymap->ResolveCompositionCommandName(command_name, &key_command)) {
        return false;
      }
      return ExecuteCompositionCommand(key_command, command);
    }

    case ImeContext::CONVERSION: {
      keymap::ConversionState::Commands key_command;
      if (!keymap->ResolveConversionCommandName(command_name, &key_command)) {
        return false;
      }
      return ExecuteConversionCommand(key_command, command);
    }

    case ImeContext::NONE:
      return false;
  }

  return false;
}

bool Session::ExecuteDirectInputCommand(
    keymap::DirectInputState::Commands key_command,
    commands::Command* command) {
  switch (key_command) {
    case keymap::DirectInputState::IME_ON:
      return IMEOn(command);
    case keymap::DirectInputState::COMPOSITION_MODE_HIRAGANA:
      return CompositionModeHiragana(command);
    case keymap::DirectInputState::COMPOSITION_MODE_FULL_KATAKANA:
      return CompositionModeFullKatakana(command);
    case keymap::DirectInputState::COMPOSITION_MODE_HALF_KATAKANA:
      return CompositionModeHalfKatakana(command);
    case keymap::DirectInputState::COMPOSITION_MODE_FULL_ALPHANUMERIC:
      return CompositionModeFullASCII(command);
    case keymap::DirectInputState::COMPOSITION_MODE_HALF_ALPHANUMERIC:
      return CompositionModeHalfASCII(command);
    case keymap::DirectInputState::NONE:
      return EchoBackAndClearUndoContext(command);
    case keymap::DirectInputState::RECONVERT:
      return RequestConvertReverse(command);
  }

  return false;
}

bool Session::ExecutePrecompositionCommand(
    keymap::PrecompositionState::Commands key_command,
    commands::Command* command) {
  switch (key_command) {
    case keymap::PrecompositionState::INSERT_CHARACTER:
      return InsertCharacter(command);
    case keymap::PrecompositionState::INSERT_SPACE:
      return InsertSpace(command);
    case keymap::PrecompositionState::INSERT_ALTERNATE_SPACE:
      return InsertSpaceToggled(command);
    case keymap::PrecompositionState::INSERT_HALF_SPACE:
      return InsertSpaceHalfWidth(command);
    case keymap::PrecompositionState::INSERT_FULL_SPACE:
      return InsertSpaceFullWidth(command);
    case keymap::PrecompositionState::TOGGLE_ALPHANUMERIC_MODE:
      return ToggleAlphanumericMode(command);
    case keymap::PrecompositionState::REVERT:
      return Revert(command);
    case keymap::PrecompositionState::UNDO:
      return RequestUndo(command);
    case keymap::PrecompositionState::IME_OFF:
      return IMEOff(command);
    case keymap::PrecompositionState::IME_ON:
      return IMEOnWhenAlreadyOn(command);

    case keymap::PrecompositionState::COMPOSITION_MODE_HIRAGANA:
      return CompositionModeHiragana(command);
    case keymap::PrecompositionState::COMPOSITION_MODE_FULL_KATAKANA:
      return CompositionModeFullKatakana(command);
    case keymap::PrecompositionState::COMPOSITION_MODE_HALF_KATAKANA:
      return CompositionModeHalfKatakana(command);
    case keymap::PrecompositionState::COMPOSITION_MODE_FULL_ALPHANUMERIC:
      return CompositionModeFullASCII(command);
    case keymap::PrecompositionState::COMPOSITION_MODE_HALF_ALPHANUMERIC:
      return CompositionModeHalfASCII(command);
    case keymap::PrecompositionState::COMPOSITION_MODE_SWITCH_KANA_TYPE:
      return CompositionModeSwitchKanaType(command);

    case keymap::PrecompositionState::LAUNCH_CONFIG_DIALOG:
      return LaunchConfigDialog(command);
    case keymap::PrecompositionState::LAUNCH_DICTIONARY_TOOL:
      return LaunchDictionaryTool(command);
    case keymap::PrecompositionState::LAUNCH_WORD_REGISTER_DIALOG:
      return LaunchWordRegisterDialog(command);

    case keymap::PrecompositionState::CANCEL:
      return EditCancel(command);
    case keymap::PrecompositionState::CANCEL_AND_IME_OFF:
      return EditCancelAndIMEOff(command);
    case keymap::PrecompositionState::COMMIT_FIRST_SUGGESTION:
      return CommitFirstSuggestion(command);
    case keymap::PrecompositionState::PREDICT_AND_CONVERT:
      return PredictAndConvert(command);

    case keymap::PrecompositionState::NONE:
      if (HasUndoContext() &&
          IsCancelKeyForCompositionOrConversion(command->input().key())) {
        return Revert(command);
      }
      return EchoBackAndClearUndoContext(command);
    case keymap::PrecompositionState::RECONVERT:
      return RequestConvertReverse(command);
    case keymap::PrecompositionState::RECONVERT_SELECTION_OR_INSERT_SPACE:
      return RequestReconvertSelectionOrInsertSpace(command);

    case keymap::PrecompositionState::IME_ACTION:
      return ImeAction(command);
  }

  return false;
}

bool Session::ExecuteCompositionCommand(
    keymap::CompositionState::Commands key_command,
    commands::Command* command) {
  switch (key_command) {
    case keymap::CompositionState::INSERT_CHARACTER:
      return InsertCharacter(command);

    case keymap::CompositionState::COMMIT:
      return Commit(command);

    case keymap::CompositionState::COMMIT_FIRST_SUGGESTION:
      return CommitFirstSuggestion(command);

    case keymap::CompositionState::CONVERT:
      return Convert(command);

    case keymap::CompositionState::CONVERT_WITHOUT_HISTORY:
      return ConvertWithoutHistory(command);

    case keymap::CompositionState::PREDICT_AND_CONVERT:
      return PredictAndConvert(command);

    case keymap::CompositionState::DEL:
      return Delete(command);

    case keymap::CompositionState::BACKSPACE:
      return Backspace(command);

    case keymap::CompositionState::INSERT_SPACE:
      return InsertSpace(command);

    case keymap::CompositionState::INSERT_ALTERNATE_SPACE:
      return InsertSpaceToggled(command);

    case keymap::CompositionState::INSERT_HALF_SPACE:
      return InsertSpaceHalfWidth(command);

    case keymap::CompositionState::INSERT_FULL_SPACE:
      return InsertSpaceFullWidth(command);

    case keymap::CompositionState::MOVE_CURSOR_LEFT:
      return MoveCursorLeft(command);

    case keymap::CompositionState::MOVE_CURSOR_RIGHT:
      return MoveCursorRight(command);

    case keymap::CompositionState::MOVE_CURSOR_TO_BEGINNING:
      return MoveCursorToBeginning(command);

    case keymap::CompositionState::MOVE_MOVE_CURSOR_TO_END:
      return MoveCursorToEnd(command);

    case keymap::CompositionState::CANCEL:
      return EditCancel(command);

    case keymap::CompositionState::CANCEL_AND_IME_OFF:
      return EditCancelAndIMEOff(command);

    case keymap::CompositionState::UNDO:
      return RequestUndo(command);

    case keymap::CompositionState::IME_OFF:
      return IMEOff(command);

    case keymap::CompositionState::IME_ON:
      return IMEOnWhenAlreadyOn(command);

    case keymap::CompositionState::CONVERT_TO_HIRAGANA:
      return ConvertToHiragana(command);

    case keymap::CompositionState::CONVERT_TO_FULL_KATAKANA:
      return ConvertToFullKatakana(command);

    case keymap::CompositionState::CONVERT_TO_HALF_KATAKANA:
      return ConvertToHalfKatakana(command);

    case keymap::CompositionState::CONVERT_TO_HALF_WIDTH:
      return ConvertToHalfWidth(command);

    case keymap::CompositionState::CONVERT_TO_FULL_ALPHANUMERIC:
      return ConvertToFullASCII(command);

    case keymap::CompositionState::CONVERT_TO_HALF_ALPHANUMERIC:
      return ConvertToHalfASCII(command);

    case keymap::CompositionState::SWITCH_KANA_TYPE:
      return SwitchKanaType(command);

    case keymap::CompositionState::DISPLAY_AS_HIRAGANA:
      return DisplayAsHiragana(command);

    case keymap::CompositionState::DISPLAY_AS_FULL_KATAKANA:
      return DisplayAsFullKatakana(command);

    case keymap::CompositionState::DISPLAY_AS_HALF_KATAKANA:
      return DisplayAsHalfKatakana(command);

    case keymap::CompositionState::TRANSLATE_HALF_WIDTH:
      return TranslateHalfWidth(command);

    case keymap::CompositionState::TRANSLATE_FULL_ASCII:
      return TranslateFullASCII(command);

    case keymap::CompositionState::TRANSLATE_HALF_ASCII:
      return TranslateHalfASCII(command);

    case keymap::CompositionState::TOGGLE_ALPHANUMERIC_MODE:
      return ToggleAlphanumericMode(command);

    case keymap::CompositionState::COMPOSITION_MODE_HIRAGANA:
      return CompositionModeHiragana(command);

    case keymap::CompositionState::COMPOSITION_MODE_FULL_KATAKANA:
      return CompositionModeFullKatakana(command);

    case keymap::CompositionState::COMPOSITION_MODE_HALF_KATAKANA:
      return CompositionModeHalfKatakana(command);

    case keymap::CompositionState::COMPOSITION_MODE_FULL_ALPHANUMERIC:
      return CompositionModeFullASCII(command);

    case keymap::CompositionState::COMPOSITION_MODE_HALF_ALPHANUMERIC:
      return CompositionModeHalfASCII(command);

    case keymap::CompositionState::NONE:
      return DoNothing(command);
  }

  return false;
}

bool Session::ExecuteConversionCommand(
    keymap::ConversionState::Commands key_command,
    commands::Command* command) {
  switch (key_command) {
    case keymap::ConversionState::BACKSPACE:
      // Backspace() edits the composer, not the visible kanji surface. An
      // ordinary (non-live) conversion must first return to composition.
      if (!live_conversion_active_ &&
          context_->state() == ImeContext::CONVERSION &&
          !ConvertCancel(command)) {
        return false;
      }
      return Backspace(command);
    case keymap::ConversionState::INSERT_CHARACTER:
      return InsertCharacter(command);

    case keymap::ConversionState::INSERT_SPACE:
      return InsertSpace(command);

    case keymap::ConversionState::INSERT_ALTERNATE_SPACE:
      return InsertSpaceToggled(command);

    case keymap::ConversionState::INSERT_HALF_SPACE:
      return InsertSpaceHalfWidth(command);

    case keymap::ConversionState::INSERT_FULL_SPACE:
      return InsertSpaceFullWidth(command);

    case keymap::ConversionState::COMMIT:
      return Commit(command);

    case keymap::ConversionState::COMMIT_SEGMENT:
      return CommitSegment(command);

    case keymap::ConversionState::CONVERT_NEXT:
      return ConvertNext(command);

    case keymap::ConversionState::CONVERT_PREV:
      return ConvertPrev(command);

    case keymap::ConversionState::CONVERT_NEXT_PAGE:
      return ConvertNextPage(command);

    case keymap::ConversionState::CONVERT_PREV_PAGE:
      return ConvertPrevPage(command);

    case keymap::ConversionState::PREDICT_AND_CONVERT:
      return PredictAndConvert(command);

    case keymap::ConversionState::SEGMENT_FOCUS_LEFT:
      return SegmentFocusLeft(command);

    case keymap::ConversionState::SEGMENT_FOCUS_RIGHT:
      return SegmentFocusRight(command);

    case keymap::ConversionState::SEGMENT_FOCUS_FIRST:
      return SegmentFocusLeftEdge(command);

    case keymap::ConversionState::SEGMENT_FOCUS_LAST:
      return SegmentFocusLast(command);

    case keymap::ConversionState::SEGMENT_WIDTH_EXPAND:
      return SegmentWidthExpand(command);

    case keymap::ConversionState::SEGMENT_WIDTH_SHRINK:
      return SegmentWidthShrink(command);

    case keymap::ConversionState::CANCEL:
      return ConvertCancel(command);

    case keymap::ConversionState::CANCEL_AND_IME_OFF:
      return EditCancelAndIMEOff(command);

    case keymap::ConversionState::UNDO:
      return RequestUndo(command);

    case keymap::ConversionState::IME_OFF:
      return IMEOff(command);

    case keymap::ConversionState::IME_ON:
      return IMEOnWhenAlreadyOn(command);

    case keymap::ConversionState::CONVERT_TO_HIRAGANA:
      return ConvertToHiragana(command);

    case keymap::ConversionState::CONVERT_TO_FULL_KATAKANA:
      return ConvertToFullKatakana(command);

    case keymap::ConversionState::CONVERT_TO_HALF_KATAKANA:
      return ConvertToHalfKatakana(command);

    case keymap::ConversionState::CONVERT_TO_HALF_WIDTH:
      return ConvertToHalfWidth(command);

    case keymap::ConversionState::CONVERT_TO_FULL_ALPHANUMERIC:
      return ConvertToFullASCII(command);

    case keymap::ConversionState::CONVERT_TO_HALF_ALPHANUMERIC:
      return ConvertToHalfASCII(command);

    case keymap::ConversionState::SWITCH_KANA_TYPE:
      return SwitchKanaType(command);

    case keymap::ConversionState::DISPLAY_AS_HIRAGANA:
      return DisplayAsHiragana(command);

    case keymap::ConversionState::DISPLAY_AS_FULL_KATAKANA:
      return DisplayAsFullKatakana(command);

    case keymap::ConversionState::DISPLAY_AS_HALF_KATAKANA:
      return DisplayAsHalfKatakana(command);

    case keymap::ConversionState::TRANSLATE_HALF_WIDTH:
      return TranslateHalfWidth(command);

    case keymap::ConversionState::TRANSLATE_FULL_ASCII:
      return TranslateFullASCII(command);

    case keymap::ConversionState::TRANSLATE_HALF_ASCII:
      return TranslateHalfASCII(command);

    case keymap::ConversionState::TOGGLE_ALPHANUMERIC_MODE:
      return ToggleAlphanumericMode(command);

    case keymap::ConversionState::COMPOSITION_MODE_HIRAGANA:
      return CompositionModeHiragana(command);

    case keymap::ConversionState::COMPOSITION_MODE_FULL_KATAKANA:
      return CompositionModeFullKatakana(command);

    case keymap::ConversionState::COMPOSITION_MODE_HALF_KATAKANA:
      return CompositionModeHalfKatakana(command);

    case keymap::ConversionState::COMPOSITION_MODE_FULL_ALPHANUMERIC:
      return CompositionModeFullASCII(command);

    case keymap::ConversionState::COMPOSITION_MODE_HALF_ALPHANUMERIC:
      return CompositionModeHalfASCII(command);

    case keymap::ConversionState::REPORT_BUG:
      return ReportBug(command);

    case keymap::ConversionState::DELETE_SELECTED_CANDIDATE:
      return DeleteSelectedCandidate(command);

    case keymap::ConversionState::NONE:
      return DoNothing(command);
  }

  return false;
}

bool Session::SendKeyDirectInputState(commands::Command* command) {
  keymap::CommandSequence command_sequence;
  const keymap::KeyMapManager* keymap = &context_->GetKeyMapManager();
  if (!keymap->GetCommandSequenceDirect(command->input().key(),
                                        &command_sequence)) {
    return EchoBackAndClearUndoContext(command);
  }

  return ExecuteCommandSequence(command_sequence, command);
}

bool Session::SendKeyPrecompositionState(commands::Command* command) {
  keymap::CommandSequence command_sequence;
  const keymap::KeyMapManager* keymap = &context_->GetKeyMapManager();
  const bool result =
      context_->converter().CheckState(EngineConverterInterface::SUGGESTION)
          ? keymap->GetCommandSequenceZeroQuerySuggestion(
                command->input().key(), &command_sequence)
          : keymap->GetCommandSequencePrecomposition(command->input().key(),
                                                     &command_sequence);

  if (!result) {
    if (HasUndoContext() &&
        IsCancelKeyForCompositionOrConversion(command->input().key())) {
      return Revert(command);
    }

    if (pending_direct_commit_learning_.pending &&
        IsCancelKeyForCompositionOrConversion(command->input().key())) {
      // A direct-commit punctuation/symbol may have already sent text to the
      // application without creating Mozc's undo context. A cancel-like key
      // such as Ctrl+Z should still cancel the pending learning, while the
      // key itself is echoed back so that the application can undo the
      // visible text.
      DiscardPendingDirectCommitLearning(
          "precomposition_cancel_key_after_direct_commit_learning");
      return EchoBackAndClearUndoContext(command);
    }
    return EchoBackAndClearUndoContext(command);
  }

  // Update the client context (if any) for later use. Note that the client
  // context is updated only here. In other words, we will stop updating the
  // client context once a conversion starts (mainly for performance reasons).
  if (command->has_input() && command->input().has_context()) {
    *context_->mutable_client_context() = command->input().context();

#if defined(_WIN32) && defined(MOZC_LEFT_CONTEXT_DEBUG)
    const commands::Context& client_context = command->input().context();
    if (client_context.has_preceding_text()) {
      MozcLeftContextDebugOutput(absl::StrCat(
          "[mozc-left-context] session preceding_text=[",
          client_context.preceding_text(), "]"));
    } else {
      MozcLeftContextDebugOutput(
          "[mozc-left-context] session context has no preceding_text");
    }
#endif  // defined(_WIN32) && defined(MOZC_LEFT_CONTEXT_DEBUG)

  } else {
    context_->mutable_client_context()->Clear();

#if defined(_WIN32) && defined(MOZC_LEFT_CONTEXT_DEBUG)
    MozcLeftContextDebugOutput("[mozc-left-context] session no context");
#endif  // defined(_WIN32) && defined(MOZC_LEFT_CONTEXT_DEBUG)
  }

  return ExecuteCommandSequence(command_sequence, command);
}

bool Session::SendKeyCompositionState(commands::Command* command) {
  keymap::CommandSequence command_sequence;
  const keymap::KeyMapManager* keymap = &context_->GetKeyMapManager();
  const bool result =
      context_->converter().CheckState(EngineConverterInterface::SUGGESTION)
          ? keymap->GetCommandSequenceSuggestion(command->input().key(),
                                                 &command_sequence)
          : keymap->GetCommandSequenceComposition(command->input().key(),
                                                  &command_sequence);

  if (!result) {
    return DoNothing(command);
  }

  return ExecuteCommandSequence(command_sequence, command);
}

bool Session::SendKeyConversionState(commands::Command* command) {
  keymap::CommandSequence command_sequence;
  const keymap::KeyMapManager* keymap = &context_->GetKeyMapManager();
  const bool is_prediction =
      context_->converter().CheckState(EngineConverterInterface::PREDICTION);
  const bool live_keymap_active = live_conversion_active_ &&
      live_conversion_from_composition_ &&
      context_->GetConfig().use_live_conversion();
  bool specialized_override = false;
  bool result = false;
  if (is_prediction) {
    result = keymap->GetCommandSequencePrediction(command->input().key(),
                                                 &command_sequence);
  } else if (live_keymap_active) {
    result = keymap->GetCommandSequenceLiveConversion(
        command->input().key(), &command_sequence, &specialized_override);
  } else {
    result = keymap->GetCommandSequenceConversion(command->input().key(),
                                                  &command_sequence);
  }

  if (!result || command_sequence.empty()) {
    return DoNothing(command);
  }

  keymap::ConversionState::Commands key_command;
  if (!keymap->ResolveConversionCommandName(command_sequence.front(),
                                            &key_command)) {
    return DoNothing(command);
  }

  const commands::KeyEvent& input_key = command->input().key();

  // Honor an explicit editing command on any bound key. Keep the command
  // sequence dispatcher responsible for executing macro continuations.
  if (key_command == keymap::ConversionState::BACKSPACE) {
    return ExecuteCommandSequence(command_sequence, command);
  }

  // Space in the middle of an English phrase types a space, exactly as it does
  // in Composition state (see Session::Convert).  Live conversion keeps the
  // context in CONVERSION state while the user is still typing, so without
  // this the space would reach ConvertNext and end the English phrase
  // mid-word.  CompositionEndsWithEnglishPiece() already refuses ordinary
  // conversion (only a live composition qualifies), so a real candidate list
  // keeps Space = ConvertNext.
  if ((live_conversion_active_ || live_conversion_pending_) &&
      IsPureSpaceKey(input_key) && CompositionEndsWithEnglishPiece()) {
    command->mutable_input()->mutable_key()->set_key_code(' ');
    return InsertCharacter(command);
  }

  if (live_conversion_active_) {
    // Backward compatibility with older CUSTOM tables lacking a specialized
    // Backspace row. Built-in maps now express this behavior explicitly in
    // LiveConversion, and custom explicit overrides always win.
    if (live_keymap_active && IsPlainBackspaceKey(input_key) &&
        key_command == keymap::ConversionState::CANCEL &&
        !specialized_override) {
      return Backspace(command);
    }

    // A prediction key such as Tab should focus prediction candidates even while
    // live conversion is active.  Do this before the generic live-conversion
    // promotion below, otherwise PredictAndConvert() would see an ordinary
    // CONVERSION state and fall back to ConvertNext().  Other conversion
    // commands still follow their existing keymap-defined conversion path.
    if (key_command == keymap::ConversionState::PREDICT_AND_CONVERT) {
      if (!PredictAndConvertFromLiveConversion(command)) {
        return false;
      }

      if (command_sequence.size() == 1) {
        return true;
      }

      const commands::Output prediction_output = command->output();
      keymap::CommandSequence remaining_sequence(
          command_sequence.begin() + 1, command_sequence.end());
      return ExecuteCommandSequenceWithInitialOutput(
          remaining_sequence, &prediction_output, command);
    }

    // Explicit conversion operations such as Space, candidate movement, or Cancel
    // promote live conversion back to normal conversion behavior.  COMMIT is
    // not one of them: Enter accepts the text already on screen, and tearing
    // the live-conversion state down first would make Commit() fall back to
    // the converter's own segmentation — committing a surface the user never
    // saw (the un-protected English reading, or Mozc's mis-conversion where
    // the piece pipeline promoted a typo correction).
    if (key_command != keymap::ConversionState::INSERT_CHARACTER &&
        key_command != keymap::ConversionState::COMMIT) {
      live_conversion_active_ = false;
      live_conversion_from_composition_ = false;
      context_->mutable_converter()->SetCandidateListVisible(true);
    }
  }

  return ExecuteCommandSequence(command_sequence, command);
}

void Session::UpdatePreferences(commands::Command* command) {
  DCHECK(command);
  const config::Config& config = command->input().config();
  if (command->input().has_capability()) {
    *context_->mutable_client_capability() = command->input().capability();
  }

  // Update config values modified temporarily.
  // TODO(team): Stop using config for temporary modification.
  if (config.has_selection_shortcut()) {
    context_->mutable_converter()->set_selection_shortcut(
        config.selection_shortcut());
  }

#if (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE) || defined(__linux__) || \
    defined(__wasm__)
  context_->mutable_converter()->set_use_cascading_window(false);
#else   // TARGET_OS_IPHONE || __linux__ || __wasm__
  if (config.has_use_cascading_window()) {
    context_->mutable_converter()->set_use_cascading_window(
        config.use_cascading_window());
  }
#endif  // TARGET_OS_IPHONE || __linux__ || __wasm__
}

bool Session::IMEOn(commands::Command* command) {
  command->mutable_output()->set_consumed(true);
  ClearUndoContext();

  SetSessionState(ImeContext::PRECOMPOSITION, context_.get());
  if (command->input().has_key() && command->input().key().has_mode()) {
    ApplyCompositionMode(command->input().key().mode(),
                         context_->mutable_composer());
  }
  OutputMode(command);
  return true;
}

bool Session::IMEOnWhenAlreadyOn(commands::Command* command) {
  const composer::Composer& composer = context_->composer();
  // Shift-triggered ASCII mode is temporary.  Restore Japanese input without
  // committing the existing preedit, which may contain both ASCII and kana.
  // An explicitly selected HALF_ASCII mode has no distinct comeback mode.
  if (composer.GetInputMode() == transliteration::HALF_ASCII &&
      composer.GetComebackInputMode() != composer.GetInputMode()) {
    return CompositionModeHiragana(command);
  }
  return DoNothing(command);
}

bool Session::IMEOff(commands::Command* command) {
  ConfirmPendingDirectCommitLearning("ime_off_after_direct_commit_learning");

  command->mutable_output()->set_consumed(true);
  ClearUndoContext();

  Commit(command);

  // Reset the context.
  context_->mutable_converter()->Reset();

  SetSessionState(ImeContext::DIRECT, context_.get());
  OutputMode(command);
  return true;
}

bool Session::MakeSureIMEOn(mozc::commands::Command* command) {
  if (command->input().has_command() &&
      command->input().command().has_composition_mode() &&
      (command->input().command().composition_mode() == commands::DIRECT)) {
    // This is invalid and unsupported usage.
    return false;
  }

  command->mutable_output()->set_consumed(true);
  if (context_->state() == ImeContext::DIRECT) {
    ClearUndoContext();
    SetSessionState(ImeContext::PRECOMPOSITION, context_.get());
  }
  if (command->input().has_command() &&
      command->input().command().has_composition_mode()) {
    ApplyCompositionMode(command->input().command().composition_mode(),
                         context_->mutable_composer());
  }
  OutputMode(command);
  return true;
}

bool Session::MakeSureIMEOff(mozc::commands::Command* command) {
  ConfirmPendingDirectCommitLearning(
      "make_sure_ime_off_after_direct_commit_learning");

  if (command->input().has_command() &&
      command->input().command().has_composition_mode() &&
      (command->input().command().composition_mode() == commands::DIRECT)) {
    // This is invalid and unsupported usage.
    return false;
  }

  command->mutable_output()->set_consumed(true);
  if (context_->state() != ImeContext::DIRECT) {
    ClearUndoContext();
    Commit(command);
    // Reset the context.
    context_->mutable_converter()->Reset();
    SetSessionState(ImeContext::DIRECT, context_.get());
  }
  if (command->input().has_command() &&
      command->input().command().has_composition_mode()) {
    ApplyCompositionMode(command->input().command().composition_mode(),
                         context_->mutable_composer());
  }
  OutputMode(command);
  return true;
}

bool Session::EchoBack(commands::Command* command) {
  command->mutable_output()->set_consumed(false);
  context_->mutable_converter()->Reset();
  OutputKey(command);
  return true;
}

bool Session::EchoBackAndClearUndoContext(commands::Command* command) {
  command->mutable_output()->set_consumed(false);

  // Don't clear undo context when KeyEvent has a modifier key only.
  // TODO(hsumita): A modifier key may be assigned to another functions.
  //                ex) InsertSpace
  //                We need to check it outside of this function.
  const commands::KeyEvent& key_event = command->input().key();

  if (IsPendingDirectCommitLearningDiscardKey(key_event)) {
    DiscardPendingDirectCommitLearning(
        "echo_back_discard_key_after_direct_commit");
  }

  if (!IsPureModifierKeyEvent(key_event)) {
    ClearUndoContext();
  }

  return EchoBack(command);
}

bool Session::DoNothing(commands::Command* command) {
  command->mutable_output()->set_consumed(true);
  // Quick hack for zero query suggestion.
  // Caveats: Resetting converter causes b/8703702 on Windows.
  // Basically we should not *do* something in DoNothing.
  // TODO(komatsu): Fix this.
  if (context_->GetRequest().zero_query_suggestion() &&
      context_->converter().IsActive() &&
      (context_->state() == ImeContext::PRECOMPOSITION)) {
    context_->mutable_converter()->Reset();
    Output(command);
  }
  if (context_->state() & (ImeContext::COMPOSITION | ImeContext::CONVERSION)) {
    Output(command);
  }
  return true;
}

bool Session::Revert(commands::Command* command) {
  DiscardPendingDirectCommitLearning(
      "revert_after_direct_commit_learning");

  ClearPendingRerankedPreeditCommitAfterConvertCancel();

  if (context_->state() == ImeContext::PRECOMPOSITION) {
    context_->mutable_converter()->Revert();
    return EchoBackAndClearUndoContext(command);
  }

  if (!(context_->state() &
        (ImeContext::COMPOSITION | ImeContext::CONVERSION))) {
    return DoNothing(command);
  }

  command->mutable_output()->set_consumed(true);
  ClearUndoContext();

  SetStateToPredompositionAndCancel(context_.get());
  ClearLiveConversionState();
  Output(command);
  return true;
}

bool Session::ResetContext(commands::Command* command) {
  ConfirmPendingDirectCommitLearning(
      "reset_context_after_direct_commit_learning");

  ClearPendingRerankedPreeditCommitAfterConvertCancel();

  if (context_->state() == ImeContext::PRECOMPOSITION) {
    context_->mutable_converter()->Reset();
    return EchoBackAndClearUndoContext(command);
  }

  command->mutable_output()->set_consumed(true);
  ClearUndoContext();

  context_->mutable_converter()->Reset();

  SetStateToPredompositionAndCancel(context_.get());
  ClearLiveConversionState();
  Output(command);
  return true;
}

void Session::SetTable(std::shared_ptr<const composer::Table> table) {
  if (!table) {
    return;
  }
  ClearUndoContext();
  context_->mutable_composer()->SetTable(std::move(table));
}

void Session::SetConfig(std::shared_ptr<const config::Config> config) {
  DCHECK(config);
  ClearUndoContext();
  if (piece_converter_ != nullptr) {
    piece_converter_->SetConfig(config);
  }
  context_->SetConfig(std::move(config));
}

void Session::SetRequest(std::shared_ptr<const commands::Request> request) {
  DCHECK(request);
  ClearUndoContext();
  if (piece_converter_ != nullptr) {
    piece_converter_->SetRequest(request);
  }
  context_->SetRequest(std::move(request));
}

void Session::SetKeyMapManager(
    std::shared_ptr<const mozc::keymap::KeyMapManager> key_map_manager) {
  DCHECK(key_map_manager);
  context_->SetKeyMapManager(key_map_manager);
}

bool Session::GetStatus(commands::Command* command) {
  OutputMode(command);
  return true;
}

bool Session::RequestConvertReverse(commands::Command* command) {
  if (context_->state() != ImeContext::PRECOMPOSITION &&
      context_->state() != ImeContext::DIRECT) {
    return DoNothing(command);
  }
  command->mutable_output()->set_consumed(true);
  Output(command);

  // Fill callback message.
  commands::SessionCommand* session_command =
      command->mutable_output()->mutable_callback()->mutable_session_command();
  session_command->set_type(commands::SessionCommand::CONVERT_REVERSE);
  return true;
}

bool Session::RequestReconvertSelectionOrInsertSpace(
    commands::Command* command) {
  if (context_->state() != ImeContext::PRECOMPOSITION) {
    return DoNothing(command);
  }

  // Build the normal InsertSpace output first.  The TSF client will apply this
  // output only when the application has no selected text.  When selected text
  // exists, the callback is handled before the fallback output is applied.
  if (!InsertSpace(command)) {
    return false;
  }

  commands::SessionCommand* session_command =
      command->mutable_output()->mutable_callback()->mutable_session_command();
  session_command->set_type(
      commands::SessionCommand::RECONVERT_SELECTION_OR_INSERT_SPACE);
  return true;
}

bool Session::ConvertReverse(commands::Command* command) {
  if (context_->state() != ImeContext::PRECOMPOSITION &&
      context_->state() != ImeContext::DIRECT) {
    return DoNothing(command);
  }

  const std::string& composition = command->input().command().text();

  // Validate before requesting reverse conversion
  if (!Util::IsValidUtf8(composition)) {
    DLOG(INFO) << "Input is not valid text as utf8";
    return DoNothing(command);
  }
  for (ConstChar32Iterator iter(composition); !iter.Done(); iter.Next()) {
    if (!Util::IsAcceptableCharacterAsCandidate(iter.Get())) {
      DLOG(INFO)
          << "Input contains characters not suitable for reverse conversion";
      return DoNothing(command);
    }
  }

  std::string reading;
  if (!context_->mutable_converter()->GetReadingText(composition, &reading)) {
    LOG(ERROR) << "Failed to get reading text";
    return DoNothing(command);
  }

  composer::Composer* composer = context_->mutable_composer();
  composer->Reset();
  ClearUndoContext();
  std::vector<std::string> reading_characters;
  composer->InsertCharacterPreedit(reading);
  composer->set_source_text(composition);
  // start conversion here.
  if (!context_->mutable_converter()->Convert(*composer)) {
    LOG(ERROR) << "Failed to start conversion for reverse conversion";
    return false;
  }

  command->mutable_output()->set_consumed(true);

  SetSessionState(ImeContext::CONVERSION, context_.get());
  context_->mutable_converter()->SetCandidateListVisible(true);
  Output(command);
  if (command_sequence_waits_for_reconversion_ &&
      !pending_command_sequence_.empty()) {
    command->mutable_output()->set_command_sequence_resume_generation(
        command_sequence_generation_);
  }
  return true;
}

bool Session::RequestUndo(commands::Command* command) {
  DiscardPendingDirectCommitLearning(
      "undo_after_direct_commit_learning");

  if (!(context_->state() &
        (ImeContext::PRECOMPOSITION | ImeContext::CONVERSION |
         ImeContext::COMPOSITION))) {
    return DoNothing(command);
  }

  // If undo context is empty, echoes back the key event so that it can be
  // handled by the application. b/5553298
  if (context_->state() == ImeContext::PRECOMPOSITION && !HasUndoContext()) {
    return EchoBack(command);
  }

  command->mutable_output()->set_consumed(true);
  Output(command);

  // Fill callback message.
  commands::SessionCommand* session_command =
      command->mutable_output()->mutable_callback()->mutable_session_command();
  session_command->set_type(commands::SessionCommand::UNDO);
  return true;
}

bool Session::Undo(commands::Command* command) {
  DiscardPendingDirectCommitLearning(
      "undo_command_after_direct_commit_learning");

  ClearPendingRerankedPreeditCommitAfterConvertCancel();

  if (!(context_->state() &
        (ImeContext::PRECOMPOSITION | ImeContext::CONVERSION |
         ImeContext::COMPOSITION))) {
    return DoNothing(command);
  }
  command->mutable_output()->set_consumed(true);

  // Check the undo context
  if (!HasUndoContext()) {
    return DoNothing(command);
  }

  // Roll back converter learning only for commit paths that actually used the
  // converter. Pending-display Submit commits a visible string directly and
  // therefore has no converter learning to revert.
  if (ShouldRevertConverterOnUndo()) {
    context_->mutable_converter()->Revert();
  }

  size_t result_size = 0;
  int32_t cursor_offset = 0;
  if (context_->output().has_result()) {
    // Check the client's capability
    if (!(context_->client_capability().text_deletion() &
          commands::Capability::DELETE_PRECEDING_TEXT)) {
      return DoNothing(command);
    }
    result_size = Util::CharsLen(context_->output().result().value());
    cursor_offset = context_->output().result().cursor_offset();
  }

  PopUndoContext();

  if (result_size > 0) {
    commands::DeletionRange* range =
        command->mutable_output()->mutable_deletion_range();
    range->set_offset(-(static_cast<int32_t>(result_size) + cursor_offset));
    range->set_length(result_size);
  }

  if (live_conversion_pending_ &&
      context_->state() == ImeContext::COMPOSITION) {
    if (OutputPendingLiveConversion(command)) {
      AttachCachedLiveConversionSuggestionCandidateWindow(
          command->mutable_output());
      AttachDelayedLiveConversionCallback(command);
      return true;
    }

    // A corrupted or incompatible snapshot must degrade to ordinary
    // composition rather than leave an unserviceable pending state.
    CancelPendingLiveConversion();
  }

  Output(command);
  return true;
}

bool Session::SelectCandidateInternal(commands::Command* command) {
  // If the current state is not conversion, composition or
  // precomposition, the candidate window should not be shown.  (On
  // composition or precomposition, the window is able to be shown as
  // a suggestion window).
  if (!(context_->state() & (ImeContext::CONVERSION | ImeContext::COMPOSITION |
                             ImeContext::PRECOMPOSITION))) {
    return false;
  }
  if (!command->input().has_command() || !command->input().command().has_id()) {
    LOG(WARNING) << "input.command or input.command.id did not exist.";
    return false;
  }
  if (!context_->converter().IsActive()) {
    LOG(WARNING) << "converter is not active. (no candidates)";
    return false;
  }

  command->mutable_output()->set_consumed(true);

  context_->mutable_converter()->CandidateMoveToId(
      command->input().command().id(), context_->composer());
  SetSessionState(ImeContext::CONVERSION, context_.get());

  return true;
}

bool Session::SelectCandidate(commands::Command* command) {
  if (!SelectCandidateInternal(command)) {
    return DoNothing(command);
  }
  Output(command);
  return true;
}

bool Session::CommitCandidate(commands::Command* command) {
  if (!(context_->state() & (ImeContext::COMPOSITION | ImeContext::CONVERSION |
                             ImeContext::PRECOMPOSITION))) {
    return false;
  }
  const commands::Input& input = command->input();
  if (!input.has_command() || !input.command().has_id()) {
    LOG(WARNING) << "input.command or input.command.id did not exist.";
    return false;
  }

  // Live conversion deliberately keeps the real converter in CONVERSION while
  // rendering a passive SUGGESTION list generated from a cloned ImeContext.
  // Therefore an ID from the visible suggestion window must never be resolved
  // against the real converter: the same integer can identify an unrelated
  // normal-conversion candidate.
  const commands::CandidateWindow* passive_suggestion_window = nullptr;
  std::unique_ptr<ImeContext>* passive_suggestion_context_owner = nullptr;

  if (live_conversion_active_ &&
      live_conversion_suggestion_candidate_window_.has_category() &&
      live_conversion_suggestion_candidate_window_.category() ==
          commands::SUGGESTION &&
      live_conversion_suggestion_candidate_window_.candidate_size() > 0 &&
      !live_conversion_suggestion_candidate_window_.has_focused_index()) {
    passive_suggestion_window = &live_conversion_suggestion_candidate_window_;
    passive_suggestion_context_owner =
        &live_conversion_suggestion_context_;
  } else if (
      live_conversion_pending_ &&
      pending_live_conversion_suggestion_candidate_window_.has_category() &&
      pending_live_conversion_suggestion_candidate_window_.category() ==
          commands::SUGGESTION &&
      pending_live_conversion_suggestion_candidate_window_.candidate_size() >
          0 &&
      !pending_live_conversion_suggestion_candidate_window_
           .has_focused_index()) {
    passive_suggestion_window =
        &pending_live_conversion_suggestion_candidate_window_;
    passive_suggestion_context_owner =
        &pending_live_conversion_suggestion_context_;
  }

  const commands::CandidateWindow::Candidate* passive_suggestion = nullptr;
  if (passive_suggestion_window != nullptr) {
    for (const auto& candidate : passive_suggestion_window->candidate()) {
      if (candidate.has_id() && candidate.id() == input.command().id()) {
        passive_suggestion = &candidate;
        break;
      }
    }
  }

  bool submit_passive_live_suggestion = false;
  if (passive_suggestion != nullptr) {
    if (passive_suggestion_context_owner == nullptr ||
        *passive_suggestion_context_owner == nullptr) {
      LOG(ERROR) << "passive suggestion context is missing";
      return DoNothing(command);
    }

    ImeContext& passive_suggestion_context =
        **passive_suggestion_context_owner;

    // Verify that the stored semantic context still owns exactly the candidate
    // the renderer displayed. If it does not, failing closed is safer than
    // aliasing the ID into another converter state.
    commands::Output suggestion_snapshot;
    passive_suggestion_context.converter().FillOutput(
        passive_suggestion_context.composer(), &suggestion_snapshot);

    const commands::CandidateWindow::Candidate* snapshot_candidate = nullptr;
    if (suggestion_snapshot.has_candidate_window() &&
        suggestion_snapshot.candidate_window().category() ==
            commands::SUGGESTION &&
        !suggestion_snapshot.candidate_window().has_focused_index()) {
      for (const auto& candidate :
           suggestion_snapshot.candidate_window().candidate()) {
        if (candidate.has_id() && candidate.id() == input.command().id()) {
          snapshot_candidate = &candidate;
          break;
        }
      }
    }

    if (snapshot_candidate == nullptr ||
        snapshot_candidate->value() != passive_suggestion->value() ||
        passive_suggestion_context.composer().GetQueryForConversion() !=
            context_->composer().GetQueryForConversion()) {
      LOG(ERROR) << "passive suggestion semantic snapshot mismatch";
      return DoNothing(command);
    }

    submit_passive_live_suggestion = true;
  }

  if (!submit_passive_live_suggestion &&
      !context_->converter().IsActive()) {
    LOG(WARNING) << "converter is not active. (no candidates)";
    return false;
  }
  command->mutable_output()->set_consumed(true);

  PushUndoContext();

  if (submit_passive_live_suggestion) {
    // Clicking a passive suggestion is an explicit override of the speculative
    // live-conversion presentation.  Switch to the exact cloned suggestion
    // context that produced the displayed ID, then reuse the ordinary
    // CommitSuggestionById path below.
    std::unique_ptr<ImeContext> suggestion_context =
        std::move(*passive_suggestion_context_owner);
    ClearLiveConversionState();
    context_ = std::move(suggestion_context);
  }

  if (context_->state() & ImeContext::CONVERSION) {
    // There is a focused candidate so just select a candidate based on
    // input message and commit first segment.
    context_->mutable_converter()->CandidateMoveToId(input.command().id(),
                                                     context_->composer());
    CommitHeadToFocusedSegmentsInternal(command->input().context());
  } else {
    // No candidate is focused.
    size_t consumed_key_size = 0;
    if (context_->mutable_converter()->CommitSuggestionById(
            input.command().id(), context_->composer(),
            command->input().context(), &consumed_key_size)) {
      if (consumed_key_size < context_->composer().GetLength()) {
        // partial suggestion was committed.
        context_->mutable_composer()->DeleteRange(0, consumed_key_size);
        // Don't clear the undo context, which we've just updated.
        MoveCursorToEndInternal(command, false);
        // Copy the previous output for Undo.
        *context_->mutable_output() = command->output();
        return true;
      }
    }
  }

  if (!context_->converter().IsActive()) {
    // If the converter is not active (ie. the segment size was one.),
    // the state should be switched to precomposition.
    SetSessionState(ImeContext::PRECOMPOSITION, context_.get());

    // Get suggestion if zero_query_suggestion is set.
    // zero_query_suggestion is usually set where the client is a
    // mobile.
    if (context_->GetRequest().zero_query_suggestion()) {
      Suggest(command->input());
    }
  }
  Output(command);
  // Copy the previous output for Undo.
  *context_->mutable_output() = command->output();
  return true;
}

bool Session::HighlightCandidate(commands::Command* command) {
  if (!SelectCandidateInternal(command)) {
    return false;
  }
  context_->mutable_converter()->SetCandidateListVisible(true);
  Output(command);
  return true;
}

bool Session::MaybeSelectCandidate(commands::Command* command) {
  if (context_->state() != ImeContext::CONVERSION) {
    return false;
  }
  // When using special romaji table (== The key event is from a virtual
  // keyboard), don't consume it as a shortcut selection operation.
  if (context_->GetRequest().special_romanji_table() !=
      commands::Request::DEFAULT_TABLE) {
    return false;
  }

  // Note that SHORTCUT_ASDFGHJKL should be handled even when the CapsLock is
  // enabled. This is why we need to normalize the key event here.
  // See b/5655743.
  commands::KeyEvent normalized_keyevent;
  KeyEventUtil::NormalizeModifiers(command->input().key(),
                                   &normalized_keyevent);

  // Check if the input character is in the shortcut.
  // TODO(komatsu): Support non ASCII characters such as Unicode and
  // special keys.
  const char shortcut = static_cast<char>(normalized_keyevent.key_code());
  return context_->mutable_converter()->CandidateMoveToShortcut(shortcut);
}

void Session::CancelPendingLiveConversion() {
  ++live_conversion_generation_;
  live_conversion_pending_ = false;
  pending_live_conversion_generation_ = 0;
  pending_live_conversion_key_.clear();
  pending_live_conversion_input_.Clear();
  pending_live_conversion_suggestion_candidate_window_.Clear();
  pending_live_conversion_suggestion_context_.reset();
  pending_live_conversion_suggestion_query_.clear();
  pending_live_conversion_holds_passive_suggestion_ = false;
}

void Session::ClearLiveConversionState() {
  ++live_conversion_generation_;

  live_conversion_active_ = false;

  live_conversion_from_composition_ = false;
  live_conversion_pending_ = false;
  pending_live_conversion_generation_ = 0;
  pending_live_conversion_key_.clear();
  pending_live_conversion_input_.Clear();
  pending_live_conversion_suggestion_candidate_window_.Clear();
  pending_live_conversion_suggestion_context_.reset();
  pending_live_conversion_suggestion_query_.clear();
  pending_live_conversion_holds_passive_suggestion_ = false;
  live_conversion_suggestion_candidate_window_.Clear();
  live_conversion_suggestion_context_.reset();
  live_conversion_suggestion_query_.clear();
  live_conversion_holds_passive_suggestion_ = false;

  live_conversion_key_.clear();
  live_conversion_preedit_.clear();
  live_conversion_value_.clear();
  live_conversion_preedit_output_.Clear();
  pending_live_conversion_presentation_.reset();
  // The piece pipeline's caches describe one live-conversion episode: both
  // depend on the candidates and choices proven while this composition was
  // on screen, and a commit or cancel ends that episode.
  meltype_piece_plan_ = PiecePlan();
  piece_options_cache_.clear();
  piece_choice_cache_.clear();
  meltype_piece_choices_.clear();
  meltype_display_protected_ = false;
}

void Session::CancelLiveConversionForEditing() {
  pending_live_conversion_presentation_.reset();
  CancelPendingLiveConversion();

  if (!live_conversion_active_) {
    return;
  }

  live_conversion_active_ = false;

  live_conversion_from_composition_ = false;
  SetSessionState(ImeContext::COMPOSITION, context_.get());
  context_->mutable_converter()->Cancel();
}

namespace {
bool IsPassiveLiveSuggestionCandidateWindow(
    const commands::CandidateWindow& candidate_window) {
  return candidate_window.has_category() &&
         candidate_window.category() == commands::SUGGESTION &&
         candidate_window.candidate_size() > 0 &&
         !candidate_window.has_focused_index();
}

constexpr size_t kMaxPendingRomanLengthForSuggestionRefresh = 2;

bool ShouldHoldSuggestionForPendingRoman(
    const composer::Composer& composer) {
  const size_t pending_length = composer.GetPendingRomanInputLength();
  if (pending_length == 0) {
    return false;
  }

  // Only the first unresolved Roman sequence of a fresh composition is allowed
  // to refresh suggestion, and only through two characters (e.g. "s" -> "sh",
  // "k" -> "ky"). Once a fixed reading exists before the suffix, even the
  // first pending character holds the previously displayed suggestion.
  if (!composer.IsPendingRomanInputAtCompositionStart()) {
    return true;
  }

  return pending_length > kMaxPendingRomanLengthForSuggestionRefresh;
}

bool IsPendingRomanSuggestionHoldKey(const commands::KeyEvent& key) {
  if (!key.has_key_code() || key.has_key_string() ||
      key.input_style() != commands::KeyEvent::FOLLOW_MODE ||
      key.modifier_keys_size() != 0 ||
      (key.has_modifiers() && key.modifiers() != 0)) {
    return false;
  }

  // Only an unmodified physical lower-case Roman key is eligible.  Modifier
  // combinations such as Ctrl+a can preserve a lower-case key_code while also
  // carrying modifier_keys, so they must be excluded explicitly rather than
  // inferred from key_code casing.
  const uint32_t key_code = key.key_code();
  return key_code >= static_cast<uint32_t>('a') &&
         key_code <= static_cast<uint32_t>('z');
}

bool HaveSamePassiveSuggestionCandidates(
    const commands::CandidateWindow& lhs,
    const commands::CandidateWindow& rhs) {
  if (!IsPassiveLiveSuggestionCandidateWindow(lhs) ||
      !IsPassiveLiveSuggestionCandidateWindow(rhs) ||
      lhs.candidate_size() != rhs.candidate_size()) {
    return false;
  }

  for (int i = 0; i < lhs.candidate_size(); ++i) {
    const commands::CandidateWindow::Candidate& lhs_candidate =
        lhs.candidate(i);
    const commands::CandidateWindow::Candidate& rhs_candidate =
        rhs.candidate(i);

    if (lhs_candidate.has_id() != rhs_candidate.has_id() ||
        (lhs_candidate.has_id() &&
         lhs_candidate.id() != rhs_candidate.id()) ||
        lhs_candidate.value() != rhs_candidate.value()) {
      return false;
    }
  }
  return true;
}

bool ShouldSuppressShiftedAsciiAutoSuggestion(
    const config::Config& config,
    const composer::Composer& composer);
}  // namespace

bool Session::MaybeStartLiveConversion(commands::Command* command) {
  return MaybeStartLiveConversionInternal(
      command, /*held_suggestion=*/nullptr);
}

bool Session::MaybeStartLiveConversionAfterEditing(
    commands::Command* command) {
  return MaybeStartLiveConversionInternal(
      command, /*held_suggestion=*/nullptr);
}

bool Session::MaybeStartLiveConversionInternal(
    commands::Command* command, PassiveSuggestionSnapshot* held_suggestion) {
  if (!context_->GetConfig().use_live_conversion()) {
    return false;
  }

  if (context_->state() != ImeContext::COMPOSITION) {
    return false;
  }

  if (context_->composer().GetInputFieldType() == commands::Context::PASSWORD) {
    return false;
  }

  // Same exception as MaybeScheduleLiveConversion: a temporary Shift+letter
  // ASCII mode inside a Japanese + English composition is still composition,
  // not direct ASCII input, so the Meltype protection must keep running.
  const transliteration::TransliterationType input_mode =
      context_->composer().GetInputMode();
  if ((input_mode == transliteration::HALF_ASCII ||
       input_mode == transliteration::FULL_ASCII) &&
      !(IsTemporaryShiftAsciiInputMode() &&
        CompositionMixesJapaneseAndEnglish())) {
    return false;
  }

  const size_t length = context_->composer().GetLength();
  const std::string live_conversion_key =
      context_->composer().GetQueryForConversion();

  if (ShouldSkipLiveConversionForCompositionKey(
          live_conversion_key,
          GetLiveConversionMinKeyLength(context_->GetConfig())) ||
      length != context_->composer().GetCursor()) {
    return false;
  }

  // Capture the raw composition before Convert(). These strings are the stable
  // source for the next pending-suffix display.
  const std::string live_conversion_preedit =
      context_->composer().GetStringForPreedit();

  const config::Config& config = context_->GetConfig();

  // Delayed live-conversion callbacks are SEND_COMMAND inputs and may not carry
  // the same request_suggestion/context data as the original SEND_KEY input.
  // Keep the original input for passive suggestion generation so the suggestion
  // window does not disappear when the delayed live conversion materializes.
  const bool use_pending_live_conversion_input =
      live_conversion_pending_ &&
      pending_live_conversion_key_ == live_conversion_key &&
      pending_live_conversion_input_.type() != commands::Input::NONE;
  const commands::Input live_conversion_suggestion_input =
      use_pending_live_conversion_input ? pending_live_conversion_input_
                                        : command->input();
  commands::CandidateWindow pending_live_conversion_suggestion_candidate_window =
      std::move(pending_live_conversion_suggestion_candidate_window_);
  std::unique_ptr<ImeContext> pending_live_conversion_suggestion_context =
      std::move(pending_live_conversion_suggestion_context_);
  std::string pending_live_conversion_suggestion_query =
      std::move(pending_live_conversion_suggestion_query_);
  const bool should_hold_pending_roman_suggestion =
      held_suggestion != nullptr ||
      (live_conversion_pending_ &&
       pending_live_conversion_holds_passive_suggestion_);

  live_conversion_pending_ = false;
  pending_live_conversion_generation_ = 0;
  pending_live_conversion_key_.clear();
  pending_live_conversion_input_.Clear();
  pending_live_conversion_suggestion_candidate_window_.Clear();
  pending_live_conversion_suggestion_context_.reset();
  pending_live_conversion_suggestion_query_.clear();
  pending_live_conversion_holds_passive_suggestion_ = false;

  if (!context_->mutable_converter()->Convert(context_->composer())) {
    if (ShouldKeepPendingLiveConversionForTransientSokuon(live_conversion_key) &&
        OutputPendingLiveConversion(command)) {
      return true;
    }

    pending_live_conversion_presentation_.reset();
    OutputComposition(command);
    return true;
  }

  TraceStep("Mozc Converter", "ok");

  // Live conversion is a temporary view of the same ongoing composition.
  // SetSessionState(CONVERSION) also calls Composer::ResetInputMode(), which
  // marks the next key as a new input. That separates a pending roman suffix
  // from a following symbol without its own table rule (e.g. n + ')'), even
  // when the custom table contains a combined rule such as n) -> ん）.
  // Preserve the composer input mode and chunk continuity for further typing.
  context_->set_state(ImeContext::CONVERSION);
  live_conversion_active_ = true;
  live_conversion_from_composition_ = true;

  // Keep the candidate list visible internally so that the Windows renderer is
  // updated.
  context_->mutable_converter()->SetCandidateListVisible(true);

  Output(command);

  // The Mozc multi-clause conversion Output() just produced is the baseline;
  // Meltype then protects the English pieces on top of it — their segments
  // are rewritten to the typed letters (the kanji conversion can never mangle
  // them, so no "後おgぇ") — and cached SystemOne choices collapse their
  // pieces' segments.  Everything else stays exactly what Mozc converted, so
  // long Japanese keeps its natural clause boundaries.
  //
  // Meltype segments the raw romaji the user typed, so it must be given
  // GetRawString().  live_conversion_key is the conversion query (kana), and a
  // kana string can never match Meltype's Latin spans.
  if (command->output().has_preedit()) {
    // Answers that landed while this keystroke was running are harvested
    // first, so a proven choice shows at 0ms instead of flickering back to
    // the baseline.
    HarvestPieceSystemOneAnswers();
    const bool meltype_protected = ApplyMeltypePiecePreedit(
        context_->composer().GetRawString(), /*final=*/false,
        command->mutable_output()->mutable_preedit());
    // Symbol style (wave dash, brackets) is restored onto the keys Mozc
    // produced: the protection only rewrites values, so the restoration
    // matches the reading exactly as it did without Meltype.
    RestorePreeditSegmentKeysForSymbolStyle(
        live_conversion_preedit,
        command->mutable_output()->mutable_preedit());

    std::string trace_preedit_key;
    std::string trace_preedit_value;
    ExtractPreeditKeyAndValue(command->output().preedit(), &trace_preedit_key,
                              &trace_preedit_value);
    TraceStep("Meltype",
              absl::StrCat(meltype_protected ? "protected" : "baseline",
                           " preedit=\"", TraceText(trace_preedit_value),
                           "\""));
  }

  command->mutable_output()->set_live_conversion(true);
  command->mutable_output()->set_live_conversion_pending(false);

  live_conversion_key_ = live_conversion_key;
  live_conversion_preedit_ = live_conversion_preedit;

  if (command->output().has_preedit()) {
    live_conversion_preedit_output_ = command->output().preedit();

    std::string unused_key;
    ExtractPreeditKeyAndValue(command->output().preedit(),
                              &unused_key,
                              &live_conversion_value_);
  } else {
    live_conversion_preedit_output_.Clear();
    live_conversion_value_ = context_->composer().GetStringForSubmission();
  }

  pending_live_conversion_presentation_.reset();

  // Passive suggestion belongs to the current composer query and must be
  // materialized before a later stage rewrites the preedit.
  const bool should_suppress_shifted_ascii_suggestion =
      ShouldSuppressShiftedAsciiAutoSuggestion(context_->GetConfig(),
                                               context_->composer());

  live_conversion_holds_passive_suggestion_ = false;

  if (should_suppress_shifted_ascii_suggestion) {
    live_conversion_suggestion_candidate_window_.Clear();
    pending_live_conversion_suggestion_candidate_window_.Clear();
    live_conversion_suggestion_context_.reset();
    pending_live_conversion_suggestion_context_.reset();
    live_conversion_suggestion_query_.clear();
    pending_live_conversion_suggestion_query_.clear();
    command->mutable_output()->clear_candidate_window();
  } else if (should_hold_pending_roman_suggestion) {
    PassiveSuggestionSnapshot fallback_snapshot;
    PassiveSuggestionSnapshot* snapshot = held_suggestion;

    if ((snapshot == nullptr || snapshot->context == nullptr) &&
        IsPassiveLiveSuggestionCandidateWindow(
            pending_live_conversion_suggestion_candidate_window) &&
        pending_live_conversion_suggestion_context != nullptr) {
      fallback_snapshot.candidate_window =
          std::move(pending_live_conversion_suggestion_candidate_window);
      fallback_snapshot.context =
          std::move(pending_live_conversion_suggestion_context);
      fallback_snapshot.suggestion_query =
          std::move(pending_live_conversion_suggestion_query);
      snapshot = &fallback_snapshot;
    }

    if ((snapshot == nullptr || snapshot->context == nullptr) &&
        IsPassiveLiveSuggestionCandidateWindow(
            live_conversion_suggestion_candidate_window_) &&
        live_conversion_suggestion_context_ != nullptr) {
      fallback_snapshot.candidate_window =
          std::move(live_conversion_suggestion_candidate_window_);
      fallback_snapshot.context =
          std::move(live_conversion_suggestion_context_);
      fallback_snapshot.suggestion_query =
          std::move(live_conversion_suggestion_query_);
      snapshot = &fallback_snapshot;
    }

    live_conversion_suggestion_candidate_window_.Clear();
    live_conversion_suggestion_context_.reset();
    live_conversion_suggestion_query_.clear();

    if (snapshot != nullptr &&
        AttachPassiveSuggestionSnapshot(
            live_conversion_suggestion_input, snapshot,
            command->mutable_output())) {
      live_conversion_suggestion_candidate_window_ =
          snapshot->candidate_window;
      live_conversion_suggestion_context_ =
          std::move(snapshot->context);
      live_conversion_suggestion_query_ = snapshot->suggestion_query;
      live_conversion_holds_passive_suggestion_ = true;
    } else {
      command->mutable_output()->clear_candidate_window();
    }
  } else if (
      !AttachLiveConversionSuggestionCandidateWindow(
          live_conversion_suggestion_input, command->mutable_output()) &&
      pending_live_conversion_suggestion_candidate_window.candidate_size() > 0) {
    // A delayed-live-conversion suggestion may only be reused with the exact
    // cloned context that produced its candidate IDs.
    live_conversion_suggestion_candidate_window_ =
        std::move(pending_live_conversion_suggestion_candidate_window);
    live_conversion_suggestion_context_ =
        std::move(pending_live_conversion_suggestion_context);
    live_conversion_suggestion_query_ =
        std::move(pending_live_conversion_suggestion_query);
    AttachCachedLiveConversionSuggestionCandidateWindow(
        command->mutable_output());
  }

  // SystemOne (Jev / Laya) decides which surface each Japanese piece shows.
  // The preedit already carries the 0ms baseline (first dictionary
  // candidate); this point only submits the questions the choice cache cannot
  // answer, each with the piece's left context, reading and candidate list.
  // The answers arrive later through an APPLY_SYSTEMONE_LIVE delayed
  // callback — the key-event path never blocks on the network, and with the
  // feature off nothing is ever asked.
  MaybeSubmitPieceSystemOne(config, command);

  return true;
}

bool Session::OutputPendingLiveConversion(commands::Command* command) const {
  const PendingLiveConversionPresentation* presentation =
      pending_live_conversion_presentation_.has_value()
          ? &*pending_live_conversion_presentation_
          : nullptr;
  return OutputPendingLiveConversionWithPresentation(
      command, presentation, nullptr);
}

bool Session::OutputPendingLiveConversionWithPresentation(
    commands::Command* command,
    const PendingLiveConversionPresentation* presentation,
    PendingLiveConversionPresentation* stable_presentation) const {
  if (stable_presentation != nullptr) {
    *stable_presentation = PendingLiveConversionPresentation();
  }

  const std::string current_key = context_->composer().GetQueryForConversion();
  const std::string raw_preedit = context_->composer().GetStringForPreedit();

  const std::string* stable_key = &live_conversion_key_;
  const std::string* stable_raw_preedit = &live_conversion_preedit_;
  const std::string* stable_value = &live_conversion_value_;
  const commands::Preedit* stable_preedit_output =
      &live_conversion_preedit_output_;
  const std::vector<StablePresentationSpan>* stable_spans = nullptr;
  bool selected_visible_presentation = false;

  // A captured presentation may later cease to be a textual prefix when the
  // trailing romaji chunk is rewritten in place.  Do
  // not ask the *new* Mozc segmentation to rediscover the old display
  // boundary.  First reuse the leading key/value spans that were already
  // proven by the Mozc result that existed when the visible presentation was
  // captured.  Each span is indivisible: a character-level common prefix is
  // never treated as a reading/value correspondence.
  std::optional<PendingLiveConversionPresentation>
      safe_common_prefix_presentation;

  if (presentation != nullptr) {
    if (StartsWithString(current_key, presentation->key) &&
        StartsWithString(raw_preedit, presentation->raw_preedit)) {
      stable_key = &presentation->key;
      stable_raw_preedit = &presentation->raw_preedit;
      stable_value = &presentation->value;
      stable_preedit_output = &presentation->preedit_output;
      stable_spans = &presentation->stable_spans;
      selected_visible_presentation = true;
    } else if (!presentation->stable_spans.empty()) {
      PendingLiveConversionPresentation prefix;
      std::string prefix_key;
      std::string prefix_raw_preedit;
      std::string prefix_value;

      for (const StablePresentationSpan& span : presentation->stable_spans) {
        const std::string next_key = absl::StrCat(prefix_key, span.key);
        const std::string next_raw_preedit =
            absl::StrCat(prefix_raw_preedit, span.raw_preedit);
        const std::string next_value = absl::StrCat(prefix_value, span.value);

        if (!StartsWithString(presentation->key, next_key) ||
            !StartsWithString(presentation->raw_preedit, next_raw_preedit) ||
            !StartsWithString(presentation->value, next_value) ||
            !StartsWithString(current_key, next_key) ||
            !StartsWithString(raw_preedit, next_raw_preedit)) {
          break;
        }

        prefix_key = next_key;
        prefix_raw_preedit = next_raw_preedit;
        prefix_value = next_value;
        prefix.key = prefix_key;
        prefix.raw_preedit = prefix_raw_preedit;
        prefix.value = prefix_value;
        *prefix.preedit_output.add_segment() = span.preedit_segment;
        prefix.stable_spans.push_back(span);
      }

      if (!prefix.key.empty() && !prefix.raw_preedit.empty() &&
          !prefix.value.empty() && prefix.preedit_output.segment_size() > 0) {
        prefix.preedit_output.set_cursor(Util::CharsLen(prefix.value));
        safe_common_prefix_presentation = std::move(prefix);

        stable_key = &safe_common_prefix_presentation->key;
        stable_raw_preedit =
            &safe_common_prefix_presentation->raw_preedit;
        stable_value = &safe_common_prefix_presentation->value;
        stable_preedit_output =
            &safe_common_prefix_presentation->preedit_output;
        stable_spans = &safe_common_prefix_presentation->stable_spans;
        selected_visible_presentation = true;
      }
    }

    // Older presentations do not necessarily have stable spans yet.  Keep the
    // post-Convert outer-segment salvage as a conservative fallback, but only
    // when no previously proven span survived.  This fallback never splits a
    // converter segment and therefore cannot invent a key/value boundary.
    if (!selected_visible_presentation && !presentation->value.empty() &&
        !live_conversion_key_.empty() && !live_conversion_preedit_.empty() &&
        live_conversion_preedit_output_.segment_size() > 0) {
      PendingLiveConversionPresentation prefix;
      std::string prefix_value;
      std::string prefix_raw_preedit;
      size_t prefix_key_chars = 0;

      const size_t live_key_chars = Util::CharsLen(live_conversion_key_);
      for (int i = 0; i < live_conversion_preedit_output_.segment_size(); ++i) {
        const commands::Preedit::Segment& segment =
            live_conversion_preedit_output_.segment(i);
        if (!segment.has_key() || segment.key().empty() ||
            segment.value().empty()) {
          break;
        }

        const std::string next_value =
            absl::StrCat(prefix_value, segment.value());
        if (!StartsWithString(presentation->value, next_value)) {
          break;
        }

        const std::string next_raw_preedit =
            absl::StrCat(prefix_raw_preedit, segment.key());
        const size_t segment_key_chars = Util::CharsLen(segment.key());
        const size_t next_key_chars = prefix_key_chars + segment_key_chars;
        if (next_key_chars > live_key_chars) {
          break;
        }

        const std::string next_key = std::string(
            Util::Utf8SubString(live_conversion_key_, 0, next_key_chars));

        if (!StartsWithString(live_conversion_preedit_, next_raw_preedit) ||
            !StartsWithString(presentation->key, next_key) ||
            !StartsWithString(presentation->raw_preedit, next_raw_preedit) ||
            !StartsWithString(current_key, next_key) ||
            !StartsWithString(raw_preedit, next_raw_preedit)) {
          break;
        }

        StablePresentationSpan span;
        span.key = std::string(Util::Utf8SubString(
            live_conversion_key_, prefix_key_chars, segment_key_chars));
        span.raw_preedit = segment.key();
        span.value = segment.value();
        span.preedit_segment = segment;

        prefix.key = next_key;
        prefix.raw_preedit = next_raw_preedit;
        prefix.value = next_value;
        *prefix.preedit_output.add_segment() = segment;
        prefix.stable_spans.push_back(std::move(span));

        prefix_value = next_value;
        prefix_raw_preedit = next_raw_preedit;
        prefix_key_chars = next_key_chars;
      }

      if (!prefix.key.empty() && !prefix.raw_preedit.empty() &&
          !prefix.value.empty() && prefix.preedit_output.segment_size() > 0) {
        prefix.preedit_output.set_cursor(Util::CharsLen(prefix.value));
        safe_common_prefix_presentation = std::move(prefix);

        stable_key = &safe_common_prefix_presentation->key;
        stable_raw_preedit =
            &safe_common_prefix_presentation->raw_preedit;
        stable_value = &safe_common_prefix_presentation->value;
        stable_preedit_output =
            &safe_common_prefix_presentation->preedit_output;
        stable_spans = &safe_common_prefix_presentation->stable_spans;
        selected_visible_presentation = true;
      }
    }
  }

  const bool has_stable_live_conversion =
      !stable_key->empty() && !stable_raw_preedit->empty() &&
      !stable_value->empty() && stable_preedit_output->segment_size() > 0;

  // First composition after starting IME has no stable converted prefix yet.
  // In that case, raw pending display is expected and should still be debounced.
  if (!has_stable_live_conversion) {
    OutputComposition(command);

    if (command->output().has_preedit()) {
      RestorePreeditSegmentKeysForSymbolStyle(
          raw_preedit, command->mutable_output()->mutable_preedit());
    }

    commands::Output* output = command->mutable_output();
    output->clear_candidate_window();
    output->set_live_conversion(true);
    output->set_live_conversion_pending(true);
    return true;
  }

  // If stable-prefix composition cannot be built safely, do not fall back to
  // raw hiragana. The caller should immediately run live conversion instead.
  if (!StartsWithString(current_key, *stable_key) ||
      !StartsWithString(raw_preedit, *stable_raw_preedit)) {
    return false;
  }

  const std::string suffix_key = current_key.substr(stable_key->size());
  const std::string suffix_value =
      raw_preedit.substr(stable_raw_preedit->size());

  OutputComposition(command);

  commands::Output* output = command->mutable_output();
  output->clear_candidate_window();
  output->set_live_conversion(true);
  output->set_live_conversion_pending(true);

  commands::Preedit* preedit = output->mutable_preedit();
  preedit->Clear();

  // Reuse the exact segment structure and annotations from the presentation
  // that proved this prefix. This avoids flickering between UNDERLINE and
  // HIGHLIGHT display attributes.
  for (int i = 0; i < stable_preedit_output->segment_size(); ++i) {
    *preedit->add_segment() = stable_preedit_output->segment(i);
  }

  if (!suffix_value.empty()) {
    AddPreeditSegmentWithPendingRomanTail(
        suffix_key.empty() ? suffix_value : suffix_key, suffix_value,
        context_->composer().GetPendingRomanDisplayLength(), preedit);
  }

  RestorePreeditSegmentKeysForSymbolStyle(raw_preedit, preedit);

  preedit->set_cursor(Util::CharsLen(*stable_value) +
                      Util::CharsLen(suffix_value));

  if (stable_presentation != nullptr) {
    stable_presentation->key = *stable_key;
    stable_presentation->raw_preedit = *stable_raw_preedit;
    stable_presentation->value = *stable_value;
    stable_presentation->preedit_output = *stable_preedit_output;
    if (stable_spans != nullptr) {
      stable_presentation->stable_spans = *stable_spans;
    } else {
      stable_presentation->stable_spans =
          BuildStablePresentationSpans(*stable_value);
    }
  }

  return true;
}

std::vector<Session::StablePresentationSpan>
Session::BuildStablePresentationSpans(absl::string_view visible_value) const {
  return MergeStablePresentationSpans(visible_value, {});
}

std::vector<Session::StablePresentationSpan>
Session::MergeStablePresentationSpans(
    absl::string_view visible_value,
    const std::vector<StablePresentationSpan>& previous_spans) const {
  std::vector<StablePresentationSpan> spans;
  if (visible_value.empty() || live_conversion_key_.empty() ||
      live_conversion_preedit_.empty() ||
      live_conversion_preedit_output_.segment_size() == 0) {
    return spans;
  }

  std::string prefix_key;
  std::string prefix_raw_preedit;
  std::string prefix_value;
  size_t prefix_key_chars = 0;

  // Keep the longest leading sequence of previously proven spans that still
  // matches both the current reading and the presented value.  The
  // spans are indivisible: if a span changes anywhere, discard that span and
  // everything after it instead of taking a character-level common prefix.
  for (const StablePresentationSpan& span : previous_spans) {
    if (span.key.empty() || span.raw_preedit.empty() || span.value.empty() ||
        !span.preedit_segment.has_key() ||
        span.preedit_segment.key() != span.raw_preedit ||
        span.preedit_segment.value() != span.value) {
      break;
    }

    const std::string next_key = absl::StrCat(prefix_key, span.key);
    const std::string next_raw_preedit =
        absl::StrCat(prefix_raw_preedit, span.raw_preedit);
    const std::string next_value = absl::StrCat(prefix_value, span.value);
    if (!StartsWithString(live_conversion_key_, next_key) ||
        !StartsWithString(live_conversion_preedit_, next_raw_preedit) ||
        !StartsWithString(visible_value, next_value)) {
      break;
    }

    spans.push_back(span);
    prefix_key = next_key;
    prefix_raw_preedit = next_raw_preedit;
    prefix_value = next_value;
    prefix_key_chars += Util::CharsLen(span.key);
  }

  // The latest Mozc result may be more coarsely segmented than the retained
  // spans.  It must never coarsen or replace a still-valid old span.  New Mozc
  // segments are therefore used only to extend the proven prefix when one of
  // their boundaries lands exactly at the end of the retained prefix.
  int extension_begin = 0;
  if (!spans.empty()) {
    std::string mozc_raw_preedit;
    size_t mozc_key_chars = 0;
    bool found_exact_boundary = false;

    for (int i = 0; i < live_conversion_preedit_output_.segment_size(); ++i) {
      const commands::Preedit::Segment& segment =
          live_conversion_preedit_output_.segment(i);
      if (!segment.has_key() || segment.key().empty()) {
        return spans;
      }

      mozc_raw_preedit.append(segment.key());
      mozc_key_chars += Util::CharsLen(segment.key());
      if (mozc_key_chars > prefix_key_chars ||
          mozc_raw_preedit.size() > prefix_raw_preedit.size()) {
        return spans;
      }

      if (mozc_key_chars == prefix_key_chars &&
          mozc_raw_preedit == prefix_raw_preedit) {
        const std::string mozc_key = std::string(
            Util::Utf8SubString(live_conversion_key_, 0, mozc_key_chars));
        if (mozc_key != prefix_key) {
          return spans;
        }
        extension_begin = i + 1;
        found_exact_boundary = true;
        break;
      }
    }

    if (!found_exact_boundary) {
      return spans;
    }
  }

  const size_t live_key_chars = Util::CharsLen(live_conversion_key_);
  for (int i = extension_begin;
       i < live_conversion_preedit_output_.segment_size(); ++i) {
    const commands::Preedit::Segment& segment =
        live_conversion_preedit_output_.segment(i);
    if (!segment.has_key() || segment.key().empty() ||
        segment.value().empty()) {
      break;
    }

    const size_t segment_key_chars = Util::CharsLen(segment.key());
    const size_t next_key_chars = prefix_key_chars + segment_key_chars;
    const std::string next_raw_preedit =
        absl::StrCat(prefix_raw_preedit, segment.key());
    const std::string next_value =
        absl::StrCat(prefix_value, segment.value());
    if (next_key_chars > live_key_chars ||
        !StartsWithString(live_conversion_preedit_, next_raw_preedit) ||
        !StartsWithString(visible_value, next_value)) {
      break;
    }

    const std::string next_key = std::string(
        Util::Utf8SubString(live_conversion_key_, 0, next_key_chars));
    if (!StartsWithString(live_conversion_key_, next_key)) {
      break;
    }

    StablePresentationSpan span;
    span.key = std::string(Util::Utf8SubString(
        live_conversion_key_, prefix_key_chars, segment_key_chars));
    span.raw_preedit = segment.key();
    span.value = segment.value();
    span.preedit_segment = segment;
    spans.push_back(std::move(span));

    prefix_key = next_key;
    prefix_raw_preedit = next_raw_preedit;
    prefix_value = next_value;
    prefix_key_chars = next_key_chars;
  }

  return spans;
}

std::optional<Session::PendingLiveConversionPresentation>
Session::CapturePendingLiveConversionPresentationForContinuedInput() const {
  PendingLiveConversionPresentation presentation;

  // GetQueryForPrediction() trims trailing alphabet characters.  If it differs
  // from the conversion query, the tail is still a transient romaji fragment
  // such as the "s" in "さすがでs".  Do not promote that transient display to
  // a new stable basis: the next key may rewrite it in place ("s" + "u" ->
  // "す").
  const std::string current_key = context_->composer().GetQueryForConversion();
  const std::string current_raw_preedit =
      context_->composer().GetStringForPreedit();
  const bool has_transient_trailing_alphabet =
      context_->composer().GetQueryForPrediction() != current_key;

  if (has_transient_trailing_alphabet &&
      pending_live_conversion_presentation_.has_value()) {
    const PendingLiveConversionPresentation& stable =
        *pending_live_conversion_presentation_;
    if (StartsWithString(current_key, stable.key) &&
        StartsWithString(current_raw_preedit, stable.raw_preedit)) {
      return stable;
    }
  }

  if (pending_live_conversion_presentation_.has_value()) {
    return pending_live_conversion_presentation_;
  }

  return std::nullopt;
}

void Session::AttachDelayedLiveConversionCallback(
    commands::Command* command) const {
  commands::Output::Callback* callback =
      command->mutable_output()->mutable_callback();

  commands::SessionCommand* session_command =
      callback->mutable_session_command();

  session_command->set_type(
      commands::SessionCommand::APPLY_LIVE_CONVERSION);
  session_command->set_live_conversion_generation(
      pending_live_conversion_generation_);
  session_command->set_live_conversion_key(pending_live_conversion_key_);

  callback->set_delay_millisec(
    GetLiveConversionDelayMillisec(context_->GetConfig()));
}

bool Session::MaybeScheduleLiveConversion(
    commands::Command* command,
    PassiveSuggestionSnapshot* held_suggestion) {
  if (!context_->GetConfig().use_live_conversion()) {
    return false;
  }

  if (context_->state() != ImeContext::COMPOSITION) {
    return false;
  }

  if (context_->composer().GetInputFieldType() == commands::Context::PASSWORD) {
    return false;
  }

  // Alphanumeric composition mode means the user switched to direct ASCII
  // input, where live conversion has nothing to convert.  The exception is
  // Mozc's own temporary Shift+letter mode inside a Japanese + English
  // composition: typing "G" in "anoGithub" puts the composer in HALF_ASCII
  // for that one letter even though the user is composing Japanese, and
  // skipping live conversion there would drop the Meltype protection (and the
  // Japanese prefix's conversion) for the rest of the composition.
  const transliteration::TransliterationType input_mode =
      context_->composer().GetInputMode();
  if ((input_mode == transliteration::HALF_ASCII ||
       input_mode == transliteration::FULL_ASCII) &&
      !(IsTemporaryShiftAsciiInputMode() &&
        CompositionMixesJapaneseAndEnglish())) {
    return false;
  }

  const size_t length = context_->composer().GetLength();
  const std::string key = context_->composer().GetQueryForConversion();

  const bool should_skip_live_conversion =
      ShouldSkipLiveConversionForCompositionKey(
          key,
          GetLiveConversionMinKeyLength(context_->GetConfig()));
  const bool cursor_at_end = length == context_->composer().GetCursor();

  if (should_skip_live_conversion || !cursor_at_end) {

    CancelPendingLiveConversion();
    return false;
  }

  const uint32_t delay_msec =
      GetLiveConversionDelayMillisec(context_->GetConfig());
  if (delay_msec == 0) {
    return MaybeStartLiveConversionInternal(command, held_suggestion);
  }

  ++live_conversion_generation_;
  live_conversion_pending_ = true;
  pending_live_conversion_generation_ = live_conversion_generation_;
  pending_live_conversion_key_ = key;
  pending_live_conversion_input_ = command->input();
  pending_live_conversion_trace_seq_ = trace_seq_;
  pending_live_conversion_trace_started_at_ = trace_sequence_start_;
  pending_live_conversion_suggestion_candidate_window_.Clear();
  pending_live_conversion_suggestion_context_.reset();
  pending_live_conversion_suggestion_query_.clear();
  pending_live_conversion_holds_passive_suggestion_ = false;

  if (!OutputPendingLiveConversion(command)) {
    // Avoid showing raw hiragana fallback. If pending display cannot be built
    // from the stable converted prefix, materialize live conversion immediately.
    return MaybeStartLiveConversion(command);
  }

  if (held_suggestion != nullptr) {
    if (AttachPassiveSuggestionSnapshot(
            command->input(), held_suggestion, command->mutable_output())) {
      pending_live_conversion_suggestion_candidate_window_ =
          held_suggestion->candidate_window;
      pending_live_conversion_suggestion_context_ =
          std::move(held_suggestion->context);
      pending_live_conversion_suggestion_query_ =
          held_suggestion->suggestion_query;
      pending_live_conversion_holds_passive_suggestion_ = true;
      live_conversion_suggestion_candidate_window_.Clear();
      live_conversion_suggestion_context_.reset();
      live_conversion_suggestion_query_.clear();
    } else {
      command->mutable_output()->clear_candidate_window();
    }
  } else if (
      AttachLiveConversionSuggestionCandidateWindow(
          command->input(), command->mutable_output())) {
    pending_live_conversion_suggestion_candidate_window_ =
        command->output().candidate_window();
    pending_live_conversion_suggestion_context_ =
        std::move(live_conversion_suggestion_context_);
    pending_live_conversion_suggestion_query_ =
        std::move(live_conversion_suggestion_query_);
    live_conversion_suggestion_candidate_window_.Clear();
  }
  AttachDelayedLiveConversionCallback(command);

  return true;
}

bool Session::IgnoreStaleDelayedLiveConversion(commands::Command* command) {
  command->mutable_output()->set_consumed(true);

  // A stale delayed callback must not return an empty Output. In TSF, an empty
  // consumed Output may clear the visible composition even though the server-side
  // composer still has text.
  if (live_conversion_pending_) {
    OutputPendingLiveConversion(command);
    AttachCachedLiveConversionSuggestionCandidateWindow(
        command->mutable_output());
    return true;
  }

  if (live_conversion_active_ && context_->state() == ImeContext::CONVERSION) {
    Output(command);

    if (command->output().has_preedit()) {
      RestorePreeditSegmentKeysForSymbolStyle(
          live_conversion_preedit_.empty()
              ? live_conversion_key_
              : live_conversion_preedit_,
          command->mutable_output()->mutable_preedit());
    }

    command->mutable_output()->set_live_conversion(true);
    command->mutable_output()->set_live_conversion_pending(false);
    AttachCachedLiveConversionSuggestionCandidateWindow(
        command->mutable_output());
    return true;
  }

  OutputFromState(command);
  return true;
}

bool Session::ApplyDelayedLiveConversion(commands::Command* command) {
  // A matching generation means no newer keystroke replaced this pipeline, so
  // the delayed block continues the originating keystroke's trace.
  const bool trace_this =
      TraceLogger::Enabled() && trace_seq_ == 0 && live_conversion_pending_ &&
      command->input().has_command() &&
      command->input().command().has_live_conversion_generation() &&
      command->input().command().live_conversion_generation() ==
          pending_live_conversion_generation_;
  if (trace_this) {
    TraceContinue(pending_live_conversion_trace_seq_,
                  pending_live_conversion_trace_started_at_);
  }

  const bool result = ApplyDelayedLiveConversionInternal(command);

  if (trace_this) {
    TraceStep("Display Output", TraceOutputSummary(command->output()));
    TraceFlush("Command: APPLY_LIVE_CONVERSION");
  }
  return result;
}

bool Session::ApplyDelayedLiveConversionInternal(commands::Command* command) {
  command->mutable_output()->set_consumed(true);

  const commands::SessionCommand& session_command =
      command->input().command();

  if (!live_conversion_pending_) {
    return IgnoreStaleDelayedLiveConversion(command);
  }

  if (!session_command.has_live_conversion_generation() ||
      session_command.live_conversion_generation() !=
          pending_live_conversion_generation_) {
    return IgnoreStaleDelayedLiveConversion(command);
  }

  if (!session_command.has_live_conversion_key() ||
      session_command.live_conversion_key() != pending_live_conversion_key_) {
    return IgnoreStaleDelayedLiveConversion(command);
  }

  if (context_->state() != ImeContext::COMPOSITION) {
    return IgnoreStaleDelayedLiveConversion(command);
  }

  const std::string current_key = context_->composer().GetQueryForConversion();
  if (current_key != pending_live_conversion_key_) {
    return IgnoreStaleDelayedLiveConversion(command);
  }

  const size_t length = context_->composer().GetLength();
  const bool should_skip_live_conversion =
      ShouldSkipLiveConversionForCompositionKey(
          current_key,
          GetLiveConversionMinKeyLength(context_->GetConfig()));
  const bool cursor_at_end = length == context_->composer().GetCursor();

  if (should_skip_live_conversion || !cursor_at_end) {

    CancelPendingLiveConversion();
    OutputFromState(command);
    return true;
  }

  return MaybeStartLiveConversion(command);
}














bool Session::SetPendingDirectCommitLearning(
    absl::string_view key,
    absl::string_view value,
    absl::string_view reason) {
  if (key.empty() || value.empty()) {
    return false;
  }

  if (context_->client_context().is_private_input()) {
    return false;
  }

  if (context_->composer().GetInputFieldType() ==
      commands::Context::PASSWORD) {
    return false;
  }

  pending_direct_commit_learning_.pending = true;
  pending_direct_commit_learning_.key = std::string(key);
  pending_direct_commit_learning_.value = std::string(value);
  pending_direct_commit_learning_.reason = std::string(reason);

  // Keep a snapshot of the converter state immediately after the normal
  // conversion commit.  The current converter will be reset later by
  // CommitStringDirectly(), so delayed cancellation must use this snapshot to
  // revert the original rich Mozc learning.
  pending_direct_commit_learning_.revert_context =
      std::make_unique<ImeContext>(*context_);

  SessionDebugOutput(absl::StrCat(
      "[direct-commit-learning] pending delayed revert reason=", reason,
      " ", RedactedTextStats("key",
                                 pending_direct_commit_learning_.key),
      " ", RedactedTextStats("value",
                                 pending_direct_commit_learning_.value)));

  return true;
}

bool Session::SetPendingDirectCommitLearningFromCommittedResult(
    const commands::Command& command,
    absl::string_view reason) {
  if (!command.output().has_result()) {
    return false;
  }

  const commands::Result& result = command.output().result();
  return SetPendingDirectCommitLearning(result.key(), result.value(), reason);
}

void Session::ConfirmPendingDirectCommitLearning(absl::string_view reason) {
  if (!pending_direct_commit_learning_.pending) {
    return;
  }

  SessionDebugOutput(absl::StrCat(
      "[direct-commit-learning] confirm reason=", reason,
      " original_reason=", pending_direct_commit_learning_.reason,
      " ", RedactedTextStats("key",
                                 pending_direct_commit_learning_.key),
      " ", RedactedTextStats("value",
                                 pending_direct_commit_learning_.value)));

  pending_direct_commit_learning_ = PendingDirectCommitLearning();
}

void Session::DiscardPendingDirectCommitLearning(absl::string_view reason) {
  if (!pending_direct_commit_learning_.pending) {
    return;
  }

  if (pending_direct_commit_learning_.revert_context != nullptr) {
    pending_direct_commit_learning_.revert_context
        ->mutable_converter()
        ->Revert();
  }

  SessionDebugOutput(absl::StrCat(
      "[direct-commit-learning] discard and revert reason=", reason,
      " original_reason=", pending_direct_commit_learning_.reason,
      " ", RedactedTextStats("key",
                                 pending_direct_commit_learning_.key),
      " ", RedactedTextStats("value",
                                 pending_direct_commit_learning_.value)));

  pending_direct_commit_learning_ = PendingDirectCommitLearning();
}

void Session::HandlePendingDirectCommitLearningForKeyEvent(
    const commands::KeyEvent& key) {
  if (!pending_direct_commit_learning_.pending) {
    return;
  }

  if (IsPendingDirectCommitLearningDiscardKey(key)) {
    DiscardPendingDirectCommitLearning("discard_key_after_direct_commit");
    return;
  }

  if (IsPureModifierKeyEvent(key)) {
    return;
  }

  ConfirmPendingDirectCommitLearning("next_real_key_after_direct_commit");
}

void Session::HandlePendingDirectCommitLearningForSessionCommand(
    commands::SessionCommand::CommandType type) {
  if (!pending_direct_commit_learning_.pending) {
    return;
  }

  switch (type) {
    case commands::SessionCommand::REVERT:
    case commands::SessionCommand::UNDO:
      DiscardPendingDirectCommitLearning(
          "session_command_discard_after_direct_commit");
      break;
    case commands::SessionCommand::RESET_CONTEXT:
      // Context loss commonly follows a committed search/form submission. It
      // closes the rollback window; it is not evidence that the commit was
      // undone.
      ConfirmPendingDirectCommitLearning(
          "session_command_reset_context_after_direct_commit");
      break;
    default:
      break;
  }
}














void Session::AttachPendingAsyncCallback(commands::Command* command) const {
  // Decide the pending state before touching Output at all: SessionCommand::type
  // is a proto2 required field, so materializing Output::Callback when nothing
  // is pending would leave type unset, make output().IsInitialized() false,
  // and the client would reject the whole message as SERVER_BROKEN_MESSAGE.
  if (!pending_piece_systemone_.pending) {
    return;
  }

  commands::Output::Callback* callback =
      command->mutable_output()->mutable_callback();
  commands::SessionCommand* session_command =
      callback->mutable_session_command();

  session_command->set_type(
      commands::SessionCommand::APPLY_SYSTEMONE_LIVE);
  session_command->set_live_conversion_generation(
      pending_piece_systemone_.generation);
  // The batch's staleness key is the raw romaji it was built from: the
  // callback fires for the composition that asked, not for a reading that
  // may since have been re-segmented.
  session_command->set_live_conversion_key(pending_piece_systemone_.raw);
  callback->set_delay_millisec(kDefaultSystemOnePollMsec);
}











bool Session::CommitLiveConversionResult(commands::Command* command) {
  if (context_->state() != ImeContext::COMPOSITION) {
    return false;
  }

  if (live_conversion_value_.empty()) {
    CommitCompositionDirectly(command);
    return true;
  }

  const size_t length = context_->composer().GetLength();
  if (length == 0) {
    CommitCompositionDirectly(command);
    return true;
  }

  const std::string preedit = context_->composer().GetStringForPreedit();
  const std::string last_char(
      Util::Utf8SubString(preedit, length - 1, 1));

  std::string key = context_->composer().GetQueryForConversion();
  std::string value = live_conversion_value_;
  value.append(last_char);

  ClearLiveConversionState();

  CommitStringDirectly(key, value, command);
  return true;
}

bool Session::CommitMeltypePieceLiveConversion(commands::Command* command) {
  if (!live_conversion_active_ ||
      context_->state() != ImeContext::CONVERSION) {
    return false;
  }

  const std::string raw = context_->composer().GetRawString();
  if (raw.empty()) return false;

  // The display protected something on top of Mozc's own conversion (an
  // English piece, a SystemOne choice).  The converter's commit would restore
  // the un-protected segments, so the visible text is committed directly.
  // Without protection the ordinary commit wins: same text, plus the
  // learning and undo semantics only CommitInternal() provides.
  if (!meltype_display_protected_) return false;

  // The composing is over, so a trailing partial word is now a complete one
  // and must not be read as in-progress romaji: re-apply the protection with
  // final=true on the last visible preedit (whose Mozc values and keys are
  // still the baseline this pipeline protects on top of).
  commands::Preedit final_preedit = live_conversion_preedit_output_;
  if (final_preedit.segment_size() == 0) return false;
  ApplyMeltypePiecePreedit(raw, /*final=*/true, &final_preedit);

  std::string value;
  std::string unused_key;
  ExtractPreeditKeyAndValue(final_preedit, &unused_key, &value);
  if (value.empty()) return false;

  const std::string key =
      live_conversion_key_.empty()
          ? context_->composer().GetQueryForConversion()
          : live_conversion_key_;
  if (key.empty()) return false;

  ClearLiveConversionState();
  CommitStringDirectly(key, value, command);
  return true;
}

std::pair<std::string, std::string>
Session::GetPendingLiveConversionDisplayCommitStrings() const {
  const std::string key =
      context_->composer().GetQueryForConversion();
  const std::string raw_preedit =
      context_->composer().GetStringForPreedit();

  if (pending_live_conversion_presentation_.has_value()) {
    const PendingLiveConversionPresentation& presentation =
        *pending_live_conversion_presentation_;
    if (!presentation.key.empty() && !presentation.raw_preedit.empty() &&
        !presentation.value.empty() &&
        presentation.preedit_output.segment_size() > 0 &&
        StartsWithString(key, presentation.key) &&
        StartsWithString(raw_preedit, presentation.raw_preedit)) {
      std::string value = presentation.value;
      value.append(raw_preedit.substr(presentation.raw_preedit.size()));
      return {key, std::move(value)};
    }
  }

  const bool has_stable_live_conversion =
      !live_conversion_key_.empty() &&
      !live_conversion_preedit_.empty() &&
      !live_conversion_value_.empty() &&
      live_conversion_preedit_output_.segment_size() > 0;

  if (has_stable_live_conversion &&
      StartsWithString(key, live_conversion_key_) &&
      StartsWithString(raw_preedit, live_conversion_preedit_)) {
    std::string value = live_conversion_value_;
    value.append(raw_preedit.substr(live_conversion_preedit_.size()));
    return {key, std::move(value)};
  }

  return {key, context_->composer().GetStringForSubmission()};
}

bool Session::CommitPendingLiveConversionDisplayDirectly(
    commands::Command* command) {
  auto [key, value] =
      GetPendingLiveConversionDisplayCommitStrings();

  ClearLiveConversionState();
  CommitStringDirectly(key, value, command);
  return true;
}

bool Session::CommitPendingLiveConversionDisplayForSubmit(
    commands::Command* command) {
  if (!live_conversion_pending_ ||
      context_->state() != ImeContext::COMPOSITION) {
    return false;
  }

  auto [key, value] =
      GetPendingLiveConversionDisplayCommitStrings();
  if (key.empty() || value.empty()) {
    CancelPendingLiveConversion();
    return false;
  }

  // Enter accepts only the preedit already visible to the user. The pending
  // converter result is speculative and must not be materialized, committed,
  // or learned. Unlike direct-commit punctuation, Enter preserves Mozc Undo.
  PushDirectCommitUndoContext();
  ClearPendingRerankedPreeditCommitAfterConvertCancel();
  ClearLiveConversionState();
  CommitStringDirectly(key, value, command);

  // Undo() reads the committed result from context_->output() to build the
  // deletion range before restoring the pending preedit snapshot.
  *context_->mutable_output() = command->output();
  return true;
}

void Session::set_client_capability(commands::Capability capability) {
  *context_->mutable_client_capability() = std::move(capability);
}

void Session::set_application_info(commands::ApplicationInfo application_info) {
  *context_->mutable_application_info() = std::move(application_info);
}

const commands::ApplicationInfo& Session::application_info() const {
  return context_->application_info();
}

absl::Time Session::create_session_time() const {
  return context_->create_time();
}

absl::Time Session::last_command_time() const {
  return context_->last_command_time();
}

void Session::ClearPendingRerankedPreeditCommitAfterConvertCancel() {
  pending_reranked_preedit_commit_after_convert_cancel_ = false;
  pending_reranked_preedit_commit_key_.clear();
  pending_reranked_preedit_commit_value_.clear();
  pending_reranked_preedit_commit_segment_keys_.clear();
}

void Session::MaybeSetPendingRerankedPreeditCommitAfterConvertCancel() {
  ClearPendingRerankedPreeditCommitAfterConvertCancel();

  const std::string key = context_->composer().GetQueryForConversion();
  const std::string value = context_->composer().GetStringForSubmission();
  if (key.empty() || value.empty()) {
    return;
  }

  pending_reranked_preedit_commit_after_convert_cancel_ = true;
  pending_reranked_preedit_commit_key_ = key;
  pending_reranked_preedit_commit_value_ = value;

  // Snapshot the visible conversion segment keys before converter->Cancel()
  // clears them.  If the user commits the restored hiragana unchanged, these
  // boundaries let user segment history learn each original conversion segment
  // separately instead of learning only the whole restored preedit string.
  std::vector<std::string> segment_keys;
  if (!context_->converter().GetConversionSegmentKeys(&segment_keys)) {
    return;
  }

  std::string joined_key;
  for (const std::string& segment_key : segment_keys) {
    if (segment_key.empty()) {
      segment_keys.clear();
      break;
    }
    joined_key.append(segment_key);
  }
  if (!segment_keys.empty() && joined_key == key) {
    pending_reranked_preedit_commit_segment_keys_ = std::move(segment_keys);
  }
}

bool Session::ShouldMarkPreeditCommitAsRerankedAfterConvertCancel() const {
  return ShouldMarkPreeditCommitAsRerankedAfterConvertCancel(
      context_->composer());
}

bool Session::ShouldMarkPreeditCommitAsRerankedAfterConvertCancel(
    const composer::Composer& composer) const {
  if (!pending_reranked_preedit_commit_after_convert_cancel_) {
    return false;
  }

  if (composer.GetInputFieldType() == commands::Context::PASSWORD) {
    return false;
  }

  const std::string key = composer.GetQueryForConversion();
  const std::string value = composer.GetStringForSubmission();
  if (key != pending_reranked_preedit_commit_key_ ||
      value != pending_reranked_preedit_commit_value_) {
    return false;
  }

  if (key != value) {
    return false;
  }

  if (Util::CharsLen(key) <
      kMinRerankedPreeditCommitCharsAfterConvertCancel) {
    return false;
  }

  return Util::IsScriptType(key, Util::HIRAGANA);
}

bool Session::CommitPendingRerankedPreeditAfterConvertCancelForDirectCommit(
    const composer::Composer& composer,
    const commands::Context& context,
    absl::string_view reason) {
  if (!pending_reranked_preedit_commit_after_convert_cancel_) {
    return false;
  }

  if (!ShouldMarkPreeditCommitAsRerankedAfterConvertCancel(composer)) {
    ClearPendingRerankedPreeditCommitAfterConvertCancel();
    return false;
  }

  const std::string key = composer.GetQueryForConversion();
  const std::string value = composer.GetStringForSubmission();

  const std::vector<std::string> segment_keys =
      pending_reranked_preedit_commit_segment_keys_;
  context_->mutable_converter()->CommitPreedit(
      composer, context, true, segment_keys);
  SetPendingDirectCommitLearning(key, value, reason);
  ClearPendingRerankedPreeditCommitAfterConvertCancel();
  return true;
}

bool Session::InsertCharacter(commands::Command* command) {
  if (!command->input().has_key()) {
    LOG(ERROR) << "No key event: " << command->input();
    return false;
  }

  const commands::KeyEvent& key = command->input().key();

  // A pending direct-commit learning entry is finalized only when the next real
  // text input starts. If the next key is Backspace/Escape, it is discarded.
  HandlePendingDirectCommitLearningForKeyEvent(key);

  if (key.input_style() == commands::KeyEvent::DIRECT_INPUT &&
      context_->state() == ImeContext::PRECOMPOSITION) {
    // If the key event represents a half width ascii character (ie.
    // key_code is equal to key_string), that key event is not
    // consumed and done echo back.
    // We must not call |EchoBackAndClearUndoContext| for a half-width space
    // here because it should be done in Session::TestSendKey or
    // Session::InsertSpaceHalfWidth. Note that the |key| comes from
    // Session::InsertSpaceHalfWidth and Session::InsertSpaceFullWidth is
    // different from the original key event.
    // For example, when the client sends a key command like
    //   {key.special_key(): HENKAN, key.modifier_keys(): [SHIFT]},
    // Session::InsertSpaceHalfWidth replaces it with
    //   {key.key_string(): " ", key.key_code(): ' '}
    // when you assign [Shift+HENKAN] to [InsertSpaceHalfWidth].
    // So |key.key_code() == ' '| does not always mean that the original key is
    // a space key w/o any modifier.
    // This is why we cannot call |EchoBackAndClearUndoContext| when
    // |key.key_code() == ' '|. This issue was found in b/5872031.
    if (key.key_string().size() == 1 && key.key_code() == key.key_string()[0] &&
        key.key_code() != ' ') {
      return EchoBackAndClearUndoContext(command);
    }

    ClearPendingRerankedPreeditCommitAfterConvertCancel();
    context_->mutable_composer()->InsertCharacterKeyEvent(key);
    CommitCompositionDirectly(command);
    ClearUndoContext();  // UndoContext must be invalidated.
    return true;
  }

  command->mutable_output()->set_consumed(true);

  // A lowercase letter that Meltype places after the finished English word is
  // Japanese input, not more of the word.  The Composer is still in Mozc's
  // temporary Shift+letter ASCII mode (it only leaves it after "AAa"), so put
  // it back first; otherwise the letter would be inserted as raw ASCII and
  // "anoGithub Cli" + "wotsukatte" would stay romaji instead of becoming
  // "あの Github Cli を使って".
  //
  // Done here, before the branches below, so every one of them sees the mode
  // the letter will actually be inserted in.
  EndTemporaryShiftAsciiRunIfWordEnded(key);

  // If a direct-commit punctuation/symbol is typed while delayed live conversion
  // is pending, commit the currently visible pending preedit. Do not materialize
  // the pending conversion here, because that would commit a conversion result
  // that has not been shown to the user yet.
  if (live_conversion_pending_ &&
      CanDirectCommitPendingLiveConversionBeforeInsert(key)) {
    const composer::Composer composer_before_insert = context_->composer();
    context_->mutable_composer()->InsertCharacterKeyEvent(key);
    ClearUndoContext();

    if (CanDirectCommitAfterPunctuation(key)) {
      const auto [direct_commit_key, direct_commit_value] =
          GetDirectCommitStringsWithDirectCommitSuffixFallback(
              composer_before_insert, key);
      const bool learned_reranked_preedit_after_cancel =
          CommitPendingRerankedPreeditAfterConvertCancelForDirectCommit(
              composer_before_insert, command->input().context(),
              "convert_cancel_direct_commit_punctuation");
      if (learned_reranked_preedit_after_cancel) {
        ClearLiveConversionState();
        CommitStringDirectly(direct_commit_key, direct_commit_value, command);
        return true;
      }
      return CommitPendingLiveConversionDisplayDirectly(command);
    }

    ClearPendingRerankedPreeditCommitAfterConvertCancel();

    // The physical key looked like a direct-commit trigger before insertion,
    // but the romaji table may have turned it into a different character.
    // Example: "v." -> "…" or "v," -> "‥".
    //
    // In that case, do not fall back to raw composition, because it discards
    // the stable converted prefix shown by live conversion, e.g. "今日は".
    // Treat it as ordinary continued composition and schedule live conversion
    // for the updated preedit.
    CancelPendingLiveConversion();
    if (MaybeScheduleLiveConversion(command)) {
      return true;
    }

    OutputComposition(command);
    return true;
  }

  const bool was_live_conversion = live_conversion_active_;
  const bool was_pending_live_conversion = live_conversion_pending_;

  if ((live_conversion_active_ || live_conversion_pending_) &&
      ShouldCommitLiveConversionBeforeShiftAsciiInput(
          context_->GetConfig(), context_->composer(), key) &&
      // A Shift+letter typed inside Japanese + English input is the next
      // letter of an English word, not a switch into alphanumeric mode.
      // Committing the live conversion here sends the already-converted
      // Japanese prefix to the application and restarts the composition at
      // the letter, which is how "anoGithubCliwotsukatte" lost its "あの".
      //
      // Both judgements must look at the composition *plus* the letter, not
      // the composition alone: the first letter of a word can still be read
      // as Japanese romaji ("github" + "C" segments as EN:github|JA:C), and a
      // purely Japanese composition is not mixed until that letter lands
      // ("ano" is JA:あの, "anoG" is JA:あの|EN:G).  Asking about the
      // composition as it is now would therefore answer "keep the commit" for
      // the very keystroke that starts the word.  A composition that stays
      // purely Japanese even with the letter ("kyou" + Shift+A is Japanese
      // only when "A" is not read as English) keeps Mozc's documented
      // commit-then-switch behaviour.
      !CompositionEndsWithEnglishPiece(ShiftedAsciiInputChar(key)) &&
      !CompositionMixesJapaneseAndEnglish(ShiftedAsciiInputChar(key))) {
    ClearPendingRerankedPreeditCommitAfterConvertCancel();
    if (live_conversion_active_) {
      const std::string live_key =
          live_conversion_key_.empty()
              ? context_->composer().GetQueryForConversion()
              : live_conversion_key_;
      const std::string live_value =
          live_conversion_value_.empty()
              ? context_->composer().GetStringForSubmission()
              : live_conversion_value_;

      ClearLiveConversionState();
      CommitStringDirectly(live_key, live_value, command);
    } else if (live_conversion_pending_) {
      CommitPendingLiveConversionDisplayDirectly(command);
    }

    context_->mutable_composer()->InsertCharacterKeyEvent(key);
    ClearUndoContext();
    SetSessionState(ImeContext::COMPOSITION, context_.get());
    OutputComposition(command);
    return true;
  }

  // Decide whether this key would extend the trailing unresolved Roman input
  // beyond the refresh budget before live-conversion editing tears down the
  // currently visible passive-suggestion context. Composer is copyable, so the
  // table-aware dry run does not mutate the real composition.
  PassiveSuggestionSnapshot held_pending_roman_suggestion;
  bool should_hold_pending_roman_suggestion = false;
  // Special Romanji tables are software/mobile keyboard state machines
  // (12-key, flick, mobile QWERTY, GODAN, NOTOUCH, etc.).  Their key_code
  // streams are table commands rather than physical desktop Roman input and
  // must retain the existing per-key suggestion/transliteration behavior.
  const bool uses_standard_romanji_table =
      context_->GetRequest().special_romanji_table() ==
      commands::Request::DEFAULT_TABLE;

  if ((context_->state() == ImeContext::COMPOSITION ||
       live_conversion_active_ || live_conversion_pending_) &&
      uses_standard_romanji_table &&
      IsPendingRomanSuggestionHoldKey(key)) {
    composer::Composer composer_after_insert_for_suggestion =
        context_->composer();
    composer_after_insert_for_suggestion.InsertCharacterKeyEvent(key);

    // Shift-triggered temporary alphanumeric input also stores its raw ASCII
    // sequence in CharChunk::pending_.  That state is not unresolved Roman
    // input and must keep refreshing suggestion for every character.
    const transliteration::TransliterationType input_mode_after_insert =
        composer_after_insert_for_suggestion.GetInputMode();
    const bool is_ascii_input_mode =
        input_mode_after_insert == transliteration::HALF_ASCII ||
        input_mode_after_insert == transliteration::FULL_ASCII;

    if (!is_ascii_input_mode) {
      should_hold_pending_roman_suggestion =
          ShouldHoldSuggestionForPendingRoman(
              composer_after_insert_for_suggestion);
    }
    if (should_hold_pending_roman_suggestion) {
      CaptureVisiblePassiveSuggestionSnapshot(
          &held_pending_roman_suggestion);
    }
  }

  // If the current conversion was started by live conversion, ordinary
  // character input should continue editing the composition. Preserve the
  // exact presentation the user saw before canceling the temporary converter
  // state.
  std::optional<PendingLiveConversionPresentation> continued_presentation =
      CapturePendingLiveConversionPresentationForContinuedInput();
  CancelLiveConversionForEditing();
  if (continued_presentation.has_value()) {
    pending_live_conversion_presentation_ =
        std::move(*continued_presentation);
  }

  // Handle shortcut keys selecting a candidate from a list.
  if (MaybeSelectCandidate(command)) {
    ClearPendingRerankedPreeditCommitAfterConvertCancel();
    Output(command);
    return true;
  }

  const std::string composition = context_->composer().GetQueryForConversion();
  bool should_commit = (context_->state() == ImeContext::CONVERSION);

  if (context_->GetRequest().space_on_alphanumeric() ==
          commands::Request::SPACE_OR_CONVERT_COMMITTING_COMPOSITION &&
      context_->state() == ImeContext::COMPOSITION &&
      // TODO(komatsu): Support FullWidthSpace
      composition.ends_with(' ')) {
    should_commit = true;
  }

  bool committed_conversion_before_insert = false;

  if (should_commit) {
    CommitNotTriggeringZeroQuerySuggest(command);
    committed_conversion_before_insert = true;

    if (key.input_style() == commands::KeyEvent::DIRECT_INPUT) {
      // Do ClearUndoContext() because it is a direct input.
      ClearPendingRerankedPreeditCommitAfterConvertCancel();
      ClearUndoContext();
      context_->mutable_composer()->InsertCharacterKeyEvent(key);
      CommitCompositionDirectly(command);
      return true;
    }
  }

  const composer::Composer composer_before_insert = context_->composer();
  context_->mutable_composer()->InsertCharacterKeyEvent(key);
  ClearUndoContext();
  TraceStep("Composer",
            absl::StrCat("raw=\"",
                         TraceText(context_->composer().GetRawString()),
                         "\" -> kana=\"",
                         TraceText(context_->composer().GetStringForPreedit()),
                         "\""));

  if (!was_live_conversion && !live_conversion_pending_ &&
      context_->composer().GetLength() == 1) {
    // A new composition must not reuse the stable prefix from a previous
    // live conversion.
    live_conversion_key_.clear();
    live_conversion_preedit_.clear();
    live_conversion_value_.clear();
    live_conversion_preedit_output_.Clear();
  }

  if (CanDirectCommitAfterPunctuation(key)) {
    const auto [direct_commit_key, direct_commit_value] =
        GetDirectCommitStringsWithDirectCommitSuffixFallback(
            composer_before_insert, key);

    const bool learned_reranked_preedit_after_cancel =
        CommitPendingRerankedPreeditAfterConvertCancelForDirectCommit(
            composer_before_insert, command->input().context(),
            "convert_cancel_direct_commit_punctuation");

    if (!learned_reranked_preedit_after_cancel &&
        (live_conversion_pending_ || was_pending_live_conversion)) {
      return CommitPendingLiveConversionDisplayDirectly(command);
    }

    if (!learned_reranked_preedit_after_cancel && was_live_conversion &&
        CommitLiveConversionResult(command)) {
      return true;
    }

    if (learned_reranked_preedit_after_cancel) {
      ClearLiveConversionState();
      CommitStringDirectly(direct_commit_key, direct_commit_value, command);
      return true;
    }

    if (committed_conversion_before_insert) {
      SetPendingDirectCommitLearningFromCommittedResult(
          *command,
          "normal_conversion_direct_commit_punctuation");
    }

    // Most direct-commit punctuation paths can commit the current Composer
    // contents as-is.  However,
    // GetDirectCommitStringsWithDirectCommitSuffixFallback() may explicitly
    // append a suffix derived from the key event when Composer has not yet
    // reflected the trigger.  Use the rewritten strings only when they differ
    // from the current Composer contents, keeping the ordinary path unchanged.
    if (direct_commit_key != context_->composer().GetQueryForConversion() ||
        direct_commit_value != context_->composer().GetStringForSubmission()) {
      CommitStringDirectly(direct_commit_key, direct_commit_value, command);
      return true;
    }

    CommitCompositionDirectly(command);
    return true;
  }

  ClearPendingRerankedPreeditCommitAfterConvertCancel();

  if (context_->mutable_composer()->ShouldCommit()) {
    CommitCompositionDirectly(command);
    return true;
  }
  size_t length_to_commit = 0;
  if (context_->composer().ShouldCommitHead(&length_to_commit)) {
    return CommitHead(length_to_commit, command);
  }

  SetSessionState(ImeContext::COMPOSITION, context_.get());
  if (CanStartAutoConversion(key)) {
    CancelPendingLiveConversion();
    return Convert(command);
  }

  composition_holds_passive_suggestion_ = false;
  composition_holds_passive_suggestion_query_.clear();

  if (MaybeScheduleLiveConversion(
          command,
          should_hold_pending_roman_suggestion
              ? &held_pending_roman_suggestion
              : nullptr)) {
    return true;
  }

  if (should_hold_pending_roman_suggestion) {
    // Keep the converter's existing suggestion state. Output() refreshes the
    // preedit from the current Composer while avoiding StartPrediction().
    composition_holds_passive_suggestion_ =
        context_->converter().CheckState(EngineConverterInterface::SUGGESTION);
    // Record which query produced the held list.  The Composer has already
    // advanced past the suggestion's own query, so promotion must compare this
    // recorded value instead of the (always-matching) snapshot Composer.
    if (composition_holds_passive_suggestion_) {
      composition_holds_passive_suggestion_query_ =
          held_pending_roman_suggestion.suggestion_query;
    } else {
      composition_holds_passive_suggestion_query_.clear();
    }
    Output(command);
    return true;
  }

  if (Suggest(command->input())) {
    Output(command);
    return true;
  }

  OutputComposition(command);
  return true;
}

bool Session::IsFullWidthInsertSpace(const commands::Input& input) const {
  // If IME is off, any space has to be half-width.
  if (context_->state() == ImeContext::DIRECT) {
    return false;
  }

  // In this method, we should not update the actual input mode stored in
  // the composer even when |input| has a new input mode. Note that this
  // method can be called from TestSendKey, where internal input mode is
  // is not expected to be changed. This is one of the reasons why this
  // method is a const method.
  // On the other hand, this method should behave as if the new input mode
  // in |input| was applied. For example, this method should behave as if
  // the current input mode was HALF_KATAKANA in the following situation.
  //   composer's input mode: HIRAGANA
  //   input.key().mode()   : HALF_KATAKANA
  // To achieve this, we create a temporary composer object to which the
  // new input mode will be stored when |input| has a new input mode.
  auto get_input_mode = [this, &input]() {
    const bool has_mode = (input.has_key() && input.key().has_mode());
    if (!has_mode) {
      return context_->composer().GetInputMode();
    }

    // Copy the current composer state just in case.
    composer::Composer temporary_composer = context_->composer();
    ApplyCompositionMode(input.key().mode(), &temporary_composer);
    // Refer to this temporary composer in this method.
    return temporary_composer.GetInputMode();
  };

  // Check the current config and the current input status.
  bool is_full_width = false;
  switch (context_->GetConfig().space_character_form()) {
    case config::Config::FUNDAMENTAL_INPUT_MODE: {
      const transliteration::TransliterationType input_mode = get_input_mode();
      if (transliteration::T13n::IsInHalfAsciiTypes(input_mode) ||
          transliteration::T13n::IsInHalfKatakanaTypes(input_mode)) {
        is_full_width = false;
      } else {
        is_full_width = true;
      }
      break;
    }
    case config::Config::FUNDAMENTAL_FULL_WIDTH:
      is_full_width = true;
      break;
    case config::Config::FUNDAMENTAL_HALF_WIDTH:
      is_full_width = false;
      break;
    default:
      LOG(WARNING) << "Unknown input mode";
      is_full_width = false;
      break;
  }

  return is_full_width;
}

bool Session::InsertSpace(commands::Command* command) {
  if (IsFullWidthInsertSpace(command->input())) {
    return InsertSpaceFullWidth(command);
  } else {
    return InsertSpaceHalfWidth(command);
  }
}

bool Session::InsertSpaceToggled(commands::Command* command) {
  if (IsFullWidthInsertSpace(command->input())) {
    return InsertSpaceHalfWidth(command);
  } else {
    return InsertSpaceFullWidth(command);
  }
}

bool Session::InsertSpaceHalfWidth(commands::Command* command) {
  if (!(context_->state() &
        (ImeContext::PRECOMPOSITION | ImeContext::COMPOSITION |
         ImeContext::CONVERSION))) {
    return DoNothing(command);
  }

  if (context_->state() == ImeContext::PRECOMPOSITION) {
    // TODO(komatsu): This is a hack to work around the problem with
    // the inconsistency between TestSendKey and SendKey.
    if (IsPureSpaceKey(command->input().key())) {
      return EchoBackAndClearUndoContext(command);
    }
    // UndoContext will be cleared in |InsertCharacter| in this case.
  }

  const bool has_mode = command->input().key().has_mode();
  const commands::CompositionMode mode = command->input().key().mode();
  command->mutable_input()->clear_key();
  commands::KeyEvent* key_event = command->mutable_input()->mutable_key();
  key_event->set_key_code(' ');
  key_event->set_key_string(" ");
  key_event->set_input_style(commands::KeyEvent::DIRECT_INPUT);
  if (has_mode) {
    key_event->set_mode(mode);
  }
  EndTemporaryShiftAsciiRunBeforeSpace();
  return InsertCharacter(command);
}

bool Session::InsertSpaceFullWidth(commands::Command* command) {
  if (!(context_->state() &
        (ImeContext::PRECOMPOSITION | ImeContext::COMPOSITION |
         ImeContext::CONVERSION))) {
    return DoNothing(command);
  }

  if (context_->state() == ImeContext::PRECOMPOSITION) {
    // UndoContext will be cleared in |InsertCharacter| in this case.

    // TODO(komatsu): make sure if
    // |context_->mutable_converter()->Reset()| is necessary here.
    context_->mutable_converter()->Reset();
  }

  const bool has_mode = command->input().key().has_mode();
  const commands::CompositionMode mode = command->input().key().mode();
  command->mutable_input()->clear_key();
  commands::KeyEvent* key_event = command->mutable_input()->mutable_key();
  key_event->set_key_code(' ');
  key_event->set_key_string("　");  // full-width space
  key_event->set_input_style(commands::KeyEvent::DIRECT_INPUT);
  if (has_mode) {
    key_event->set_mode(mode);
  }
  EndTemporaryShiftAsciiRunBeforeSpace();
  return InsertCharacter(command);
}

bool Session::TryCancelConvertReverse(commands::Command* command) {
  // If source_text is set, it usually means this session started by a
  // reverse conversion.
  if (context_->composer().source_text().empty()) {
    return false;
  }
  CommitSourceTextDirectly(command);
  return true;
}

bool Session::EditCancelOnPasswordField(commands::Command* command) {
  if (context_->composer().GetInputFieldType() != commands::Context::PASSWORD) {
    return false;
  }

  // In password mode, we should commit preedit and close keyboard
  // on Android.
  // TODO(matsuzakit): Remove this trick. b/5955618
  if (context_->composer().source_text().empty()) {
    CommitCompositionDirectly(command);
  } else {
    // Commits original text of reverse conversion.
    CommitSourceTextDirectly(command);
  }
  // Passes the key event through to MozcService.java
  // to continue the processes which are invoked by cancel operation.
  command->mutable_output()->set_consumed(false);

  return true;
}

bool Session::EditCancel(commands::Command* command) {
  DiscardPendingDirectCommitLearning(
      "edit_cancel_after_direct_commit_learning");

  ClearPendingRerankedPreeditCommitAfterConvertCancel();

  if (EditCancelOnPasswordField(command)) {
    return true;
  }

  command->mutable_output()->set_consumed(true);

  TryCancelConvertReverse(command);

  SetStateToPredompositionAndCancel(context_.get());
  ClearLiveConversionState();
  Output(command);
  return true;
}

bool Session::EditCancelAndIMEOff(commands::Command* command) {
  DiscardPendingDirectCommitLearning(
      "edit_cancel_and_ime_off_after_direct_commit_learning");

  ClearPendingRerankedPreeditCommitAfterConvertCancel();

  if (EditCancelOnPasswordField(command)) {
    return true;
  }

  if (!(context_->state() &
        (ImeContext::PRECOMPOSITION | ImeContext::COMPOSITION |
         ImeContext::CONVERSION))) {
    return DoNothing(command);
  }

  command->mutable_output()->set_consumed(true);

  TryCancelConvertReverse(command);

  ClearUndoContext();

  // Reset the context.
  context_->mutable_converter()->Reset();

  SetSessionState(ImeContext::DIRECT, context_.get());
  ClearLiveConversionState();
  Output(command);
  return true;
}

bool Session::CommitInternal(commands::Command* command,
                             bool trigger_zero_query_suggest) {
  if (!(context_->state() &
        (ImeContext::COMPOSITION | ImeContext::CONVERSION))) {
    return DoNothing(command);
  }
  command->mutable_output()->set_consumed(true);

  PushUndoContext();

  if (context_->state() == ImeContext::COMPOSITION) {
    const bool mark_preedit_as_reranked =
        ShouldMarkPreeditCommitAsRerankedAfterConvertCancel();
    std::vector<std::string> segment_keys;
    if (mark_preedit_as_reranked) {
      segment_keys = pending_reranked_preedit_commit_segment_keys_;
    }
    ClearPendingRerankedPreeditCommitAfterConvertCancel();
    context_->mutable_converter()->CommitPreedit(
        context_->composer(), command->input().context(),
        mark_preedit_as_reranked, segment_keys);
  } else {  // ImeContext::CONVERSION
    ClearPendingRerankedPreeditCommitAfterConvertCancel();
    context_->mutable_converter()->Commit(context_->composer(),
                                          command->input().context());
  }

  SetSessionState(ImeContext::PRECOMPOSITION, context_.get());

  if (trigger_zero_query_suggest) {
    Suggest(command->input());
  }

  Output(command);
  // Copy the previous output for Undo.
  *context_->mutable_output() = command->output();

  ClearLiveConversionState();
  return true;
}

bool Session::Commit(commands::Command* command) {
  // The visible preedit is the Meltype piece display (English pieces show the
  // typed letters, Japanese pieces show their chosen candidate); the
  // converter's candidate list is a different segmentation and never carries
  // the English pieces.  Committing through CommitInternal() would therefore
  // restore a surface the user never saw, which is what made "google" commit
  // as a kanji run that no longer matched the composing.
  if (CommitMeltypePieceLiveConversion(command)) {
    return true;
  }

  if (CommitPendingLiveConversionDisplayForSubmit(command)) {
    return true;
  }

  return CommitInternal(command,
                        context_->GetRequest().zero_query_suggestion());
}

bool Session::CommitNotTriggeringZeroQuerySuggest(commands::Command* command) {
  return CommitInternal(command, false);
}

bool Session::CommitHead(size_t count, commands::Command* command) {
  if (!(context_->state() &
        (ImeContext::COMPOSITION | ImeContext::PRECOMPOSITION))) {
    return DoNothing(command);
  }
  command->mutable_output()->set_consumed(true);

  // TODO(yamaguchi): Support undo feature.
  ClearUndoContext();

  size_t committed_size;
  context_->mutable_converter()->CommitHead(count, context_->composer(),
                                            &committed_size);
  context_->mutable_composer()->DeleteRange(0, committed_size);
  Output(command);
  return true;
}

bool Session::CommitFirstSuggestion(commands::Command* command) {
  if (!(context_->state() == ImeContext::COMPOSITION ||
        context_->state() == ImeContext::PRECOMPOSITION)) {
    return DoNothing(command);
  }
  if (!context_->converter().IsActive()) {
    return DoNothing(command);
  }
  command->mutable_output()->set_consumed(true);

  PushUndoContext();

  constexpr int kFirstIndex = 0;
  size_t committed_key_size = 0;
  context_->mutable_converter()->CommitSuggestionByIndex(
      kFirstIndex, context_->composer(), command->input().context(),
      &committed_key_size);

  SetSessionState(ImeContext::PRECOMPOSITION, context_.get());

  // Get suggestion if zero_query_suggestion is set.
  // zero_query_suggestion is usually set where the client is a mobile.
  if (context_->GetRequest().zero_query_suggestion()) {
    Suggest(command->input());
  }

  Output(command);
  // Copy the previous output for Undo.
  *context_->mutable_output() = command->output();
  return true;
}

bool Session::CommitSegment(commands::Command* command) {
  if (!(context_->state() & (ImeContext::CONVERSION))) {
    return DoNothing(command);
  }
  command->mutable_output()->set_consumed(true);

  PushUndoContext();

  size_t size;
  context_->mutable_converter()->CommitFirstSegment(
      context_->composer(), command->input().context(), &size);
  if (size > 0) {
    // Delete the key characters of the first segment from the preedit.
    context_->mutable_composer()->DeleteRange(0, size);
    // The number of segments should be more than one.
    DCHECK_GT(context_->composer().GetLength(), 0);
  }

  if (!context_->converter().IsActive()) {
    // If the converter is not active (ie. the segment size was one.),
    // the state should be switched to precomposition.
    SetSessionState(ImeContext::PRECOMPOSITION, context_.get());

    // Get suggestion if zero_query_suggestion is set.
    // zero_query_suggestion is usually set where the client is a mobile.
    if (context_->GetRequest().zero_query_suggestion()) {
      Suggest(command->input());
    }
  }
  Output(command);
  // Copy the previous output for Undo.
  *context_->mutable_output() = command->output();
  return true;
}

void Session::CommitHeadToFocusedSegmentsInternal(
    const commands::Context& context) {
  size_t size;
  context_->mutable_converter()->CommitHeadToFocusedSegments(
      context_->composer(), context, &size);
  if (size > 0) {
    // Delete the key characters of the first segment from the preedit.
    context_->mutable_composer()->DeleteRange(0, size);
    // The number of segments should be more than one.
    DCHECK_GT(context_->composer().GetLength(), 0);
  }
}

void Session::CommitCompositionDirectly(commands::Command* command) {
  const std::string composition = context_->composer().GetQueryForConversion();
  const std::string conversion = context_->composer().GetStringForSubmission();
  CommitStringDirectly(composition, conversion, command);
}

void Session::CommitSourceTextDirectly(commands::Command* command) {
  // We cannot use a reference since composer will be cleared on
  // CommitStringDirectly.
  absl::string_view copied_source_text = context_->composer().source_text();
  CommitStringDirectly(copied_source_text, copied_source_text, command);
}

void Session::CommitRawTextDirectly(commands::Command* command) {
  const std::string raw_text = context_->composer().GetRawString();
  CommitStringDirectly(raw_text, raw_text, command);
}

void Session::CommitStringDirectly(absl::string_view key,
                                   absl::string_view preedit,
                                   commands::Command* command) {
  if (key.empty() || preedit.empty()) {
    return;
  }

  command->mutable_output()->set_consumed(true);
  context_->mutable_converter()->Reset();

  commands::Result* result = command->mutable_output()->mutable_result();
  DCHECK(result != nullptr);
  result->set_type(commands::Result::STRING);
  result->mutable_key()->append(key);
  result->mutable_value()->append(preedit);
  SetSessionState(ImeContext::PRECOMPOSITION, context_.get());

  // Get suggestion if zero_query_suggestion is set.
  // zero_query_suggestion is usually set where the client is a mobile.
  if (context_->GetRequest().zero_query_suggestion()) {
    Suggest(command->input());
  }

  Output(command);
}

namespace {
bool SuppressSuggestion(const commands::Input& input) {
  if (!input.has_context()) {
    return false;
  }
  if (input.context().has_suppress_suggestion() &&
      input.context().suppress_suggestion()) {
    return true;
  }
  // If the target input field is in Chrome's Omnibox or Google
  // search box, the suggest window is hidden.
  for (size_t i = 0; i < input.context().experimental_features_size(); ++i) {
    const std::string& feature = input.context().experimental_features(i);
    if (feature == "chrome_omnibox" || feature == "google_search_box") {
      return true;
    }
  }
  return false;
}

bool IsAsciiUpperAlpha(const char c) {
  return 'A' <= c && c <= 'Z';
}

bool IsAsciiLowerAlpha(const char c) {
  return 'a' <= c && c <= 'z';
}

bool LooksLikeShiftedAsciiRomanizedSuggestionContext(
    const composer::Composer& composer) {
  const transliteration::TransliterationType input_mode =
      composer.GetInputMode();
  if (input_mode == transliteration::HALF_ASCII ||
      input_mode == transliteration::FULL_ASCII) {
    return false;
  }

  const std::string raw = composer.GetRawString();
  if (raw.size() < 3 || !IsAsciiUpperAlpha(raw[0]) ||
      !IsAsciiUpperAlpha(raw[1])) {
    return false;
  }

  bool has_lower_alpha = false;
  for (const char c : raw) {
    if (static_cast<unsigned char>(c) >= 0x80) {
      return false;
    }
    if (IsAsciiLowerAlpha(c)) {
      has_lower_alpha = true;
    }
  }
  if (!has_lower_alpha) {
    return false;
  }

  // Example:
  //   raw     = "AIde"
  //   preedit = "AI + Japanese text"
  //
  // This is the ambiguity caused by the shifted ASCII sequence being reverted
  // to Japanese input.  When dictionary suggest is disabled, do not surface the
  // raw ASCII rescue candidate automatically.
  return raw != composer.GetStringForPreedit();
}

bool ShouldSuppressShiftedAsciiAutoSuggestion(
    const config::Config& config,
    const composer::Composer& composer) {
  if (config.use_dictionary_suggest()) {
    return false;
  }
  return composer.is_in_shifted_ascii_revert_context() ||
         LooksLikeShiftedAsciiRomanizedSuggestionContext(composer);
}

// Ceiling on the per-request budget the worker enforces, and on how long the
// poll chain waits for the answer.  The request now runs off the key-event
// path, but an answer older than a few hundred milliseconds is worthless for a
// composition the user is still typing, so the configured budget stays capped.
constexpr uint32_t kMaxSystemOneWaitMsec = 500;

// Slack added to the request budget before the poll chain gives up.  The
// worker's Ask() honors the request timeout itself; this covers thread
// scheduling and debug logging on top of it.
constexpr uint32_t kSystemOneResultSlackMsec = 150;

}  // namespace

SystemOneWorker* Session::EnsureSystemOneWorker() {
  if (systemone_worker_ == nullptr) {
    systemone_worker_ = std::make_unique<SystemOneWorker>();
  }
  return systemone_worker_.get();
}

// Cap on the candidate list one piece offers SystemOne.  The dictionary can
// hold hundreds of entries for a reading; the model picks from a short list,
// and the request carries one criteria entry per option.
constexpr std::size_t kMaxPieceOptions = 10;

// True when Mozc marked |candidate| as its own typo correction
// ("もしかして").  CorrectionRewriter sets the attribute unconditionally and
// additionally writes the "→ " prefix and the "<もしかして: ...>" description
// only on the legacy (non display_value) path, so any of the three is proof.
bool IsMozcTypoCorrection(const commands::CandidateWord& candidate) {
  for (const int attribute : candidate.attributes()) {
    if (attribute == commands::SPELLING_CORRECTION ||
        attribute == commands::TYPING_CORRECTION) {
      return true;
    }
  }
  if (!candidate.has_annotation()) return false;
  const commands::Annotation& annotation = candidate.annotation();
  if (annotation.has_prefix() && annotation.prefix() == "→ ") return true;
  if (annotation.has_description()) {
    const absl::string_view description = annotation.description();
    if (absl::StrContains(description, "もしかして") ||
        absl::StrContains(description, "Did you mean")) {
      return true;
    }
  }
  return false;
}

std::vector<std::vector<std::string>> Session::CollectMozcPieceOptions(
    const std::vector<mozkey::meltype::ScoredSegmenter::Segment>& pieces,
    std::vector<bool>* typo_corrections) {
  std::vector<std::vector<std::string>> options(pieces.size());
  if (typo_corrections != nullptr) {
    typo_corrections->assign(pieces.size(), false);
  }
  if (piece_converter_ == nullptr) return options;

  composer::Composer piece_composer;
  for (std::size_t i = 0; i < pieces.size(); ++i) {
    if (pieces[i].is_english || pieces[i].kana.empty()) continue;

    const auto cached = piece_options_cache_.find(pieces[i].kana);
    if (cached != piece_options_cache_.end()) {
      options[i] = cached->second.surfaces;
      if (typo_corrections != nullptr) {
        (*typo_corrections)[i] = cached->second.typo_correction;
      }
      continue;
    }

    // The reading enters the composer as-is: InsertCharacterPreedit bypasses
    // transliteration, so no romaji table is involved and the conversion
    // query is exactly the piece's kana.
    piece_composer.Reset();
    piece_composer.SetInputMode(transliteration::HIRAGANA);
    piece_composer.InsertCharacterPreedit(pieces[i].kana);
    if (!piece_converter_->Convert(piece_composer,
                                   context_->client_context())) {
      continue;
    }

    commands::Output scratch;
    piece_converter_->FillOutput(piece_composer, &scratch);

    // Mozc keeps its natural clause segmentation: the focused (first)
    // segment's candidates vary that clause, and the remaining segments'
    // preedit values form the untouched tail.  Concatenating the two gives
    // whole-piece surfaces for SystemOne without merging the segments into a
    // single clause (no SegmentWidthExpand), so the multi-clause quality
    // stays intact — and the first option equals the conversion preedit.
    std::string tail;
    if (scratch.has_preedit()) {
      const auto& preedit_segments = scratch.preedit().segment();
      for (int s = 1; s < preedit_segments.size(); ++s) {
        tail += preedit_segments[s].value();
      }
    }

    // Mozc's typo correction ("もしかして") is collected separately: it is
    // promoted to the front below, so the kMaxPieceOptions cut must never
    // drop it.  The dictionary happily puts it at index 5 or later.
    std::vector<PieceCandidate> piece_candidates;
    std::vector<std::string> correction_surfaces;
    if (scratch.has_all_candidate_words()) {
      for (const commands::CandidateWord& candidate :
           scratch.all_candidate_words().candidates()) {
        if (candidate.value().empty()) continue;
        std::string surface = candidate.value() + tail;
        if (std::find(correction_surfaces.begin(), correction_surfaces.end(),
                      surface) != correction_surfaces.end()) {
          continue;
        }
        if (IsMozcTypoCorrection(candidate)) {
          correction_surfaces.push_back(std::move(surface));
          continue;
        }
        if (std::find_if(piece_candidates.begin(), piece_candidates.end(),
                         [&surface](const PieceCandidate& existing) {
                           return existing.surface == surface;
                         }) != piece_candidates.end()) {
          continue;
        }
        if (piece_candidates.size() >= kMaxPieceOptions) continue;
        piece_candidates.push_back(PieceCandidate{std::move(surface), false});
      }
    }
    for (std::string& surface : correction_surfaces) {
      piece_candidates.push_back(PieceCandidate{std::move(surface), true});
    }

    // No candidates at all: the conversion preedit still carries the piece's
    // surface, so the question keeps one honest option instead of vanishing.
    if (piece_candidates.empty() && scratch.has_preedit()) {
      std::string unused_key;
      std::string value;
      ExtractPreeditKeyAndValue(scratch.preedit(), &unused_key, &value);
      if (!value.empty()) {
        piece_candidates.push_back(PieceCandidate{std::move(value), false});
      }
    }
    if (piece_candidates.empty()) continue;

    OrderedPieceCandidates ordered = OrderPieceCandidates(piece_candidates);
    if (ordered.surfaces.empty()) continue;

    piece_options_cache_[pieces[i].kana] = ordered;
    options[i] = std::move(ordered.surfaces);
    if (typo_corrections != nullptr) {
      (*typo_corrections)[i] = ordered.typo_correction;
    }
  }
  return options;
}

bool Session::RebuildMeltypePiecePlan(const std::string& raw, bool final) {
  if (raw.empty()) {
    meltype_piece_plan_ = PiecePlan();
    meltype_piece_choices_.clear();
    return false;
  }

  const std::vector<mozkey::meltype::ScoredSegmenter::Segment> pieces =
      MeltypeBridge::SegmentRomaji(raw, final);
  // Empty is "no opinion" — dictionaries unavailable or nothing to say — and
  // both mean "keep Mozc's own output".
  if (pieces.empty()) {
    meltype_piece_plan_ = PiecePlan();
    meltype_piece_choices_.clear();
    return false;
  }

  std::vector<bool> typo_corrections;
  const std::vector<std::vector<std::string>> options =
      CollectMozcPieceOptions(pieces, &typo_corrections);
  meltype_piece_plan_ = BuildPiecePlan(pieces, options, typo_corrections);
  // Mozc's own "もしかして" for a piece is promoted to that piece's first
  // candidate, so the display shows the corrected reading at 0ms instead of
  // the mis-conversion, and SystemOne sees it as the natural baseline.
  meltype_piece_choices_.assign(pieces.size(), std::string());

  for (const PieceQuestion& question : meltype_piece_plan_.questions) {
    const auto chosen = piece_choice_cache_.find(PieceChoiceKey(question));
    if (chosen == piece_choice_cache_.end()) continue;
    if (ApplyPieceChoice(&meltype_piece_plan_, question.piece_index,
                         chosen->second)) {
      meltype_piece_choices_[question.piece_index] = chosen->second;
    } else {
      // The cached choice no longer fits the current options (the candidate
      // list moved): forget it so the piece is asked again instead of being
      // stuck between cache and options.
      piece_choice_cache_.erase(chosen);
    }
  }

  // A piece without a proven SystemOne answer but with Mozc's typo
  // correction keeps that correction on screen (and in the commit).
  meltype_piece_choices_ =
      ResolvePieceDisplayChoices(meltype_piece_plan_, meltype_piece_choices_);
  return true;
}

bool Session::ApplyMeltypePiecePreedit(const std::string& raw, bool final,
                                       commands::Preedit* preedit) {
  meltype_display_protected_ = false;
  if (preedit == nullptr || raw.empty()) return false;
  if (preedit->segment_size() == 0) return false;
  if (!RebuildMeltypePiecePlan(raw, final)) return false;

  // Snapshot what Mozc showed: its keys are the reading slices, its values
  // the natural multi-clause conversion this pipeline keeps as the baseline.
  std::vector<PieceSegment> mozc_segments;
  mozc_segments.reserve(preedit->segment_size());
  for (int i = 0; i < preedit->segment_size(); ++i) {
    const commands::Preedit::Segment& segment = preedit->segment(i);
    mozc_segments.push_back(
        PieceSegment{segment.has_key() ? segment.key() : std::string(),
                     segment.value()});
  }

  const std::vector<PieceSegment> protected_segments =
      ProtectSegmentsWithPieces(mozc_segments, meltype_piece_plan_.pieces,
                                meltype_piece_choices_);
  if (protected_segments == mozc_segments) return false;

  // Same size alone does not mean the same partition: the English-boundary
  // slice can split one segment and the English collapse can merge two, so
  // the counts can match while the keys moved.  Value-swapping in that case
  // would put a protected surface under the wrong reading; compare the keys
  // and fall back to the rebuild.
  bool same_partition =
      protected_segments.size() == mozc_segments.size();
  if (same_partition) {
    for (std::size_t i = 0; i < mozc_segments.size(); ++i) {
      if (protected_segments[i].key != mozc_segments[i].key) {
        same_partition = false;
        break;
      }
    }
  }

  if (same_partition) {
    // Only values changed: keep every key, annotation and boundary Mozc
    // produced and swap the surfaces in place.
    for (int i = 0; i < preedit->segment_size(); ++i) {
      const PieceSegment& rewritten = protected_segments[i];
      commands::Preedit::Segment* segment = preedit->mutable_segment(i);
      if (segment->value() != rewritten.value) {
        segment->set_value(rewritten.value);
        segment->set_value_length(Util::CharsLen(rewritten.value));
      }
    }
  } else {
    // The partition changed: a SystemOne choice collapsed a run, or the
    // English-boundary slice split one segment.  Rebuild from the protected
    // segments.  annotation is required in the proto, so carry the first
    // segment's over.
    const commands::Preedit::Segment::Annotation annotation =
        preedit->segment(0).annotation();
    preedit->clear_segment();
    for (const PieceSegment& rewritten : protected_segments) {
      commands::Preedit::Segment* segment = preedit->add_segment();
      segment->set_key(rewritten.key);
      segment->set_value(rewritten.value);
      segment->set_value_length(Util::CharsLen(rewritten.value));
      segment->set_annotation(annotation);
    }
  }

  // Live typing keeps the caret at the end of the now-protected value;
  // |cursor| counts characters, not bytes.
  std::string protected_value;
  for (const PieceSegment& segment : protected_segments) {
    protected_value += segment.value;
  }
  preedit->set_cursor(static_cast<int>(Util::CharsLen(protected_value)));

  meltype_display_protected_ = true;
  return true;
}

std::string Session::PieceChoiceKey(const PieceQuestion& question) {
  return absl::StrCat(question.piece_index, "\x02", question.reading);
}

int Session::HarvestPieceSystemOneAnswers() {
  if (!pending_piece_systemone_.pending) return 0;

  const config::Config& config = context_->GetConfig();
  const uint32_t wait_budget_msec =
      std::min<uint32_t>(config.systemone_timeout_msec(),
                         kMaxSystemOneWaitMsec) +
      kSystemOneResultSlackMsec;
  const std::vector<std::optional<SystemOneResponse>> results =
      EnsureSystemOneWorker()->TakeResults(
          pending_piece_systemone_.generation,
          pending_piece_systemone_.requests.size());
  const absl::Time now = Clock::GetAbslTime();
  SystemOneLogger logger(config.systemone_debug_logging());

  int newly_handled = 0;
  std::size_t unhandled = 0;
  for (std::size_t i = 0; i < pending_piece_systemone_.requests.size(); ++i) {
    if (pending_piece_systemone_.handled[i]) continue;

    const SystemOneRequest& request = pending_piece_systemone_.requests[i];
    const PieceQuestion& question = pending_piece_systemone_.questions[i];

    if (i < results.size() && results[i].has_value()) {
      const SystemOneResponse& response = *results[i];
      pending_piece_systemone_.handled[i] = true;
      ++newly_handled;
      logger.LogExchange(request, response);
      if (response.ok &&
          std::find(question.options.begin(), question.options.end(),
                    response.choice) != question.options.end()) {
        piece_choice_cache_[PieceChoiceKey(question)] = response.choice;
        TraceStep("SystemOne Piece",
                  absl::StrCat("status=OK | piece=", question.piece_index,
                               " | reading=\"", TraceText(question.reading),
                               "\" | choice=\"",
                               TraceText(response.choice),
                               "\" | latency=", response.latency_msec,
                               "ms"));
      } else {
        TraceStep("SystemOne Piece",
                  absl::StrCat(
                      "status=", response.ok ? "REJECTED" : "FAILED",
                      " | piece=", question.piece_index, " | reading=\"",
                      TraceText(question.reading), "\"",
                      response.error_message.empty()
                          ? std::string()
                          : absl::StrCat(" | ",
                                         TraceText(response.error_message))));
      }
      continue;
    }

    if (now - pending_piece_systemone_.issued_at >=
        absl::Milliseconds(wait_budget_msec)) {
      pending_piece_systemone_.handled[i] = true;
      ++newly_handled;
      SystemOneResponse expired;
      expired.error_message = absl::StrCat(
          "async wait exhausted after ", wait_budget_msec,
          "ms; keeping the first dictionary candidate");
      logger.LogExchange(request, expired);
      TraceStep("SystemOne Piece",
                absl::StrCat("status=TIMEOUT | piece=", question.piece_index,
                             " | reading=\"", TraceText(question.reading),
                             "\" | wait=", wait_budget_msec, "ms"));
      continue;
    }
    ++unhandled;
  }

  if (unhandled == 0) {
    // The batch is fully read; drop the worker's slot so a late store from an
    // in-flight request can never surface under someone else's questions.
    pending_piece_systemone_.pending = false;
    if (systemone_worker_ != nullptr) {
      systemone_worker_->CancelPending();
    }
  }
  return newly_handled;
}

bool Session::MaybeSubmitPieceSystemOne(const config::Config& config,
                                        commands::Command* command) {
  if (!config.use_systemone_decision() ||
      meltype_piece_plan_.questions.empty()) {
    // Off means the pipeline still runs at 0ms with the first dictionary
    // candidate — it simply never asks anyone.
    if (pending_piece_systemone_.pending) {
      CancelPendingSystemOneDecision();
    }
    return false;
  }

  std::vector<PieceQuestion> missing;
  for (const PieceQuestion& question : meltype_piece_plan_.questions) {
    if (piece_choice_cache_.contains(PieceChoiceKey(question))) continue;
    missing.push_back(question);
  }
  if (missing.empty()) {
    // Everything on screen is already a proven choice; no question, no batch.
    if (pending_piece_systemone_.pending) {
      CancelPendingSystemOneDecision();
    }
    return false;
  }

  // The rebuild at the top of this keystroke harvested the previous batch,
  // so what the cache still misses is exactly what must be asked now.
  ++systemone_generation_;
  pending_piece_systemone_ = PendingPieceSystemOne();
  pending_piece_systemone_.pending = true;
  pending_piece_systemone_.generation = systemone_generation_;
  pending_piece_systemone_.raw = context_->composer().GetRawString();
  pending_piece_systemone_.issued_at = Clock::GetAbslTime();
  pending_piece_systemone_.trace_seq = trace_seq_;
  pending_piece_systemone_.trace_started_at = trace_sequence_start_;
  pending_piece_systemone_.questions = std::move(missing);
  pending_piece_systemone_.requests.reserve(
      pending_piece_systemone_.questions.size());

  for (const PieceQuestion& question : pending_piece_systemone_.questions) {
    SystemOneRequest request;
    request.endpoint_url = config.systemone_endpoint_url();
    request.api_key = config.systemone_api_key();
    request.model_name = config.systemone_model_name();
    // Off the key-event path now, but an answer older than a few hundred
    // milliseconds is worthless for a composition the user is still typing,
    // so the configured budget stays capped.
    request.timeout_msec =
        std::min<uint32_t>(config.systemone_timeout_msec(),
                           kMaxSystemOneWaitMsec);
    request.state = PieceQuestionState(question);
    request.options = question.options;

    TraceStep("SystemOne Piece",
              absl::StrCat("status=SUBMIT | piece=", question.piece_index,
                           " | state=\"", TraceText(request.state),
                           "\" | options=", request.options.size()));

    pending_piece_systemone_.requests.push_back(std::move(request));
    pending_piece_systemone_.handled.push_back(false);
  }

  EnsureSystemOneWorker()->Submit(pending_piece_systemone_.requests,
                                  pending_piece_systemone_.generation);
  AttachPendingAsyncCallback(command);
  return true;
}

void Session::CancelPendingSystemOneDecision() {
  pending_piece_systemone_ = PendingPieceSystemOne();
  if (systemone_worker_ != nullptr) {
    systemone_worker_->CancelPending();
  }
}

bool Session::IsCurrentSystemOneLiveDecisionCallback(
    const commands::Command& command) const {
  if (!pending_piece_systemone_.pending || !command.input().has_command()) {
    return false;
  }

  const commands::SessionCommand& session_command = command.input().command();
  if (!session_command.has_live_conversion_generation() ||
      session_command.live_conversion_generation() !=
          pending_piece_systemone_.generation ||
      !session_command.has_live_conversion_key() ||
      session_command.live_conversion_key() != pending_piece_systemone_.raw) {
    return false;
  }

  // The answers are only meaningful for the composition they were asked
  // about: live conversion must still own the display, and the raw romaji
  // must be the one the batch was built from.
  return live_conversion_active_ &&
         context_->state() == ImeContext::CONVERSION &&
         context_->composer().GetRawString() == pending_piece_systemone_.raw;
}

void Session::OutputCurrentLiveConversionForSystemOne(
    commands::Command* command) {
  command->mutable_output()->set_consumed(true);

  if (live_conversion_active_ && context_->state() == ImeContext::CONVERSION) {
    Output(command);

    if (command->output().has_preedit()) {
      // Output() rebuilt the preedit from the converter segments, so the
      // Meltype protection ("google" and friends) is gone from it again.
      // Re-applying it here is what keeps every Jev/SystemOne poll tick (this
      // runs every ~24ms while an answer is pending) from flipping the
      // protected English back to Mozc's raw kana a few milliseconds after
      // the keystroke showed the letters.
      ApplyMeltypePiecePreedit(context_->composer().GetRawString(),
                               /*final=*/false,
                               command->mutable_output()->mutable_preedit());
      RestorePreeditSegmentKeysForSymbolStyle(
          live_conversion_preedit_.empty()
              ? live_conversion_key_
              : live_conversion_preedit_,
          command->mutable_output()->mutable_preedit());
    }

    commands::Output* output = command->mutable_output();
    output->set_live_conversion(true);
    output->set_live_conversion_pending(false);
    AttachCachedLiveConversionSuggestionCandidateWindow(output);
    return;
  }

  OutputFromState(command);
}

bool Session::ApplySystemOneLiveDecision(commands::Command* command) {
  // A matching generation means no newer decision replaced this one, so the
  // answer block continues the submitting command's trace.
  const bool trace_this =
      TraceLogger::Enabled() && trace_seq_ == 0 &&
      pending_piece_systemone_.pending &&
      command->input().has_command() &&
      command->input().command().has_live_conversion_generation() &&
      command->input().command().live_conversion_generation() ==
          pending_piece_systemone_.generation;
  if (trace_this) {
    TraceContinue(pending_piece_systemone_.trace_seq,
                  pending_piece_systemone_.trace_started_at);
  }

  const bool result = ApplySystemOneLiveDecisionInternal(command);

  if (trace_this) {
    TraceStep("Display Output", TraceOutputSummary(command->output()));
    TraceFlush("Command: APPLY_SYSTEMONE_LIVE");
  }
  return result;
}

bool Session::ApplySystemOneLiveDecisionInternal(commands::Command* command) {
  command->mutable_output()->set_consumed(true);

  if (!IsCurrentSystemOneLiveDecisionCallback(*command)) {
    // Cancel only when this callback belongs to the pending generation: that
    // means the answers target the current reservation but the composer moved
    // on (the user kept editing), so they are genuinely unusable.  A
    // generation mismatch is an old callback that fired after a newer batch
    // was already scheduled; cancelling here would kill the latest pending
    // reservation during fast typing, so it is kept untouched instead.
    const bool same_generation =
        pending_piece_systemone_.pending &&
        command->input().has_command() &&
        command->input().command().has_live_conversion_generation() &&
        command->input().command().live_conversion_generation() ==
            pending_piece_systemone_.generation;
    if (same_generation) {
      CancelPendingSystemOneDecision();
    }
    // This stale callback consumed the shared Output::Callback slot.  Re-attach
    // so a waiting SystemOne chain keeps its timer instead of stalling.
    const bool stale_result = IgnoreStaleDelayedLiveConversion(command);
    AttachPendingAsyncCallback(command);
    return stale_result;
  }

  // Every answer that arrived (or expired) lands in the choice cache; the
  // rebuild inside OutputCurrentLiveConversionForSystemOne() turns the cache
  // into the visible preedit, so one Japanese piece changing its surface
  // never touches the others.
  const int newly_handled = HarvestPieceSystemOneAnswers();
  if (newly_handled == 0) {
    TraceStep("SystemOne Piece", "status=PENDING");
  }

  OutputCurrentLiveConversionForSystemOne(command);
  if (live_conversion_active_ && command->output().has_preedit()) {
    // The preedit now carries whatever the cache proved (or the first
    // candidate while the batch is still in flight).  These cached copies
    // are what the commit path and the correction pipeline compare against,
    // so leaving them on a pre-answer value would silently restore it on the
    // next keystroke.
    live_conversion_preedit_output_ = command->output().preedit();
    std::string unused_key;
    ExtractPreeditKeyAndValue(command->output().preedit(), &unused_key,
                              &live_conversion_value_);
  }
  AttachPendingAsyncCallback(command);
  return true;
}

void Session::TraceStart() {
  if (!TraceLogger::Enabled()) return;
  trace_seq_ = TraceLogger::NextSequenceId();
  trace_first_block_ = true;
  trace_sequence_start_ = Clock::GetAbslTime();
  trace_block_start_ = trace_sequence_start_;
  trace_mark_ = trace_sequence_start_;
  trace_steps_.clear();
}

void Session::TraceContinue(uint64_t seq, absl::Time sequence_start) {
  if (!TraceLogger::Enabled() || seq == 0) return;
  trace_seq_ = seq;
  trace_sequence_start_ = sequence_start;
  trace_block_start_ = trace_mark_ = Clock::GetAbslTime();
  trace_steps_.clear();
}

void Session::TraceStep(absl::string_view name, const std::string& detail) {
  if (!TraceLogger::Enabled() || trace_seq_ == 0) return;
  const absl::Time now = Clock::GetAbslTime();
  const double delta_ms = absl::ToDoubleMilliseconds(now - trace_mark_);
  trace_mark_ = now;
  trace_steps_.push_back(absl::StrCat(
      "  ├─ [", name, "] time=", TraceLogger::FormatMillis(delta_ms),
      "ms | ", detail));
}

void Session::TraceFlush(const std::string& detail) {
  if (!TraceLogger::Enabled() || trace_seq_ == 0) return;
  const absl::Time now = Clock::GetAbslTime();
  const double offset_ms =
      absl::ToDoubleMilliseconds(trace_block_start_ - trace_sequence_start_);
  const double total_ms = absl::ToDoubleMilliseconds(now - trace_block_start_);
  TraceLogger::WriteBlock(trace_seq_,
                          trace_first_block_ ? -1.0 : offset_ms, total_ms,
                          detail, trace_steps_);
  trace_first_block_ = false;
  trace_block_start_ = now;
  trace_mark_ = now;
  trace_steps_.clear();
  trace_seq_ = 0;  // A delayed callback reopens via TraceContinue().
}

bool Session::Suggest(const commands::Input& input) {
  composition_holds_passive_suggestion_ = false;
  composition_holds_passive_suggestion_query_.clear();

  if (SuppressSuggestion(input)) {
    return false;
  }

  if (ShouldSuppressShiftedAsciiAutoSuggestion(context_->GetConfig(),
                                               context_->composer())) {
    live_conversion_suggestion_candidate_window_.Clear();
    pending_live_conversion_suggestion_candidate_window_.Clear();
    live_conversion_suggestion_context_.reset();
    pending_live_conversion_suggestion_context_.reset();
    live_conversion_suggestion_query_.clear();
    pending_live_conversion_suggestion_query_.clear();
    return false;
  }

  // |request_suggestion| is not supposed to always ensure suppressing
  // suggestion since this field is used for performance improvement
  // by skipping interim suggestions.  However, the implementation of
  // EngineConverter::Suggest does not perform suggest
  // whenever this flag is on.  So the caller should consider whether
  // this flag should be set or not.  Because the original logic was
  // implemented in Session::InserCharacter, we check the input.type()
  // is SEND_KEY assuming SEND_KEY results InsertCharacter (in most
  // cases).
  //
  // TODO(komatsu): Move the logic into EngineConverter.
  if (input.has_request_suggestion() &&
      input.type() == commands::Input::SEND_KEY) {
    ConversionPreferences conversion_preferences =
        context_->converter().conversion_preferences();
    conversion_preferences.request_suggestion = input.request_suggestion();
    return context_->mutable_converter()->Suggest(
        context_->composer(), input.context(), conversion_preferences);
  }

  return context_->mutable_converter()->Suggest(context_->composer(),
                                                input.context());
}

bool Session::CaptureVisiblePassiveSuggestionSnapshot(
    PassiveSuggestionSnapshot* snapshot) const {
  DCHECK(snapshot);

  snapshot->candidate_window.Clear();
  snapshot->context.reset();
  snapshot->suggestion_query.clear();

  auto capture_live_snapshot =
      [&](const commands::CandidateWindow& candidate_window,
          const ImeContext* suggestion_context,
          const std::string& suggestion_query) {
        if (!IsPassiveLiveSuggestionCandidateWindow(candidate_window) ||
            suggestion_context == nullptr || suggestion_query.empty() ||
            suggestion_query !=
                context_->composer().GetQueryForPrediction()) {
          return false;
        }

        commands::Output suggestion_output;
        suggestion_context->converter().FillOutput(
            suggestion_context->composer(), &suggestion_output);
        if (!suggestion_output.has_candidate_window() ||
            !HaveSamePassiveSuggestionCandidates(
                candidate_window, suggestion_output.candidate_window())) {
          return false;
        }

        snapshot->candidate_window = candidate_window;
        snapshot->context =
            std::make_unique<ImeContext>(*suggestion_context);
        snapshot->suggestion_query = suggestion_query;
        return true;
      };

  if (live_conversion_active_ &&
      capture_live_snapshot(live_conversion_suggestion_candidate_window_,
                            live_conversion_suggestion_context_.get(),
                            live_conversion_suggestion_query_)) {
    return true;
  }

  if (live_conversion_pending_ &&
      capture_live_snapshot(
          pending_live_conversion_suggestion_candidate_window_,
          pending_live_conversion_suggestion_context_.get(),
          pending_live_conversion_suggestion_query_)) {
    return true;
  }

  commands::Output current_output;
  context_->converter().FillOutput(context_->composer(), &current_output);
  if (!current_output.has_candidate_window() ||
      !IsPassiveLiveSuggestionCandidateWindow(
          current_output.candidate_window())) {
    return false;
  }

  snapshot->candidate_window = current_output.candidate_window();
  snapshot->context = std::make_unique<ImeContext>(*context_);
  // The real converter state produced this list for the query on screen now.
  snapshot->suggestion_query =
      context_->composer().GetQueryForPrediction();
  return true;
}

bool Session::AttachPassiveSuggestionSnapshot(
    const commands::Input& input,
    PassiveSuggestionSnapshot* snapshot,
    commands::Output* output) {
  DCHECK(snapshot);
  DCHECK(output);

  output->clear_candidate_window();

  if (!IsPassiveLiveSuggestionCandidateWindow(snapshot->candidate_window) ||
      snapshot->context == nullptr) {
    return false;
  }

  if (SuppressSuggestion(input) ||
      ShouldSuppressShiftedAsciiAutoSuggestion(
          context_->GetConfig(), context_->composer())) {
    snapshot->candidate_window.Clear();
    snapshot->context.reset();
    return false;
  }

  // The converter candidate state remains the exact state that produced the
  // displayed IDs. Only Composer follows the user's newer unresolved Roman
  // input. This lets FillOutput and candidate submission use the current
  // composition without running prediction again.
  //
  // The provenance of the candidate IDs is therefore the query recorded in
  // |snapshot->suggestion_query|, not the Composer that is overwritten here.
  // FocusHeldPassiveSuggestion() compares that recorded query against the live
  // Composer before promoting this list.
  *snapshot->context->mutable_composer() = context_->composer();
  snapshot->context->set_state(ImeContext::COMPOSITION);

  commands::Output suggestion_output;
  snapshot->context->converter().FillOutput(
      snapshot->context->composer(), &suggestion_output);
  if (!suggestion_output.has_candidate_window() ||
      !HaveSamePassiveSuggestionCandidates(
          snapshot->candidate_window,
          suggestion_output.candidate_window())) {
    snapshot->candidate_window.Clear();
    snapshot->context.reset();
    return false;
  }

  *output->mutable_candidate_window() = snapshot->candidate_window;
  return true;
}

bool Session::FocusHeldPassiveSuggestion(commands::Command* command) {
  DCHECK(command);

  // Ordinary (non-live) HOLD keeps the real converter in SUGGESTION state.
  // Promote that exact list directly instead of calling Predict() with the
  // newer pending-Roman composer.
  if (composition_holds_passive_suggestion_ &&
      context_->state() == ImeContext::COMPOSITION &&
      context_->converter().CheckState(EngineConverterInterface::SUGGESTION)) {
    // The held list is only valid for the query that produced it.  If the user
    // typed on ("で" -> "できます"), promoting it would replace the visible
    // reading with candidates for the older prefix.
    const bool query_matches =
        !composition_holds_passive_suggestion_query_.empty() &&
        composition_holds_passive_suggestion_query_ ==
            context_->composer().GetQueryForPrediction();
    commands::Output current_output;
    context_->converter().FillOutput(context_->composer(), &current_output);
    if (query_matches && current_output.has_candidate_window() &&
        IsPassiveLiveSuggestionCandidateWindow(
            current_output.candidate_window()) &&
        current_output.candidate_window().candidate_size() > 0 &&
        current_output.candidate_window().candidate(0).has_id()) {
      const int first_id =
          current_output.candidate_window().candidate(0).id();
      if (context_->mutable_converter()
              ->FocusSuggestionCandidateWithoutPrediction(first_id)) {
        composition_holds_passive_suggestion_ = false;
        composition_holds_passive_suggestion_query_.clear();
        command->mutable_output()->set_consumed(true);
        SetSessionState(ImeContext::CONVERSION, context_.get());
        Output(command);
        return true;
      }
    }

    // If the held state is no longer internally serviceable, fail open to the
    // ordinary prediction path instead of repeatedly treating it as held.
    // The converter still holds the older query's suggestion segments, so
    // Cancel() it: otherwise Predict() would prepend those stale candidates
    // (previous_suggestions_) instead of predicting for the current reading.
    composition_holds_passive_suggestion_ = false;
    composition_holds_passive_suggestion_query_.clear();
    if (context_->converter().CheckState(EngineConverterInterface::SUGGESTION)) {
      context_->mutable_converter()->Cancel();
    }
  }

  const commands::CandidateWindow* candidate_window = nullptr;
  const ImeContext* suggestion_context = nullptr;
  const std::string* suggestion_query = nullptr;

  if (live_conversion_active_ &&
      live_conversion_holds_passive_suggestion_ &&
      IsPassiveLiveSuggestionCandidateWindow(
          live_conversion_suggestion_candidate_window_)) {
    candidate_window = &live_conversion_suggestion_candidate_window_;
    suggestion_context = live_conversion_suggestion_context_.get();
    suggestion_query = &live_conversion_suggestion_query_;
  } else if (
      live_conversion_pending_ &&
      pending_live_conversion_holds_passive_suggestion_ &&
      IsPassiveLiveSuggestionCandidateWindow(
          pending_live_conversion_suggestion_candidate_window_)) {
    candidate_window =
        &pending_live_conversion_suggestion_candidate_window_;
    suggestion_context =
        pending_live_conversion_suggestion_context_.get();
    suggestion_query = &pending_live_conversion_suggestion_query_;
  }

  if (candidate_window == nullptr || suggestion_context == nullptr ||
      suggestion_query == nullptr || candidate_window->candidate_size() == 0 ||
      !candidate_window->candidate(0).has_id() ||
      !suggestion_context->converter().CheckState(
          EngineConverterInterface::SUGGESTION) ||
      suggestion_query->empty() ||
      *suggestion_query != context_->composer().GetQueryForPrediction()) {
    return false;
  }

  commands::Output snapshot_output;
  suggestion_context->converter().FillOutput(
      suggestion_context->composer(), &snapshot_output);
  if (!snapshot_output.has_candidate_window() ||
      !HaveSamePassiveSuggestionCandidates(
          *candidate_window, snapshot_output.candidate_window())) {
    return false;
  }

  const int first_id = candidate_window->candidate(0).id();
  auto focused_context = std::make_unique<ImeContext>(*suggestion_context);
  if (!focused_context->mutable_converter()
           ->FocusSuggestionCandidateWithoutPrediction(first_id)) {
    return false;
  }

  // AttachPassiveSuggestionSnapshot() has already advanced the snapshot
  // Composer to the user's current composition, including the unresolved Roman
  // suffix.  Therefore Cancel can still return to the current input rather than
  // rolling the Composer back to the older suggestion query.
  command->mutable_output()->set_consumed(true);
  composition_holds_passive_suggestion_ = false;
  composition_holds_passive_suggestion_query_.clear();
  ClearLiveConversionState();
  context_ = std::move(focused_context);
  SetSessionState(ImeContext::CONVERSION, context_.get());
  Output(command);
  return true;
}

bool Session::AttachLiveConversionSuggestionCandidateWindow(
    const commands::Input& input, commands::Output* output) {
  DCHECK(output);

  // Output() from live conversion may contain the real CONVERSION candidate
  // window.  Only the passive SUGGESTION window built below is allowed to
  // remain on live-conversion output; otherwise the renderer would show the
  // ordinary conversion candidate list without an explicit Space/Down action.
  live_conversion_suggestion_candidate_window_.Clear();
  live_conversion_suggestion_context_.reset();
  live_conversion_suggestion_query_.clear();
  output->clear_candidate_window();

  if (SuppressSuggestion(input)) {
    return false;
  }

  if (ShouldSuppressShiftedAsciiAutoSuggestion(context_->GetConfig(),
                                               context_->composer())) {
    live_conversion_suggestion_candidate_window_.Clear();
    pending_live_conversion_suggestion_candidate_window_.Clear();
    live_conversion_suggestion_context_.reset();
    pending_live_conversion_suggestion_context_.reset();
    live_conversion_suggestion_query_.clear();
    pending_live_conversion_suggestion_query_.clear();
    return false;
  }

  // Live conversion intentionally keeps the real converter in CONVERSION state
  // so that Space/Down can enter the normal conversion candidate list with a
  // single key press.  EngineConverter::Suggest(), however, is defined only for
  // COMPOSITION/SUGGESTION states and resets converter state while generating
  // candidates.  Build the passive suggestion window on a cloned context and
  // copy only candidate_window to the live-conversion output.  The real
  // converter state, live_conversion_active_, and selected segment are left
  // untouched.
  auto suggestion_context = std::make_unique<ImeContext>(*context_);
  if (suggestion_context->converter().IsActive()) {
    suggestion_context->mutable_converter()->Cancel();
  }
  suggestion_context->set_state(ImeContext::COMPOSITION);

  bool has_suggestion = false;
  if (input.has_request_suggestion() &&
      input.type() == commands::Input::SEND_KEY) {
    ConversionPreferences conversion_preferences =
        suggestion_context->converter().conversion_preferences();
    conversion_preferences.request_suggestion = input.request_suggestion();
    has_suggestion =
        suggestion_context->mutable_converter()->SuggestWithPreferences(
            suggestion_context->composer(), input.context(),
            conversion_preferences);
  } else {
    has_suggestion = suggestion_context->mutable_converter()->Suggest(
        suggestion_context->composer(), input.context());
  }

  if (!has_suggestion) {
    return false;
  }

  commands::Output suggestion_output;
  suggestion_context->mutable_converter()->PopOutput(
      suggestion_context->composer(), &suggestion_output);

  if (!suggestion_output.has_candidate_window() ||
      suggestion_output.candidate_window().candidate_size() == 0) {
    return false;
  }

  live_conversion_suggestion_candidate_window_ =
      suggestion_output.candidate_window();
  live_conversion_suggestion_context_ = std::move(suggestion_context);
  // The clone's Composer is the one the suggestion was generated for.
  live_conversion_suggestion_query_ =
      live_conversion_suggestion_context_->composer()
          .GetQueryForPrediction();
  *output->mutable_candidate_window() =
      live_conversion_suggestion_candidate_window_;
  return true;
}

bool Session::AttachCachedLiveConversionSuggestionCandidateWindow(
    commands::Output* output) {
  DCHECK(output);

  output->clear_candidate_window();

  if (ShouldSuppressShiftedAsciiAutoSuggestion(context_->GetConfig(),
                                               context_->composer())) {
    live_conversion_suggestion_candidate_window_.Clear();
    pending_live_conversion_suggestion_candidate_window_.Clear();
    live_conversion_suggestion_context_.reset();
    pending_live_conversion_suggestion_context_.reset();
    live_conversion_suggestion_query_.clear();
    pending_live_conversion_suggestion_query_.clear();
    return false;
  }

  const commands::CandidateWindow* candidate_window = nullptr;
  ImeContext* suggestion_context = nullptr;
  const std::string* suggestion_query = nullptr;

  if (live_conversion_active_ &&
      IsPassiveLiveSuggestionCandidateWindow(
          live_conversion_suggestion_candidate_window_)) {
    candidate_window = &live_conversion_suggestion_candidate_window_;
    suggestion_context = live_conversion_suggestion_context_.get();
    suggestion_query = &live_conversion_suggestion_query_;
  } else if (
      live_conversion_pending_ &&
      IsPassiveLiveSuggestionCandidateWindow(
          pending_live_conversion_suggestion_candidate_window_)) {
    candidate_window =
        &pending_live_conversion_suggestion_candidate_window_;
    suggestion_context =
        pending_live_conversion_suggestion_context_.get();
    suggestion_query = &pending_live_conversion_suggestion_query_;
  }

  if (candidate_window == nullptr) {
    return false;
  }

  if (suggestion_context == nullptr || suggestion_query == nullptr ||
      suggestion_query->empty() ||
      *suggestion_query != context_->composer().GetQueryForPrediction()) {
    if (live_conversion_active_) {
      live_conversion_suggestion_candidate_window_.Clear();
      live_conversion_suggestion_context_.reset();
      live_conversion_suggestion_query_.clear();
    }
    if (live_conversion_pending_) {
      pending_live_conversion_suggestion_candidate_window_.Clear();
      pending_live_conversion_suggestion_context_.reset();
      pending_live_conversion_suggestion_query_.clear();
    }
    return false;
  }

  commands::Output suggestion_snapshot;
  suggestion_context->converter().FillOutput(
      suggestion_context->composer(), &suggestion_snapshot);
  if (!suggestion_snapshot.has_candidate_window() ||
      !HaveSamePassiveSuggestionCandidates(
          *candidate_window, suggestion_snapshot.candidate_window())) {
    if (live_conversion_active_) {
      live_conversion_suggestion_candidate_window_.Clear();
      live_conversion_suggestion_context_.reset();
      live_conversion_suggestion_query_.clear();
    }
    if (live_conversion_pending_) {
      pending_live_conversion_suggestion_candidate_window_.Clear();
      pending_live_conversion_suggestion_context_.reset();
      pending_live_conversion_suggestion_query_.clear();
    }
    return false;
  }

  *output->mutable_candidate_window() = *candidate_window;
  return true;
}

bool Session::ConvertToTransliteration(
    commands::Command* command,
    const transliteration::TransliterationType type) {
  if (!(context_->state() &
        (ImeContext::CONVERSION | ImeContext::COMPOSITION))) {
    return DoNothing(command);
  }
  command->mutable_output()->set_consumed(true);

  if (!context_->mutable_converter()->ConvertToTransliteration(
          context_->composer(), type)) {
    return false;
  }
  SetSessionState(ImeContext::CONVERSION, context_.get());
  Output(command);
  return true;
}

bool Session::ConvertToHiragana(commands::Command* command) {
  return ConvertToTransliteration(command, transliteration::HIRAGANA);
}

bool Session::ConvertToFullKatakana(commands::Command* command) {
  return ConvertToTransliteration(command, transliteration::FULL_KATAKANA);
}

bool Session::ConvertToHalfKatakana(commands::Command* command) {
  return ConvertToTransliteration(command, transliteration::HALF_KATAKANA);
}

bool Session::ConvertToFullASCII(commands::Command* command) {
  return ConvertToTransliteration(command, transliteration::FULL_ASCII);
}

bool Session::ConvertToHalfASCII(commands::Command* command) {
  return ConvertToTransliteration(command, transliteration::HALF_ASCII);
}

bool Session::SwitchKanaType(commands::Command* command) {
  if (!(context_->state() &
        (ImeContext::CONVERSION | ImeContext::COMPOSITION))) {
    return DoNothing(command);
  }
  command->mutable_output()->set_consumed(true);

  if (!context_->mutable_converter()->SwitchKanaType(context_->composer())) {
    return false;
  }
  SetSessionState(ImeContext::CONVERSION, context_.get());
  Output(command);
  return true;
}

bool Session::DisplayAsHiragana(commands::Command* command) {
  command->mutable_output()->set_consumed(true);
  if (context_->state() == ImeContext::CONVERSION) {
    return ConvertToHiragana(command);
  } else {  // context_->state() == ImeContext::COMPOSITION
    context_->mutable_composer()->SetOutputMode(transliteration::HIRAGANA);
    OutputComposition(command);
    return true;
  }
}

bool Session::DisplayAsFullKatakana(commands::Command* command) {
  command->mutable_output()->set_consumed(true);
  if (context_->state() == ImeContext::CONVERSION) {
    return ConvertToFullKatakana(command);
  } else {  // context_->state() == ImeContext::COMPOSITION
    context_->mutable_composer()->SetOutputMode(transliteration::FULL_KATAKANA);
    OutputComposition(command);
    return true;
  }
}

bool Session::DisplayAsHalfKatakana(commands::Command* command) {
  command->mutable_output()->set_consumed(true);
  if (context_->state() == ImeContext::CONVERSION) {
    return ConvertToHalfKatakana(command);
  } else {  // context_->state() == ImeContext::COMPOSITION
    context_->mutable_composer()->SetOutputMode(transliteration::HALF_KATAKANA);
    OutputComposition(command);
    return true;
  }
}

bool Session::TranslateFullASCII(commands::Command* command) {
  command->mutable_output()->set_consumed(true);
  if (context_->state() == ImeContext::CONVERSION) {
    return ConvertToFullASCII(command);
  } else {  // context_->state() == ImeContext::COMPOSITION
    context_->mutable_composer()->SetOutputMode(
        transliteration::T13n::ToggleFullAsciiTypes(
            context_->composer().GetOutputMode()));
    OutputComposition(command);
    return true;
  }
}

bool Session::TranslateHalfASCII(commands::Command* command) {
  command->mutable_output()->set_consumed(true);
  if (context_->state() == ImeContext::CONVERSION) {
    return ConvertToHalfASCII(command);
  } else {  // context_->state() == ImeContext::COMPOSITION
    context_->mutable_composer()->SetOutputMode(
        transliteration::T13n::ToggleHalfAsciiTypes(
            context_->composer().GetOutputMode()));
    OutputComposition(command);
    return true;
  }
}

bool Session::CompositionModeHiragana(commands::Command* command) {
  command->mutable_output()->set_consumed(true);
  EnsureIMEIsOn();
  // The temporary mode should not be overridden.
  SwitchInputMode(transliteration::HIRAGANA, context_->mutable_composer());
  OutputFromState(command);
  return true;
}

bool Session::CompositionModeFullKatakana(commands::Command* command) {
  command->mutable_output()->set_consumed(true);
  EnsureIMEIsOn();
  // The temporary mode should not be overridden.
  SwitchInputMode(transliteration::FULL_KATAKANA, context_->mutable_composer());
  OutputFromState(command);
  return true;
}

bool Session::CompositionModeHalfKatakana(commands::Command* command) {
  command->mutable_output()->set_consumed(true);
  EnsureIMEIsOn();
  // The temporary mode should not be overridden.
  SwitchInputMode(transliteration::HALF_KATAKANA, context_->mutable_composer());
  OutputFromState(command);
  return true;
}

bool Session::CompositionModeFullASCII(commands::Command* command) {
  command->mutable_output()->set_consumed(true);
  EnsureIMEIsOn();
  // The temporary mode should not be overridden.
  SwitchInputMode(transliteration::FULL_ASCII, context_->mutable_composer());
  OutputFromState(command);
  return true;
}

bool Session::CompositionModeHalfASCII(commands::Command* command) {
  command->mutable_output()->set_consumed(true);
  EnsureIMEIsOn();
  // The temporary mode should not be overridden.
  SwitchInputMode(transliteration::HALF_ASCII, context_->mutable_composer());
  OutputFromState(command);
  return true;
}

bool Session::CompositionModeSwitchKanaType(commands::Command* command) {
  if (context_->state() != ImeContext::PRECOMPOSITION) {
    return DoNothing(command);
  }

  command->mutable_output()->set_consumed(true);

  transliteration::TransliterationType current_type =
      context_->composer().GetInputMode();
  transliteration::TransliterationType next_type;

  switch (current_type) {
    case transliteration::HIRAGANA:
      next_type = transliteration::FULL_KATAKANA;
      break;

    case transliteration::FULL_KATAKANA:
      next_type = transliteration::HALF_KATAKANA;
      break;

    case transliteration::HALF_KATAKANA:
      next_type = transliteration::HIRAGANA;
      break;

    case transliteration::HALF_ASCII:
    case transliteration::FULL_ASCII:
      next_type = current_type;
      break;

    default:
      LOG(ERROR) << "Unknown input mode: " << current_type;
      // don't change input mode
      next_type = current_type;
      break;
  }

  // The temporary mode should not be overridden.
  SwitchInputMode(next_type, context_->mutable_composer());
  OutputFromState(command);
  return true;
}

bool Session::ConvertToHalfWidth(commands::Command* command) {
  if (!(context_->state() &
        (ImeContext::CONVERSION | ImeContext::COMPOSITION))) {
    return DoNothing(command);
  }
  command->mutable_output()->set_consumed(true);

  if (!context_->mutable_converter()->ConvertToHalfWidth(
          context_->composer())) {
    return false;
  }
  SetSessionState(ImeContext::CONVERSION, context_.get());
  Output(command);
  return true;
}

bool Session::TranslateHalfWidth(commands::Command* command) {
  command->mutable_output()->set_consumed(true);
  if (context_->state() == ImeContext::CONVERSION) {
    return ConvertToHalfWidth(command);
  } else {  // context_->state() == ImeContext::COMPOSITION
    const transliteration::TransliterationType type =
        context_->composer().GetOutputMode();
    if (type == transliteration::HIRAGANA ||
        type == transliteration::FULL_KATAKANA ||
        type == transliteration::HALF_KATAKANA) {
      context_->mutable_composer()->SetOutputMode(
          transliteration::HALF_KATAKANA);
    } else if (type == transliteration::FULL_ASCII) {
      context_->mutable_composer()->SetOutputMode(transliteration::HALF_ASCII);
    } else if (type == transliteration::FULL_ASCII_UPPER) {
      context_->mutable_composer()->SetOutputMode(
          transliteration::HALF_ASCII_UPPER);
    } else if (type == transliteration::FULL_ASCII_LOWER) {
      context_->mutable_composer()->SetOutputMode(
          transliteration::HALF_ASCII_LOWER);
    } else if (type == transliteration::FULL_ASCII_CAPITALIZED) {
      context_->mutable_composer()->SetOutputMode(
          transliteration::HALF_ASCII_CAPITALIZED);
    } else {
      // transliteration::HALF_ASCII_something
      return TranslateHalfASCII(command);
    }
    OutputComposition(command);
    return true;
  }
}

bool Session::LaunchConfigDialog(commands::Command* command) {
  command->mutable_output()->set_launch_tool_mode(
      commands::Output::CONFIG_DIALOG);
  return DoNothing(command);
}

bool Session::LaunchDictionaryTool(commands::Command* command) {
  command->mutable_output()->set_launch_tool_mode(
      commands::Output::DICTIONARY_TOOL);
  return DoNothing(command);
}

bool Session::LaunchWordRegisterDialog(commands::Command* command) {
  command->mutable_output()->set_launch_tool_mode(
      commands::Output::WORD_REGISTER_DIALOG);
  return DoNothing(command);
}

bool Session::UndoOrRewind(commands::Command* command) {
  // Undo is prioritized over rewind otherwise the undo operation for
  // partial commit doesn't work (rewind always consumes the event).
  if (HasUndoContext()) {
    return Undo(command);
  }

  // Rewind if the state is in composition.
  if (!(context_->state() & ImeContext::COMPOSITION)) {
    // Mozc decoder doesn't do anything for UNDO_OR_REWIND.
    // Echo back the event to the client to give it a chance to delegate
    // undo operation to the app.
    return EchoBack(command);
  }

  command->mutable_output()->set_consumed(true);
  context_->mutable_composer()->InsertCommandCharacter(
      composer::Composer::REWIND);
  ClearUndoContext();

  // InsertCommandCharacter method updates the preedit text
  // so we need to update suggest candidates.
  if (Suggest(command->input())) {
    Output(command);
    return true;
  }
  OutputComposition(command);
  return true;
}

bool Session::StopKeyToggling(commands::Command* command) {
  if (!(context_->state() & ImeContext::COMPOSITION)) {
    return DoNothing(command);
  }

  command->mutable_output()->set_consumed(true);
  context_->mutable_composer()->InsertCommandCharacter(
      composer::Composer::STOP_KEY_TOGGLING);
  ClearUndoContext();

  // Since the output should not be changed on STOP_KEY_TOGGLING,
  // The last output is used instead of calling the converter operations.
  Output(command);
  return true;
}

bool Session::ToggleAlphanumericMode(commands::Command* command) {
  command->mutable_output()->set_consumed(true);
  context_->mutable_composer()->ToggleInputMode();

  OutputFromState(command);
  return true;
}

bool Session::DeleteCandidateFromHistory(commands::Command* command) {
  std::optional<int> id = std::nullopt;
  if (command->input().has_command() && command->input().command().has_id()) {
    id = command->input().command().id();
  }
  if (!context_->mutable_converter()->DeleteCandidateFromHistory(id)) {
    return DoNothing(command);
  }
  return ConvertCancel(command);
}

bool Session::DeleteSelectedCandidate(commands::Command* command) {
  std::optional<int> id = std::nullopt;
  if (command->input().has_command() && command->input().command().has_id()) {
    id = command->input().command().id();
  }
  if (!context_->mutable_converter()->DeleteOrSuppressCandidate(id)) {
    return DoNothing(command);
  }
  return ConvertCancel(command);
}

bool Session::Convert(commands::Command* command) {
  CancelPendingLiveConversion();
  command->mutable_output()->set_consumed(true);
  const std::string composition = context_->composer().GetQueryForConversion();
  const bool should_show_candidate_window_on_initial_conversion =
      context_->state() == ImeContext::COMPOSITION &&
      context_->GetConfig().show_candidate_window_on_initial_conversion() &&
      !context_->GetConfig().use_live_conversion();

  // Space in the middle of an English phrase types a space instead of
  // starting conversion.  Mozc's classical keymap binds Composition Space to
  // Convert, which fires on the space in "anoGithub Cliwotsukatte" and
  // converts the English run away mid-phrase.  Meltype already knows the
  // composition ends in an English piece there, so the space is what the user
  // meant: the separator between English words.  A Japanese composition
  // (kyouha) keeps Convert, and so does a Space typed right after a space
  // (the trailing whitespace is its own piece, so the second Space converts
  // what was typed) — pressing Space twice is the escape hatch.
  //
  // The space is half-width on purpose, matching the ASCII-composition branch
  // below: this is an English phrase, and a full-width space would be wrong
  // in it.  Only a plain Space qualifies; Shift+Space (InsertAlternateSpace)
  // is an explicit request for the alternate width and stays untouched.
  if (context_->state() == ImeContext::COMPOSITION &&
      IsPureSpaceKey(command->input().key()) &&
      CompositionEndsWithEnglishPiece()) {
    command->mutable_input()->mutable_key()->set_key_code(' ');
    return InsertCharacter(command);
  }

  // TODO(komatsu): Make a function like ConvertOrSpace.
  // Handle a space key on the ASCII composition mode.
  if (context_->state() == ImeContext::COMPOSITION &&
      (context_->composer().GetInputMode() == transliteration::HALF_ASCII ||
       context_->composer().GetInputMode() == transliteration::FULL_ASCII) &&
      command->input().key().has_special_key() &&
      command->input().key().special_key() == commands::KeyEvent::SPACE) {
    // TODO(komatsu): Consider FullWidth Space too.
    if (!composition.ends_with(' ') ||
        context_->composer().GetLength() != context_->composer().GetCursor()) {
      if (context_->GetRequest().space_on_alphanumeric() ==
          commands::Request::COMMIT) {
        // Space is committed with the composition
        context_->mutable_composer()->InsertCharacterPreedit(" ");
        // Don't push the context to the undo context here.
        // It'll be done in Commit() below.
        return Commit(command);
      } else {
        // SPACE_OR_CONVERT_KEEPING_COMPOSITION or
        // SPACE_OR_CONVERT_COMMITTING_COMPOSITION.

        // If the last character is not space, space is inserted to the
        // composition.
        command->mutable_input()->mutable_key()->set_key_code(' ');
        return InsertCharacter(command);
      }
    }

    if (!composition.empty()) {
      DCHECK_EQ(' ', composition[composition.size() - 1]);
      // Delete the last space.
      context_->mutable_composer()->Backspace();
      ClearUndoContext();
    }
  }

  if (!context_->mutable_converter()->Convert(context_->composer(),
                                              context_->client_context())) {
    LOG(ERROR) << "Conversion failed for some reasons.";
    OutputComposition(command);
    return true;
  }

  SetSessionState(ImeContext::CONVERSION, context_.get());
  if (should_show_candidate_window_on_initial_conversion) {
    context_->mutable_converter()->SetCandidateListVisible(true);
  }
  Output(command);
  return true;
}

bool Session::ConvertWithoutHistory(commands::Command* command) {
  CancelPendingLiveConversion();
  command->mutable_output()->set_consumed(true);

  ConversionPreferences preferences =
      context_->converter().conversion_preferences();
  preferences.use_history = false;
  if (!context_->mutable_converter()->Convert(
          context_->composer(), context_->client_context(), preferences)) {
    LOG(ERROR) << "Conversion failed for some reasons.";
    OutputComposition(command);
    return true;
  }

  SetSessionState(ImeContext::CONVERSION, context_.get());
  Output(command);
  return true;
}

bool Session::CommitIfPassword(commands::Command* command) {
  if (context_->composer().GetInputFieldType() == commands::Context::PASSWORD) {
    CommitCompositionDirectly(command);
    return true;
  }
  return false;
}

bool Session::MoveCursorRight(commands::Command* command) {
  // In future, we may want to change the strategy of committing, to support
  // more flexible behavior.
  // - If the composing text has some "pending toggling character(s) at the
  //   end", we'd like to "fix" the toggling state, but not to commit.
  // - Otherwise (i.e. if there is no such character(s)), we'd like to commit
  //   (considering the use cases, probably we'd like to apply it only for
  //   alphabet mode).
  // Before supporting it, we'll need to support auto fixing by waiting
  // a period. Also, it is necessary to support displaying the current toggling
  // state (otherwise, users would be confused).
  // So, to keep users out from such confusion, we only commit if the current
  // composing mode doesn't has toggling state. Clients has the responsibility
  // to check if the keyboard has toggling state or not. Note that the server
  // should know the current table has toggling state or not. However,
  // a client may NOT want to auto committing even if the composition mode
  // doesn't have the toggling state, so the server just relies on the flag
  // passed from the client.
  // TODO(hidehiko): Support it, when it is prioritized.
  if (context_->GetRequest().crossing_edge_behavior() ==
          commands::Request::COMMIT_WITHOUT_CONSUMING &&
      context_->composer().GetLength() == context_->composer().GetCursor()) {
    Commit(command);

    // Do not consume.
    command->mutable_output()->set_consumed(false);
    return true;
  }

  command->mutable_output()->set_consumed(true);
  if (CommitIfPassword(command)) {
    return true;
  }
  context_->mutable_composer()->MoveCursorRight();
  ClearUndoContext();
  if (Suggest(command->input())) {
    Output(command);
    return true;
  }
  OutputComposition(command);
  return true;
}

bool Session::MoveCursorLeft(commands::Command* command) {
  if (context_->GetRequest().crossing_edge_behavior() ==
          commands::Request::COMMIT_WITHOUT_CONSUMING &&
      context_->composer().GetCursor() == 0) {
    CommitNotTriggeringZeroQuerySuggest(command);

    // Move the cursor to the beginning of the values.
    command->mutable_output()->mutable_result()->set_cursor_offset(
        -static_cast<int32_t>(
            Util::CharsLen(command->output().result().value())));

    // Do not consume.
    command->mutable_output()->set_consumed(false);
    return true;
  }

  command->mutable_output()->set_consumed(true);
  if (CommitIfPassword(command)) {
    return true;
  }
  context_->mutable_composer()->MoveCursorLeft();
  ClearUndoContext();
  if (Suggest(command->input())) {
    Output(command);
    return true;
  }
  OutputComposition(command);
  return true;
}

bool Session::MoveCursorToEnd(commands::Command* command) {
  return MoveCursorToEndInternal(command, true);
}

bool Session::MoveCursorToEndInternal(commands::Command* command,
                                      bool clear_undo) {
  command->mutable_output()->set_consumed(true);
  if (CommitIfPassword(command)) {
    return true;
  }
  context_->mutable_composer()->MoveCursorToEnd();
  if (clear_undo) {
    ClearUndoContext();
  }
  if (Suggest(command->input())) {
    Output(command);
    return true;
  }
  OutputComposition(command);
  return true;
}

bool Session::MoveCursorTo(commands::Command* command) {
  // This method moves the cursor *inside* the composition text.
  // Therefore on PRECOMPOSITION state, where there is no composition text,
  // this method shouldn't consume the event but send back it to the client.
  if (context_->state() == ImeContext::PRECOMPOSITION) {
    return EchoBack(command);
  }
  if (context_->state() != ImeContext::COMPOSITION) {
    return DoNothing(command);
  }
  command->mutable_output()->set_consumed(true);
  if (CommitIfPassword(command)) {
    return true;
  }
  context_->mutable_composer()->MoveCursorTo(
      command->input().command().cursor_position());
  ClearUndoContext();
  if (Suggest(command->input())) {
    Output(command);
    return true;
  }
  OutputComposition(command);
  return true;
}

bool Session::MoveCursorToBeginning(commands::Command* command) {
  command->mutable_output()->set_consumed(true);
  if (CommitIfPassword(command)) {
    return true;
  }
  context_->mutable_composer()->MoveCursorToBeginning();
  ClearUndoContext();
  if (Suggest(command->input())) {
    Output(command);
    return true;
  }
  OutputComposition(command);
  return true;
}

bool Session::Delete(commands::Command* command) {
  DiscardPendingDirectCommitLearning(
      "delete_after_direct_commit_learning");
  ClearPendingRerankedPreeditCommitAfterConvertCancel();

  command->mutable_output()->set_consumed(true);
  CancelLiveConversionForEditing();
  context_->mutable_composer()->Delete();
  ClearUndoContext();
  if (context_->mutable_composer()->Empty()) {
    SetStateToPredompositionAndCancel(context_.get());
    Output(command);
  } else if (MaybeStartLiveConversionAfterEditing(command)) {
    return true;
  } else if (Suggest(command->input())) {
    Output(command);
  } else {
    OutputComposition(command);
  }
  return true;
}

bool Session::Backspace(commands::Command* command) {
  DiscardPendingDirectCommitLearning(
      "backspace_after_direct_commit_learning");
  ClearPendingRerankedPreeditCommitAfterConvertCancel();

  command->mutable_output()->set_consumed(true);
  CancelLiveConversionForEditing();
  context_->mutable_composer()->Backspace();
  ClearUndoContext();
  if (context_->mutable_composer()->Empty()) {
    SetStateToPredompositionAndCancel(context_.get());
    Output(command);
  } else if (MaybeStartLiveConversionAfterEditing(command)) {
    return true;
  } else if (Suggest(command->input())) {
    Output(command);
  } else {
    OutputComposition(command);
  }
  return true;
}

bool Session::SegmentFocusRight(commands::Command* command) {
  if (!(context_->state() & (ImeContext::CONVERSION))) {
    return DoNothing(command);
  }
  command->mutable_output()->set_consumed(true);
  context_->mutable_converter()->SegmentFocusRight();
  Output(command);
  return true;
}

bool Session::SegmentFocusLast(commands::Command* command) {
  if (!(context_->state() & (ImeContext::CONVERSION))) {
    return DoNothing(command);
  }
  command->mutable_output()->set_consumed(true);
  context_->mutable_converter()->SegmentFocusLast();
  Output(command);
  return true;
}

bool Session::SegmentFocusLeft(commands::Command* command) {
  if (!(context_->state() & (ImeContext::CONVERSION))) {
    return DoNothing(command);
  }
  command->mutable_output()->set_consumed(true);
  context_->mutable_converter()->SegmentFocusLeft();
  Output(command);
  return true;
}

bool Session::SegmentFocusLeftEdge(commands::Command* command) {
  if (!(context_->state() & (ImeContext::CONVERSION))) {
    return DoNothing(command);
  }
  command->mutable_output()->set_consumed(true);
  context_->mutable_converter()->SegmentFocusLeftEdge();
  Output(command);
  return true;
}

bool Session::SegmentWidthExpand(commands::Command* command) {
  if (!(context_->state() & (ImeContext::CONVERSION))) {
    return DoNothing(command);
  }
  command->mutable_output()->set_consumed(true);
  context_->mutable_converter()->SegmentWidthExpand(context_->composer());
  Output(command);
  return true;
}

bool Session::SegmentWidthShrink(commands::Command* command) {
  if (!(context_->state() & (ImeContext::CONVERSION))) {
    return DoNothing(command);
  }
  command->mutable_output()->set_consumed(true);
  context_->mutable_converter()->SegmentWidthShrink(context_->composer());
  Output(command);
  return true;
}

bool Session::ReportBug(commands::Command* command) {
  return DoNothing(command);
}

bool Session::ConvertNext(commands::Command* command) {
  command->mutable_output()->set_consumed(true);
  context_->mutable_converter()->CandidateNext(context_->composer());
  Output(command);
  return true;
}

bool Session::ConvertNextPage(commands::Command* command) {
  if (!(context_->state() & (ImeContext::CONVERSION))) {
    return DoNothing(command);
  }
  command->mutable_output()->set_consumed(true);
  context_->mutable_converter()->CandidateNextPage();
  Output(command);
  return true;
}

bool Session::ConvertPrev(commands::Command* command) {
  command->mutable_output()->set_consumed(true);
  context_->mutable_converter()->CandidatePrev();
  Output(command);
  return true;
}

bool Session::ConvertPrevPage(commands::Command* command) {
  if (!(context_->state() & (ImeContext::CONVERSION))) {
    return DoNothing(command);
  }
  command->mutable_output()->set_consumed(true);
  context_->mutable_converter()->CandidatePrevPage();
  Output(command);
  return true;
}

bool Session::ConvertCancel(commands::Command* command) {
  DiscardPendingDirectCommitLearning(
      "convert_cancel_after_direct_commit_learning");

  command->mutable_output()->set_consumed(true);

  MaybeSetPendingRerankedPreeditCommitAfterConvertCancel();
  ClearLiveConversionState();

  SetSessionState(ImeContext::COMPOSITION, context_.get());
  context_->mutable_converter()->Cancel();
  if (Suggest(command->input())) {
    Output(command);
  } else {
    OutputComposition(command);
  }
  return true;
}

bool Session::PredictAndConvertFromLiveConversion(commands::Command* command) {
  DCHECK(command);

  if (FocusHeldPassiveSuggestion(command)) {
    return true;
  }

  CancelPendingLiveConversion();
  command->mutable_output()->set_consumed(true);

  if (context_->state() != ImeContext::CONVERSION || !live_conversion_active_) {
    return PredictAndConvert(command);
  }

  // Selecting prediction candidates is an explicit user override of the
  // currently visible live conversion result.

  // Passive suggestions shown during live conversion are generated from a cloned
  // context and are not the real converter's current suggestion state.  Reset the
  // real converter before entering prediction selection so stale previous
  // suggestions from an older prefix, e.g. "ふ" before "ふる", are not reused.
  ClearLiveConversionState();
  SetSessionState(ImeContext::COMPOSITION, context_.get());
  context_->mutable_converter()->Cancel();

  if (context_->mutable_converter()->Predict(context_->composer())) {
    SetSessionState(ImeContext::CONVERSION, context_.get());
    Output(command);
  } else {
    // EngineConverter::Predict() resets its internal state on a first-prediction
    // failure.  Keep Session and EngineConverter aligned and fall back to the
    // same composition output that ordinary PredictAndConvert() uses on failure.
    OutputComposition(command);
  }
  return true;
}

bool Session::PredictAndConvert(commands::Command* command) {
  if (FocusHeldPassiveSuggestion(command)) {
    return true;
  }

  CancelPendingLiveConversion();

  if (context_->state() == ImeContext::CONVERSION) {
    return ConvertNext(command);
  }

  command->mutable_output()->set_consumed(true);
  if (context_->mutable_converter()->Predict(context_->composer(),
                                             context_->client_context())) {
    SetSessionState(ImeContext::CONVERSION, context_.get());
    Output(command);
  } else {
    OutputComposition(command);
  }
  return true;
}

void Session::OutputFromState(commands::Command* command) {
  if (context_->state() == ImeContext::DIRECT) {
    OutputMode(command);
    return;
  }
  Output(command);
}

void Session::Output(commands::Command* command) {
  OutputMode(command);
  context_->mutable_converter()->PopOutput(context_->composer(),
                                           command->mutable_output());
  MaybeUpdateCandidateActionFooter(command->mutable_output());

  // Once live conversion materializes, EngineConverter::FillConversion()
  // rebuilds the preedit from converter segments and therefore no longer passes
  // through output::FillPreedit(), where pending roman metadata is normally
  // attached. Restore only the presentation metadata here; key/value text and
  // converter state remain unchanged.
  if (context_->state() == ImeContext::CONVERSION &&
      command->output().has_preedit()) {
    MarkPendingRomanTailOnPreedit(
        context_->composer(), command->mutable_output()->mutable_preedit());
  }

  command->mutable_output()->set_pre_live_conversion_reading(
      command->output().has_preedit() &&
      ShouldExposePreLiveConversionReading(*context_));

}

void Session::MaybeUpdateCandidateActionFooter(
    commands::Output* output) const {
  if (output == nullptr || !output->has_candidate_window()) {
    return;
  }

  commands::CandidateWindow* candidate_window =
      output->mutable_candidate_window();
  if (!candidate_window->has_focused_index()) {
    return;
  }

  const commands::CandidateWindow_Candidate* focused_candidate = nullptr;
  for (const commands::CandidateWindow_Candidate& candidate :
       candidate_window->candidate()) {
    if (candidate.index() == candidate_window->focused_index()) {
      focused_candidate = &candidate;
      break;
    }
  }
  if (focused_candidate == nullptr || !focused_candidate->has_annotation()) {
    return;
  }

  const commands::Annotation& annotation = focused_candidate->annotation();
  const bool deletable = annotation.deletable();
  const bool suppressible = annotation.suppressible();
  if (!deletable && !suppressible) {
    return;
  }

  const keymap::KeyMapManager& keymap = context_->GetKeyMapManager();
  constexpr keymap::ConversionState::Commands kCommand =
      keymap::ConversionState::DELETE_SELECTED_CANDIDATE;

  std::vector<std::string> bindings;
  switch (candidate_window->category()) {
    case commands::PREDICTION:
      bindings = keymap.GetKeyBindingsForPredictionCommand(kCommand);
      break;
    case commands::CONVERSION:
      bindings = keymap.GetKeyBindingsForConversionCommand(kCommand);
      break;
    default:
      bindings.clear();
      break;
  }

  commands::Footer* footer = candidate_window->mutable_footer();
  // Keep the compact action footer free of the legacy logo area.
  footer->set_logo_visible(false);
  if (bindings.empty()) {
    // EngineOutput historically emits a fixed Ctrl+Del instruction.  The
    // final Session output must not advertise an action that is unreachable
    // in the active keymap.
    footer->clear_label();
    return;
  }

  const std::string key_label =
      FormatCandidateActionKeyLabel(bindings.front());
  footer->set_label(
      absl::StrCat(key_label,
                   deletable ? "で履歴削除" : "で候補非表示"));
}

void Session::OutputMode(commands::Command* command) const {
  const commands::CompositionMode mode =
      ToCompositionMode(context_->composer().GetInputMode());
  const commands::CompositionMode comeback_mode =
      ToCompositionMode(context_->composer().GetComebackInputMode());

  commands::Output* output = command->mutable_output();
  commands::Status* status = output->mutable_status();
  if (context_->state() == ImeContext::DIRECT) {
    output->set_mode(commands::DIRECT);
    status->set_activated(false);
  } else {
    output->set_mode(mode);
    status->set_activated(true);
  }
  status->set_mode(mode);
  status->set_comeback_mode(comeback_mode);
}

void Session::OutputComposition(commands::Command* command) const {
  OutputMode(command);
  commands::Output* output = command->mutable_output();
  context_->converter().FillPreedit(
      context_->composer(), output->mutable_preedit());
  output->set_pre_live_conversion_reading(
      output->has_preedit() &&
      ShouldExposePreLiveConversionReading(*context_));
}

void Session::OutputKey(commands::Command* command) const {
  OutputMode(command);
  commands::KeyEvent* key = command->mutable_output()->mutable_key();
  *key = command->input().key();
}

namespace {

bool MatchesKeyEvent(const commands::KeyEvent& key_event,
                     const uint32_t key_code,
                     std::initializer_list<absl::string_view> key_strings) {
  if (key_event.key_code() == key_code && key_event.key_string().empty()) {
    return true;
  }

  for (const absl::string_view s : key_strings) {
    if (key_event.key_string() == s) {
      return true;
    }
  }
  return false;
}

bool MatchesString(absl::string_view value,
                   std::initializer_list<absl::string_view> candidates) {
  for (const absl::string_view s : candidates) {
    if (value == s) {
      return true;
    }
  }
  return false;
}

bool SymbolMethodUsesMiddleDot(const config::Config& config) {
  return config.symbol_method() == config::Config::CORNER_BRACKET_MIDDLE_DOT ||
         config.symbol_method() == config::Config::SQUARE_BRACKET_MIDDLE_DOT;
}

std::string DirectCommitFallbackSuffixFromKeyEvent(
    const config::Config& config, const commands::KeyEvent& key_event) {
  if (MatchesString(key_event.key_string(),
                    {"。", "｡", "．", "、", "､", "，", "？", "！",
                     "（", "）", "「", "」", "［", "］", "・", "･"})) {
    return key_event.key_string();
  }

  if (MatchesString(key_event.key_string(), {".", "．"})) {
    return "。";
  }
  if (MatchesString(key_event.key_string(), {",", "，"})) {
    return "、";
  }
  if (key_event.key_string() == "?") {
    return "？";
  }
  if (key_event.key_string() == "!") {
    return "！";
  }
  if (key_event.key_string() == "(") {
    return "（";
  }
  if (key_event.key_string() == ")") {
    return "）";
  }
  if (MatchesString(key_event.key_string(), {"[", "［"})) {
    return "「";
  }
  if (MatchesString(key_event.key_string(), {"]", "］"})) {
    return "」";
  }
  if (SymbolMethodUsesMiddleDot(config) &&
      MatchesString(key_event.key_string(), {"/", "／"})) {
    return "・";
  }

  switch (key_event.key_code()) {
    case static_cast<uint32_t>('.'):
      return "。";
    case static_cast<uint32_t>(','):
      return "、";
    case static_cast<uint32_t>('?'):
      return "？";
    case static_cast<uint32_t>('!'):
      return "！";
    case static_cast<uint32_t>('('):
      return "（";
    case static_cast<uint32_t>(')'):
      return "）";
    case static_cast<uint32_t>('['):
      return "「";
    case static_cast<uint32_t>(']'):
      return "」";
    case static_cast<uint32_t>('/'):
      if (SymbolMethodUsesMiddleDot(config)) {
        return "・";
      }
      break;
    default:
      break;
  }

  return "";
}

// Auto conversion helper.
// NOTE: This checks the last character in preedit, not key_event.key_string().
bool IsValidAutoConversionKey(const config::Config& config,
                              const uint32_t key_code,
                              absl::string_view last_char) {
  return (((key_code == static_cast<uint32_t>('.') && last_char.empty()) ||
           last_char == "." || last_char == "．" || last_char == "。" ||
           last_char == "｡") &&
          (config.auto_conversion_key() &
           config::Config::AUTO_CONVERSION_KUTEN)) ||
         (((key_code == static_cast<uint32_t>(',') && last_char.empty()) ||
           last_char == "," || last_char == "，" || last_char == "、" ||
           last_char == "､") &&
          (config.auto_conversion_key() &
           config::Config::AUTO_CONVERSION_TOUTEN)) ||
         (((key_code == static_cast<uint32_t>('?') && last_char.empty()) ||
           last_char == "?" || last_char == "？") &&
          (config.auto_conversion_key() &
           config::Config::AUTO_CONVERSION_QUESTION_MARK)) ||
         (((key_code == static_cast<uint32_t>('!') && last_char.empty()) ||
           last_char == "!" || last_char == "！") &&
          (config.auto_conversion_key() &
           config::Config::AUTO_CONVERSION_EXCLAMATION_MARK));
}

bool IsValidDirectCommitTriggerKey(const config::Config& config,
                                   const commands::KeyEvent& key_event) {
  return (MatchesKeyEvent(key_event, static_cast<uint32_t>('.'),
                          {".", "．", "。", "｡"}) &&
          (config.direct_commit_key() &
           config::Config::DIRECT_COMMIT_KUTEN)) ||
         (MatchesKeyEvent(key_event, static_cast<uint32_t>(','),
                          {",", "，", "、", "､"}) &&
          (config.direct_commit_key() &
           config::Config::DIRECT_COMMIT_TOUTEN)) ||
         (MatchesKeyEvent(key_event, static_cast<uint32_t>('?'),
                          {"?", "？"}) &&
          (config.direct_commit_key() &
           config::Config::DIRECT_COMMIT_QUESTION_MARK)) ||
         (MatchesKeyEvent(key_event, static_cast<uint32_t>('!'),
                          {"!", "！"}) &&
          (config.direct_commit_key() &
           config::Config::DIRECT_COMMIT_EXCLAMATION_MARK)) ||
         (MatchesKeyEvent(key_event, static_cast<uint32_t>('('),
                          {"(", "（"}) &&
          (config.direct_commit_key() &
           config::Config::DIRECT_COMMIT_OPEN_PARENTHESIS)) ||
         (MatchesKeyEvent(key_event, static_cast<uint32_t>(')'),
                          {")", "）"}) &&
          (config.direct_commit_key() &
           config::Config::DIRECT_COMMIT_CLOSE_PARENTHESIS)) ||
         (MatchesKeyEvent(key_event, static_cast<uint32_t>('['),
                          {"[", "［", "「"}) &&
          (config.direct_commit_key() &
           config::Config::DIRECT_COMMIT_OPEN_BRACKET)) ||
         (MatchesKeyEvent(key_event, static_cast<uint32_t>(']'),
                          {"]", "］", "」"}) &&
          (config.direct_commit_key() &
           config::Config::DIRECT_COMMIT_CLOSE_BRACKET)) ||
         ((MatchesString(key_event.key_string(), {"・", "･"}) ||
           (SymbolMethodUsesMiddleDot(config) &&
            MatchesKeyEvent(key_event, static_cast<uint32_t>('/'),
                            {"/", "／"}))) &&
          (config.direct_commit_key() &
           config::Config::DIRECT_COMMIT_MIDDLE_DOT));
}

bool IsKutenOrToutenChar(absl::string_view ch) {
  absl::string_view rest;
  char32_t codepoint = 0;
  if (!Util::SplitLastChar32(ch, &rest, &codepoint) || !rest.empty()) {
    return false;
  }
  switch (codepoint) {
    case 0x002E:  // ASCII full stop
    case 0xFF0E:  // Fullwidth full stop
    case 0x3002:  // Ideographic full stop
    case 0xFF61:  // Halfwidth ideographic full stop
    case 0x002C:  // ASCII comma
    case 0xFF0C:  // Fullwidth comma
    case 0x3001:  // Ideographic comma
    case 0xFF64:  // Halfwidth ideographic comma
      return true;
    default:
      return false;
  }
}

bool IsValidDirectCommitChar(const config::Config& config,
                             absl::string_view last_char) {
  return
      (MatchesString(last_char, {".", "．", "。", "｡"}) &&
       (config.direct_commit_key() &
        config::Config::DIRECT_COMMIT_KUTEN)) ||

      (MatchesString(last_char, {",", "，", "、", "､"}) &&
       (config.direct_commit_key() &
        config::Config::DIRECT_COMMIT_TOUTEN)) ||

      (MatchesString(last_char, {"?", "？"}) &&
       (config.direct_commit_key() &
        config::Config::DIRECT_COMMIT_QUESTION_MARK)) ||

      (MatchesString(last_char, {"!", "！"}) &&
       (config.direct_commit_key() &
        config::Config::DIRECT_COMMIT_EXCLAMATION_MARK)) ||

      (MatchesString(last_char, {"(", "（"}) &&
       (config.direct_commit_key() &
        config::Config::DIRECT_COMMIT_OPEN_PARENTHESIS)) ||

      (MatchesString(last_char, {")", "）"}) &&
       (config.direct_commit_key() &
        config::Config::DIRECT_COMMIT_CLOSE_PARENTHESIS)) ||

      (MatchesString(last_char, {"[", "［", "「"}) &&
       (config.direct_commit_key() &
        config::Config::DIRECT_COMMIT_OPEN_BRACKET)) ||

      (MatchesString(last_char, {"]", "］", "」"}) &&
       (config.direct_commit_key() &
        config::Config::DIRECT_COMMIT_CLOSE_BRACKET)) ||

      (MatchesString(last_char, {"・", "･"}) &&
       (config.direct_commit_key() &
        config::Config::DIRECT_COMMIT_MIDDLE_DOT));
}

}  // namespace

std::pair<std::string, std::string>
Session::GetDirectCommitStringsWithDirectCommitSuffixFallback(
    const composer::Composer& composer_before_insert,
    const commands::KeyEvent& key) const {
  const std::string key_before_insert =
      composer_before_insert.GetQueryForConversion();
  const std::string value_before_insert =
      composer_before_insert.GetStringForSubmission();

  std::string key_to_commit = context_->composer().GetQueryForConversion();
  std::string value_to_commit = context_->composer().GetStringForSubmission();

  const std::string suffix = DirectCommitFallbackSuffixFromKeyEvent(
      context_->GetConfig(), key);

  // Some test and client paths provide the direct-commit trigger as the key
  // event itself while leaving the restored preedit unchanged in Composer.
  // In that case, explicitly append the trigger suffix so that the visible
  // commit remains `きょう。`, while the strong learning target remains the
  // suffix-free `きょう` captured before insertion.
  if (value_to_commit == value_before_insert) {
    if (!suffix.empty()) {
      key_to_commit = absl::StrCat(key_before_insert, suffix);
      value_to_commit = absl::StrCat(value_before_insert, suffix);
    }
  }

  return {key_to_commit, value_to_commit};
}

bool Session::CanStartAutoConversion(
    const commands::KeyEvent& key_event) const {
  if (!context_->GetConfig().use_auto_conversion()) {
    return false;
  }

  // Disable if the input comes from non-standard user keyboards, like numpad.
  if (key_event.input_style() != commands::KeyEvent::FOLLOW_MODE) {
    return false;
  }

  // We simply disable the auto conversion feature if the mode is ASCII.
  // We conclude that disabling this feature is better in this situation.
  // TODO(taku): fix the behavior. Converter module needs to be fixed.
  if (key_event.mode() == commands::HALF_ASCII ||
      key_event.mode() == commands::FULL_ASCII) {
    return false;
  }

  // We should NOT check key_string.
  // http://b/issue?id=3217992
  // Auto conversion is not triggered if the composition is empty or
  // only one character, or the cursor is not in the end of the
  // composition.
  const size_t length = context_->composer().GetLength();
  if (length <= 1 || length != context_->composer().GetCursor()) {
    return false;
  }

  const uint32_t key_code = key_event.key_code();
  const std::string preedit = context_->composer().GetStringForPreedit();
  const absl::string_view last_char = Util::Utf8SubString(preedit, length - 1, 1);
  if (last_char.empty()) {
    return false;
  }

  if (!IsValidAutoConversionKey(context_->GetConfig(), key_code, last_char)) {
    return false;
  }

  // Check the previous character of last_character.
  // when |last_prev_char| is number, we don't invoke auto_conversion
  // if the same invoke key is repeated, do not conversion.
  // http://b/issue?id=2932118
  const absl::string_view last_prev_char =
      Util::Utf8SubString(preedit, length - 2, 1);
  if (last_prev_char.empty() || last_prev_char == last_char ||
      Util::NUMBER == Util::GetScriptType(last_prev_char)) {
    return false;
  }
  return true;
}

bool Session::CompositionEndsWithEnglishPiece(absl::string_view extra) const {
  // Only a live composition can end in an English piece: with nothing typed
  // there is no piece, and during conversion Space means "next candidate".
  if (context_->state() != ImeContext::COMPOSITION &&
      !live_conversion_active_ && !live_conversion_pending_) {
    return false;
  }

  std::string raw = context_->composer().GetRawString();
  if (raw.empty()) return false;
  raw.append(extra);

  // Ask Meltype about the text as typed so far.
  const std::vector<mozkey::meltype::ScoredSegmenter::Segment> pieces =
      MeltypeBridge::SegmentRomaji(raw, /*final=*/false);
  if (pieces.empty()) return false;  // no dictionaries: Mozc's Convert stays

  // Only the very last piece decides.  A trailing space is its own Japanese
  // piece, so "anoGithub " (the space just accepted) makes the *next* Space
  // convert — pressing Space twice is the escape hatch when the user really
  // wants a candidate list.
  return pieces.back().is_english;
}

bool Session::IsTemporaryShiftAsciiInputMode() const {
  const transliteration::TransliterationType input_mode =
      context_->composer().GetInputMode();
  if (input_mode != transliteration::HALF_ASCII &&
      input_mode != transliteration::FULL_ASCII) {
    return false;
  }

  // Mozc's temporary Shift+letter mode leaves the comeback mode at the user's
  // real mode (HIRAGANA); choosing alphanumeric mode on purpose (Eisu) makes
  // the alphanumeric mode the comeback mode too.
  const transliteration::TransliterationType comeback_mode =
      context_->composer().GetComebackInputMode();
  return comeback_mode != transliteration::HALF_ASCII &&
         comeback_mode != transliteration::FULL_ASCII;
}

void Session::EndTemporaryShiftAsciiRunIfWordEnded(
    const commands::KeyEvent& key_event) {
  // The ASCII mode has to be Mozc's own temporary one.  A user-chosen
  // alphanumeric mode (Eisu) is where the user wants raw ASCII, so nothing is
  // reverted there.
  if (!IsTemporaryShiftAsciiInputMode()) return;

  // Only the classic ASCII-switch behaviour creates the temporary mode.
  if (context_->GetConfig().shift_key_mode_switch() !=
      config::Config::ASCII_INPUT_MODE) {
    return;
  }

  // A Shift+letter always starts a new word, so only an unshifted letter can
  // end the current one.  AsciiInputChar() returns the character the key
  // types, and an uppercase one here means the key was shifted.
  const std::string input = AsciiInputChar(key_event);
  if (input.size() != 1) return;
  const char letter = input[0];
  if (letter < 'a' || letter > 'z') return;

  // Meltype is the only judge: it reports the boundary where the English piece
  // ends and the Japanese piece begins.  Without its dictionaries it has no
  // opinion and the ASCII run continues exactly as before.
  const std::string raw = context_->composer().GetRawString();
  if (raw.empty()) return;
  if (!MeltypeBridge::EndsTemporaryShiftAsciiRun(raw, letter)) return;

  composer::Composer* composer = context_->mutable_composer();

  // Some of the Japanese syllable was already inserted as raw ASCII before the
  // boundary could be known.  "Android" + "s" is Android + す, but "Androids"
  // is also an English plural, so the boundary only becomes visible on the
  // "u" that follows — by then the "s" is already in the composition as ASCII.
  // Erase that tail and re-insert it in Hiragana; the letter being typed now is
  // not in the composition yet and will be transliterated normally.
  const std::size_t rewrite =
      MeltypeBridge::JapaneseTailLength(raw, letter);
  if (rewrite > 0 && rewrite <= raw.size() &&
      composer->GetCursor() == composer->GetLength()) {
    const std::string tail = raw.substr(raw.size() - rewrite);
    // Composition positions count characters, and these are all ASCII letters,
    // so deriving the start from GetLength() is safe either way.
    composer->DeleteRange(composer->GetLength() - rewrite, rewrite);
    composer->ResetInputMode();
    for (const char c : tail) {
      composer->InsertCharacter(std::string(1, c));
    }
    TraceStep("Composer",
              absl::StrCat("end temporary ASCII before \"", TraceText(input),
                           "\" and re-read \"", TraceText(tail),
                           "\" as Japanese"));
    return;
  }

  // Back to the user's real mode.  ResetInputMode() is Mozc's own "leave the
  // temporary Shift+letter mode" operation — it restores comeback_input_mode_
  // and resets the shifted-sequence counters, so the next Shift+letter starts a
  // fresh English word.
  composer->ResetInputMode();
  TraceStep("Composer",
            absl::StrCat("end temporary ASCII before \"", TraceText(input),
                         "\" (Meltype: the English word ended)"));
}

void Session::EndTemporaryShiftAsciiRunBeforeSpace() {
  if (!IsTemporaryShiftAsciiInputMode()) return;
  if (context_->GetConfig().shift_key_mode_switch() !=
      config::Config::ASCII_INPUT_MODE) {
    return;
  }
  // Only when the composition as it stands ends with an English piece.  A
  // trailing Japanese word must not be disturbed, and a Japanese word that
  // merely contains letters ("あのGithubの") ends with a Japanese piece, so this
  // asks Meltype about the text as typed rather than guessing from the mode.
  if (!CompositionEndsWithEnglishPiece()) return;

  context_->mutable_composer()->ResetInputMode();
  TraceStep("Composer",
            "end temporary ASCII at a space after an English word");
}

bool Session::CompositionMixesJapaneseAndEnglish(absl::string_view extra) const {
  std::string raw = context_->composer().GetRawString();
  if (raw.empty()) return false;
  raw.append(extra);

  const std::vector<mozkey::meltype::ScoredSegmenter::Segment> pieces =
      MeltypeBridge::SegmentRomaji(raw, /*final=*/false);
  if (pieces.empty()) return false;

  bool has_japanese = false;
  bool has_english = false;
  for (const auto& piece : pieces) {
    (piece.is_english ? has_english : has_japanese) = true;
  }
  return has_japanese && has_english;
}

bool Session::CanDirectCommitPendingLiveConversionBeforeInsert(
    const commands::KeyEvent& key_event) const {
  const config::Config& config = context_->GetConfig();

  if (!config.use_direct_commit()) {
    return false;
  }

  // Mutual exclusion guard. Even if both are accidentally enabled in config,
  // direct commit is disabled here.
  if (config.use_auto_conversion()) {
    return false;
  }

  if (context_->state() != ImeContext::COMPOSITION) {
    return false;
  }

  // Disable if the input comes from non-standard user keyboards, like numpad.
  if (key_event.input_style() != commands::KeyEvent::FOLLOW_MODE) {
    return false;
  }

  // Disable in ASCII mode.
  if (key_event.mode() == commands::HALF_ASCII ||
      key_event.mode() == commands::FULL_ASCII) {
    return false;
  }

  const size_t length = context_->composer().GetLength();
  if (length == 0 || length != context_->composer().GetCursor()) {
    return false;
  }

  return IsValidDirectCommitTriggerKey(config, key_event);
}

bool Session::CanDirectCommitAfterPunctuation(
    const commands::KeyEvent& key_event) const {
  const config::Config& config = context_->GetConfig();

  if (!config.use_direct_commit()) {
    return false;
  }

  // Mutual exclusion guard. Even if both are accidentally enabled in config,
  // direct commit is disabled here.
  if (config.use_auto_conversion()) {
    return false;
  }

  if (!(context_->state() &
        (ImeContext::PRECOMPOSITION | ImeContext::COMPOSITION))) {
    return false;
  }

  // Disable if the input comes from non-standard user keyboards, like numpad.
  if (key_event.input_style() != commands::KeyEvent::FOLLOW_MODE) {
    return false;
  }

  // Disable in ASCII mode.
  if (key_event.mode() == commands::HALF_ASCII ||
      key_event.mode() == commands::FULL_ASCII) {
    return false;
  }

  const size_t length = context_->composer().GetLength();
  if (length == 0 || length != context_->composer().GetCursor()) {
    return false;
  }

  const std::string preedit = context_->composer().GetStringForPreedit();
  const absl::string_view last_char =
      Util::Utf8SubString(preedit, length - 1, 1);
  if (last_char.empty() || !IsValidDirectCommitChar(config, last_char)) {
    return false;
  }

  // Keep numeric punctuation in the composition. Mozc normalizes Japanese
  // punctuation after a number to decimal/grouping punctuation, so committing
  // here would split inputs such as "3.14" and "1,000" at the punctuation.
  if (length >= 2 && IsKutenOrToutenChar(last_char)) {
    const absl::string_view last_prev_char =
        Util::Utf8SubString(preedit, length - 2, 1);
    if (!last_prev_char.empty() &&
        Util::GetScriptType(last_prev_char) == Util::NUMBER) {
      return false;
    }
  }

  return true;
}

void Session::UpdateTime() {
  context_->set_last_command_time(Clock::GetAbslTime());
}

void Session::TransformInput(commands::Input* input) {
  if (!input->has_key()) {
    return;
  }

  context_->key_event_transformer().TransformKeyEvent(input->mutable_key());

  if (!input->has_context() || !input->context().vertical_writing()) {
    return;
  }

  VerticalWritingKeyState vertical_key_state = VerticalWritingKeyState::kOther;
  if (context_->state() == ImeContext::COMPOSITION &&
      context_->converter().CheckState(EngineConverterInterface::SUGGESTION)) {
    vertical_key_state = VerticalWritingKeyState::kSuggestion;
  } else if (context_->state() == ImeContext::CONVERSION) {
    vertical_key_state =
        context_->converter().CheckState(EngineConverterInterface::PREDICTION)
            ? VerticalWritingKeyState::kPrediction
            : VerticalWritingKeyState::kConversion;
  }

  TransformVerticalWritingCandidateArrowKey(
      /*vertical_writing=*/true, vertical_key_state,
      context_->GetKeyMapManager(), input->mutable_key());
}

bool Session::SwitchInputFieldType(commands::Command* command) {
  command->mutable_output()->set_consumed(true);
  context_->mutable_composer()->SetInputFieldType(
      command->input().context().input_field_type());
  Output(command);
  return true;
}

bool Session::HandleIndirectImeOnOff(commands::Command* command) {
  const commands::KeyEvent& key = command->input().key();
  if (!key.has_activated()) {
    return true;
  }
  const ImeContext::State state = context_->state();
  if (state == ImeContext::DIRECT && key.activated()) {
    // Indirect IME On found.
    commands::Command on_command;
    on_command = *command;
    if (!IMEOn(&on_command)) {
      return false;
    }
  } else if (state != ImeContext::DIRECT && !key.activated()) {
    // Indirect IME Off found.
    commands::Command off_command;
    off_command = *command;
    if (!IMEOff(&off_command)) {
      return false;
    }
  }
  return true;
}

bool Session::ImeAction(commands::Command* command) {
  if (context_->state() != ImeContext::PRECOMPOSITION ||
      context_->composer().GetInputFieldType() != commands::Context::NORMAL) {
    return false;
  }

  // ImeAction is triggered when the mobile-specific IME action buttons such as
  // Search, Go, or Next, are tapped. After a user finishes an IME session, they
  // might still perform manual edits using the Backspace key. To sync these
  // edits with the converter, we call CommitContext. Typical use case is the
  // partial-revert on user history training.
  context_->mutable_converter()->CommitContext(context_->composer(),
                                               command->input().context());

  return true;
}

bool Session::CommitRawText(commands::Command* command) {
  if (context_->composer().GetLength() == 0) {
    return false;
  }
  CommitRawTextDirectly(command);
  return true;
}

// TODO(komatsu): delete this function.
composer::Composer* Session::get_internal_composer_only_for_unittest() {
  return context_->mutable_composer();
}

const ImeContext& Session::context() const { return *context_; }

}  // namespace session
}  // namespace mozc
