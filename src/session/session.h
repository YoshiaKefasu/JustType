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

#ifndef MOZC_SESSION_SESSION_H_
#define MOZC_SESSION_SESSION_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "composer/composer.h"
#include "composer/table.h"
#include "engine/engine_interface.h"
#include "protocol/commands.pb.h"
#include "protocol/config.pb.h"
#include "session/ime_context.h"
#include "session/keymap.h"
#include "session/meltype_piece_pipeline.h"
#include "session/systemone_client.h"
#include "session/systemone_worker.h"
#include "session/trace_logger.h"
#include "transliteration/transliteration.h"

namespace mozc {
namespace session {

class Session {
 public:
  explicit Session(const EngineInterface& engine);
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  bool SendKey(mozc::commands::Command* command);

  // Check if the input key event will be consumed by the session.
  bool TestSendKey(mozc::commands::Command* command);

  // Perform the SEND_COMMAND command defined commands.proto.
  bool SendCommand(mozc::commands::Command* command);

  // Turn on IME. Do nothing (but the keyevent is consumed) when IME is already
  // turned on.
  bool IMEOn(mozc::commands::Command* command);

  // Turn off IME. Do nothing (but the keyevent is consumed) when IME is already
  // turned off.
  bool IMEOff(mozc::commands::Command* command);

  // Unlike IMEOn/IMEOff, these commands 1) can update composition mode, and
  // 2) are functional even when IME is already turned on/off.
  // TODO(team): Merge these into IMEOn/Off once b/10250883 is fixed.
  bool MakeSureIMEOn(mozc::commands::Command* command);
  bool MakeSureIMEOff(mozc::commands::Command* command);

  bool EchoBack(mozc::commands::Command* command);
  bool EchoBackAndClearUndoContext(mozc::commands::Command* command);
  bool DoNothing(mozc::commands::Command* command);

  // Tries deleting the specified candidate from the user prediction history.
  // The candidate is determined by command.input.command.id, or the current
  // focused candidate if that ....command.id is not specified. If
  // that candidate, as a key value pair, doesn't exist in the user history,
  // nothing happens. Regardless of the result of internal history deletion,
  // invoking this method has the same effect as ConvertCancel() from the
  // viewpoint of session, meaning that the session state gets back to
  // composition.
  bool DeleteCandidateFromHistory(mozc::commands::Command* command);

  // Handles the keymap-level DeleteSelectedCandidate action.
  bool DeleteSelectedCandidate(mozc::commands::Command* command);

  // Resets the composer and clear conversion segments.
  // History segments will not be cleared.
  // Therefore if a user commits "風"(かぜ) and Revert method is called,
  // preedit "ひいた"  will be converted into "邪引いた".
  bool Revert(mozc::commands::Command* command);
  // Reset the composer and clear all the segments (including history segments).
  // Therefore preedit "ひいた"  will *not* be converted into "邪引いた"
  // on the situation described above.
  bool ResetContext(mozc::commands::Command* command);

  // Returns the current status such as a composition string, input mode, etc.
  bool GetStatus(mozc::commands::Command* command);

  // Fills Output::Callback with the CONVERT_REVERSE SessionCommand to
  // ask the client to send back the SessionCommand to the server.
  // This function is called when the key event representing the
  // ConvertReverse keybinding is called.
  bool RequestConvertReverse(mozc::commands::Command* command);

  // Generates the normal InsertSpace fallback output and attaches a callback
  // asking the TSF client to reconvert selected application text, if any.
  bool RequestReconvertSelectionOrInsertSpace(
      mozc::commands::Command* command);

  // Begins reverse conversion for the given session.  This function
  // is called when the CONVERT_REVERSE SessionCommand is called.
  bool ConvertReverse(mozc::commands::Command* command);

  // Fills Output::Callback with the Undo SessionCommand to ask the
  // client to send back the SessionCommand to the server.
  // This function is called when the key event representing the
  // Undo keybinding is called.
  bool RequestUndo(mozc::commands::Command* command);

  // Undos the commitment.  This function is called when the
  // UNDO SessionCommand is called.
  bool Undo(mozc::commands::Command* command);

  bool InsertSpace(mozc::commands::Command* command);
  bool InsertSpaceToggled(mozc::commands::Command* command);
  bool InsertSpaceHalfWidth(mozc::commands::Command* command);
  bool InsertSpaceFullWidth(mozc::commands::Command* command);
  bool InsertCharacter(mozc::commands::Command* command);
  bool UpdateComposition(mozc::commands::Command* command);
  bool UpdateCompositionInternal(mozc::commands::Command* command);
  bool Delete(mozc::commands::Command* command);
  bool Backspace(mozc::commands::Command* command);
  bool EditCancel(mozc::commands::Command* command);
  bool EditCancelAndIMEOff(mozc::commands::Command* command);

  bool MoveCursorRight(mozc::commands::Command* command);
  bool MoveCursorLeft(mozc::commands::Command* command);
  bool MoveCursorToEnd(mozc::commands::Command* command);
  bool MoveCursorToBeginning(mozc::commands::Command* command);
  bool MoveCursorTo(mozc::commands::Command* command);
  bool Convert(mozc::commands::Command* command);
  // Starts conversion not using user history.  This is used for debugging.
  bool ConvertWithoutHistory(mozc::commands::Command* command);
  bool ConvertNext(mozc::commands::Command* command);
  bool ConvertPrev(mozc::commands::Command* command);
  // Shows the next page of candidates.
  bool ConvertNextPage(mozc::commands::Command* command);
  // Shows the previous page of candidates.
  bool ConvertPrevPage(mozc::commands::Command* command);
  bool ConvertCancel(mozc::commands::Command* command);
  bool PredictAndConvert(mozc::commands::Command* command);
  // Note: Commit() also triggers zero query suggestion.
  // TODO(team): Rename this method to CommitWithZeroQuerySuggest.
  bool Commit(mozc::commands::Command* command);
  bool CommitNotTriggeringZeroQuerySuggest(commands::Command* command);
  bool CommitFirstSuggestion(mozc::commands::Command* command);
  // Select a candidate located by input.command.id and commit.
  bool CommitCandidate(mozc::commands::Command* command);

  // Commits only the first segment.
  bool CommitSegment(mozc::commands::Command* command);
  // Commits some characters at the head of the preedit.
  bool CommitHead(size_t count, mozc::commands::Command* command);
  // Commits preedit if in password mode.
  bool CommitIfPassword(mozc::commands::Command* command);

  bool SegmentFocusRight(mozc::commands::Command* command);
  bool SegmentFocusLeft(mozc::commands::Command* command);
  bool SegmentFocusLast(mozc::commands::Command* command);
  bool SegmentFocusLeftEdge(mozc::commands::Command* command);
  bool SegmentWidthExpand(mozc::commands::Command* command);
  bool SegmentWidthShrink(mozc::commands::Command* command);

  // Selects the transliteration candidate.  If the current state is
  // composition, candidates will be generated with only translitaration
  // candidates.
  bool ConvertToHiragana(mozc::commands::Command* command);
  bool ConvertToFullKatakana(mozc::commands::Command* command);
  bool ConvertToHalfKatakana(mozc::commands::Command* command);
  bool ConvertToFullASCII(mozc::commands::Command* command);
  bool ConvertToHalfASCII(mozc::commands::Command* command);
  bool ConvertToHalfWidth(mozc::commands::Command* command);
  // Switch the composition to Hiragana, full-width Katakana or
  // half-width Katakana by rotation.
  bool SwitchKanaType(mozc::commands::Command* command);

  // Select the transliteration candidate if the current status is
  // conversion.  This is same with the above ConvertTo functions.  If
  // the current state is composition, the display mode is changed to the
  // transliteration and the composition state still remains.
  bool DisplayAsHiragana(mozc::commands::Command* command);
  bool DisplayAsFullKatakana(mozc::commands::Command* command);
  bool DisplayAsHalfKatakana(mozc::commands::Command* command);
  bool TranslateFullASCII(mozc::commands::Command* command);
  bool TranslateHalfASCII(mozc::commands::Command* command);
  bool TranslateHalfWidth(mozc::commands::Command* command);
  bool ToggleAlphanumericMode(mozc::commands::Command* command);

  // Switch the composition mode.
  bool CompositionModeHiragana(mozc::commands::Command* command);
  bool CompositionModeFullKatakana(mozc::commands::Command* command);
  bool CompositionModeHalfKatakana(mozc::commands::Command* command);
  bool CompositionModeFullASCII(mozc::commands::Command* command);
  bool CompositionModeHalfASCII(mozc::commands::Command* command);
  bool CompositionModeSwitchKanaType(mozc::commands::Command* command);

  // Specify the input field type.
  bool SwitchInputFieldType(mozc::commands::Command* command);

  // Let client launch config dialog
  bool LaunchConfigDialog(mozc::commands::Command* command);

  // Let client launch dictionary tool
  bool LaunchDictionaryTool(mozc::commands::Command* command);

  // Let client launch word register dialog
  bool LaunchWordRegisterDialog(mozc::commands::Command* command);

  // Undo if pre-composition is empty. Rewind KANA cycle otherwise.
  bool UndoOrRewind(mozc::commands::Command* command);

  // Stops key toggling in the composer.
  bool StopKeyToggling(mozc::commands::Command* command);

  // Special IME action key on mobile, such as Search, Send, Go, and Next
  // keys.
  bool ImeAction(mozc::commands::Command* command);

  bool ReportBug(mozc::commands::Command* command);

  void SetConfig(std::shared_ptr<const mozc::config::Config> config);

  void SetKeyMapManager(
      std::shared_ptr<const mozc::keymap::KeyMapManager> key_map_manager);

  void SetRequest(std::shared_ptr<const mozc::commands::Request> request);

  void SetConfig(mozc::config::Config config) {
    SetConfig(std::make_shared<const config::Config>(std::move(config)));
  }

  void SetRequest(mozc::commands::Request request) {
    SetRequest(
        std::make_shared<const mozc::commands::Request>(std::move(request)));
  }

  void SetTable(std::shared_ptr<const mozc::composer::Table> table);

  // Set client capability for this session.  Used by unittest.
  void set_client_capability(mozc::commands::Capability capability);

  // Set application information for this session.
  void set_application_info(mozc::commands::ApplicationInfo application_info);

  // Get application information
  const mozc::commands::ApplicationInfo& application_info() const;

  // Return the time when this instance was created.
  absl::Time create_session_time() const;

  // return 0 (default value) if no command is executed in this session.
  absl::Time last_command_time() const;

  // TODO(komatsu): delete this function.
  // For unittest only
  mozc::composer::Composer* get_internal_composer_only_for_unittest();

  const ImeContext& context() const;

 private:
  friend class SessionTestPeer;

  struct PassiveSuggestionSnapshot {
    commands::CandidateWindow candidate_window;
    std::unique_ptr<ImeContext> context;
  };

  // One continuation per IME session. Invalidated on every physical key.
  // Never retain a pointer to a stack-allocated commands::Command.
  keymap::CommandSequence pending_command_sequence_;
  uint32_t command_sequence_generation_ = 0;
  bool command_sequence_waits_for_reconversion_ = false;
  std::string pending_command_sequence_key_;

  std::unique_ptr<ImeContext> context_;

  // True only while ordinary composition is displaying a passive suggestion
  // that was intentionally held across unresolved trailing Roman input.
  bool composition_holds_passive_suggestion_ = false;

  // True while the current CONVERSION state was started by live conversion.
  // In this mode, ordinary character input should keep editing the underlying
  // composition instead of committing the conversion.
  bool live_conversion_active_ = false;

  // True when the live-conversion state came from streaming composition
  // (including a pending live-conversion request) rather than ordinary
  // conversion.  LiveConversion keymap bindings and live-edit continuation
  // only apply in this mode.
  bool live_conversion_from_composition_ = false;

  // True while a live conversion request has been scheduled but not applied yet.
  bool live_conversion_pending_ = false;

  // Monotonically increasing generation used to ignore stale delayed callbacks.
  uint32_t live_conversion_generation_ = 0;
  uint32_t pending_live_conversion_generation_ = 0;

  // Reading at the time when the current delayed live conversion was scheduled.
  std::string pending_live_conversion_key_;

  // Original SEND_KEY input that scheduled the current delayed live conversion.
  // Delayed APPLY_LIVE_CONVERSION callbacks are SEND_COMMAND inputs, so this is
  // reused to keep passive suggestion generation consistent after the delay.
  commands::Input pending_live_conversion_input_;

  // Passive suggestion window generated for the current delayed live conversion.
  // It is reused if the delayed callback cannot regenerate suggestions, which
  // prevents the visible suggestion window from disappearing at materialization.
  commands::CandidateWindow pending_live_conversion_suggestion_candidate_window_;

  // Exact cloned context that owns candidate IDs in the pending window above.
  // Pending and active live-conversion suggestions are separate semantic
  // snapshots and must never share ownership implicitly.
  std::unique_ptr<ImeContext> pending_live_conversion_suggestion_context_;

  // True when the delayed live-conversion request was scheduled from a
  // physical lower-case Roman key whose unresolved suffix must keep the
  // previously visible passive suggestion instead of refreshing prediction.
  // Keep this explicit: Composer state alone cannot distinguish Roman keyboard
  // input from key_string/mobile input that may use pending_ internally.
  bool pending_live_conversion_holds_passive_suggestion_ = false;

  // Trace sequence carried by the pending delayed live conversion, so the
  // delayed block appends to the keystroke that scheduled it.
  uint64_t pending_live_conversion_trace_seq_ = 0;
  absl::Time pending_live_conversion_trace_started_at_;

  // Passive suggestion window currently associated with live conversion output.
  // Some delayed callbacks re-render live conversion without regenerating
  // suggestions; this cache keeps the passive suggestion window stable there.
  commands::CandidateWindow live_conversion_suggestion_candidate_window_;

  // Exact cloned context that produced the passive suggestion window above.
  // Candidate IDs in that window belong to this converter state, not to the
  // real live-conversion converter which intentionally remains in CONVERSION.
  std::unique_ptr<ImeContext> live_conversion_suggestion_context_;

  // True only when the active live-conversion passive suggestion is an exact
  // held snapshot from before the current unresolved Roman suffix.
  bool live_conversion_holds_passive_suggestion_ = false;

  // The reading used for the latest successful live conversion.
  std::string live_conversion_key_;

  // The raw preedit string at the time of the latest successful live
  // conversion. This is used to compute the pending raw suffix.
  std::string live_conversion_preedit_;

  // The visible converted preedit generated by the latest live conversion.
  // This is used as the stable converted prefix while a delayed live
  // conversion is pending.
  std::string live_conversion_value_;

  // The full preedit generated by the latest successful live conversion.
  // Pending live conversion reuses its segment structure and annotations
  // to avoid display-attribute flicker.
  commands::Preedit live_conversion_preedit_output_;

  // A presentation-only basis used while ordinary character input continues
  // from the visible presentation.  This state exists only so pending preedit
  // output and pending direct commit extend what the user actually saw rather
  // than leaking a hidden surface.
  struct StablePresentationSpan {
    std::string key;
    std::string raw_preedit;
    std::string value;
    commands::Preedit::Segment preedit_segment;
  };

  struct PendingLiveConversionPresentation {
    std::string key;
    std::string raw_preedit;
    std::string value;
    commands::Preedit preedit_output;

    // Leading reading/value spans whose correspondence was already proven by
    // the Mozc result that existed when this presentation was visible.  These
    // spans may survive a later converter re-segmentation as long as their
    // reading still matches the current composition at the same position.
    std::vector<StablePresentationSpan> stable_spans;
  };
  std::optional<PendingLiveConversionPresentation>
      pending_live_conversion_presentation_;

  // Set only after an explicit conversion Cancel command, such as Esc or Ctrl+Z
  // in the default keymap.  If the user commits the unchanged hiragana preedit
  // immediately after that cancel, the raw preedit should be learned like an
  // explicitly selected non-default candidate, matching F6 -> Enter behavior.
  bool pending_reranked_preedit_commit_after_convert_cancel_ = false;
  std::string pending_reranked_preedit_commit_key_;
  std::string pending_reranked_preedit_commit_value_;
  std::vector<std::string> pending_reranked_preedit_commit_segment_keys_;

  struct PendingDirectCommitLearning {
    bool pending = false;
    std::string key;
    std::string value;
    std::string reason;

    // Snapshot immediately after normal CommitInternal().
    // It preserves the converter's revert_id and committed Segments so that
    // delayed cancellation can revert the original rich Mozc learning even
    // after the current converter is reset by CommitStringDirectly().
    std::unique_ptr<ImeContext> revert_context;
  };

  PendingDirectCommitLearning pending_direct_commit_learning_;

  // SystemOne (Jev / Laya) per-piece decision for the current live
  // conversion.  One batch holds one question per Japanese piece; it runs on
  // the worker thread and the answers return through an APPLY_SYSTEMONE_LIVE
  // delayed callback that polls TakeResults(), so the key-event path never
  // blocks on the network.  A piece answered once stays answered: its choice
  // lives in piece_choice_cache_, so the next keystroke shows it at 0ms
  // without asking again.
  struct PendingPieceSystemOne {
    bool pending = false;
    uint32_t generation = 0;
    // The raw romaji the batch was built from; a callback for any other raw
    // is stale even when the generation matches (the callback never carries
    // a stale generation, but the composer may have moved on without a new
    // submission).
    std::string raw;
    // Parallel to |questions|: the request built for each question.
    std::vector<SystemOneRequest> requests;
    std::vector<PieceQuestion> questions;
    // Set as each answer lands (or fails); applied answers are never
    // re-logged or re-cached within the same batch.
    std::vector<bool> handled;
    absl::Time issued_at;
    // Trace sequence of the command that submitted this batch, so the answer
    // block is appended to that keystroke's trace.
    uint64_t trace_seq = 0;
    absl::Time trace_started_at;
  };

  uint32_t systemone_generation_ = 0;
  PendingPieceSystemOne pending_piece_systemone_;
  std::unique_ptr<SystemOneWorker> systemone_worker_;

  // Dedicated converter for the per-piece dictionary candidate lists.  The
  // session's own converter stays on the whole composition (candidate window,
  // Space -> Convert, learning), so the piece queries must not share its
  // state.
  std::unique_ptr<engine::EngineConverterInterface> piece_converter_;

  // Candidates are stable within one live-conversion episode, and every poll
  // tick rebuilds the display, so per-reading caching keeps a tick down to
  // map lookups.  Cleared with the live conversion state.
  std::map<std::string, std::vector<std::string>> piece_options_cache_;

  // SystemOne's proven choices: key = PieceChoiceKey(question as asked),
  // value = the chosen surface.  Cleared with the live conversion state.
  std::map<std::string, std::string> piece_choice_cache_;

  // The plan for the SystemOne questions: per-piece baselines, left
  // contexts, options.  Empty = Meltype had no opinion.  The screen itself
  // is Mozc's own conversion; this plan feeds the questions and the
  // protection overlay.
  PiecePlan meltype_piece_plan_;

  // The cached SystemOne answer per piece, parallel to meltype_piece_plan_.
  // pieces ("" = none).  Rebuilt with the plan, consumed by the overlay.
  std::vector<std::string> meltype_piece_choices_;

  // True when the last display build rewrote anything on top of Mozc's own
  // conversion — an English piece protected to its letters, or a SystemOne
  // choice collapsed in.  Only then does the commit bypass the converter
  // (whose segments would restore the un-protected surface); otherwise the
  // ordinary commit keeps its learning and undo semantics.
  bool meltype_display_protected_ = false;

  // Per-keystroke pipeline trace; see TraceLogger.  trace_seq_ == 0 means no
  // sequence is active and every helper is a no-op.  A sequence spans the
  // keystroke and the delayed live-conversion / SystemOne callbacks that
  // follow it; each block flushes independently.
  uint64_t trace_seq_ = 0;
  bool trace_first_block_ = true;
  absl::Time trace_sequence_start_;
  absl::Time trace_block_start_;
  absl::Time trace_mark_;
  std::vector<std::string> trace_steps_;

  struct PendingLiveConversionUndoState {
    std::string pending_key;
    commands::Input pending_input;
    std::optional<PendingLiveConversionPresentation> pending_presentation;
    commands::CandidateWindow pending_suggestion_candidate_window;
    std::unique_ptr<ImeContext> pending_suggestion_context;
    commands::CandidateWindow live_suggestion_candidate_window;
    std::unique_ptr<ImeContext> live_suggestion_context;
    std::string live_key;
    std::string live_preedit;
    std::string live_value;
    commands::Preedit live_preedit_output;
  };

  struct UndoEntry {
    std::unique_ptr<ImeContext> context;
    std::optional<PendingLiveConversionUndoState> pending_live_conversion;
    bool revert_converter_on_undo = true;
  };

  // Undo stack. *begin is the oldest, and *back is the newest.
  std::deque<UndoEntry> undo_contexts_;

  std::unique_ptr<ImeContext> CreateContext(
      const EngineInterface& engine) const;

  void PushUndoContext();
  void PushDirectCommitUndoContext();
  void PopUndoContext();
  bool ShouldRevertConverterOnUndo() const;
  // Clear the undo context.
  // This should be called when the composer's preedit or cursor position
  // is updated by non-undo related operations. This achieves intuitive
  // behavior, which clears the undo context on the user's edit operation.
  void ClearUndoContext();
  bool HasUndoContext() const;

  // Returns true if |key| is assigned to Cancel in the current composition or
  // conversion keymap.
  bool IsCancelKeyForCompositionOrConversion(
      const mozc::commands::KeyEvent& key) const;

  // Set command.output.status.undo_available to true if HasUndoContext()==true.
  // Otherwise no-op.
  // Some code treats empty Status and default Status differently so
  // we don't want to create a new Status instance if not required.
  void MaybeSetUndoStatus(commands::Command* command) const;

  // Return true if full width space is preferred in the given new input
  // state than half width space. When |input| does not have new input mode,
  // the current mode will be considered.
  bool IsFullWidthInsertSpace(const mozc::commands::Input& input) const;

  bool EditCancelOnPasswordField(mozc::commands::Command* command);

  void MaybeSetPendingRerankedPreeditCommitAfterConvertCancel();
  void ClearPendingRerankedPreeditCommitAfterConvertCancel();
  bool ShouldMarkPreeditCommitAsRerankedAfterConvertCancel() const;
  bool ShouldMarkPreeditCommitAsRerankedAfterConvertCancel(
      const composer::Composer& composer) const;
  bool CommitPendingRerankedPreeditAfterConvertCancelForDirectCommit(
      const composer::Composer& composer,
      const commands::Context& context,
      absl::string_view reason);

  bool ConvertToTransliteration(
      mozc::commands::Command* command,
      mozc::transliteration::TransliterationType type);

  // Select a candidate located by input.command.id.  This command
  // would not be used from SendKey but used from SendCommand because
  // it requires the argument id.
  bool SelectCandidate(mozc::commands::Command* command);

  // Calls EngineConverter::ConmmitHeadToFocusedSegments()
  // and deletes characters from the composer.
  void CommitHeadToFocusedSegmentsInternal(const commands::Context& context);

  // Commits without EngineConverter.
  void CommitCompositionDirectly(commands::Command* command);
  std::pair<std::string, std::string>
  GetDirectCommitStringsWithDirectCommitSuffixFallback(
      const composer::Composer& composer_before_insert,
      const commands::KeyEvent& key) const;
  void CommitSourceTextDirectly(commands::Command* command);
  void CommitRawTextDirectly(commands::Command* command);
  void CommitStringDirectly(absl::string_view key, absl::string_view preedit,
                            commands::Command* command);
  bool CommitInternal(commands::Command* command,
                      bool trigger_zero_query_suggest);

  // Calls EngineConverter::Suggest if the condition is applicable to
  // call it.  True is returned when EngineConverter::Suggest is
  // called and results exist.  False is returned when
  // EngineConverter::Suggest is not called or no results exist.
  bool Suggest(const mozc::commands::Input& input);

  // Commands like EditCancel should restore the original string used for
  // the reverse conversion without any modification.
  // Returns true if the |source_text| is committed to cancel reconversion.
  // Returns false if this function has nothing to do.
  bool TryCancelConvertReverse(mozc::commands::Command* command);

  // Set the focus to the candidate located by input.command.id.  This
  // command would not be used from SendKey but used from SendCommand
  // because it requires the argument id.  The difference from
  // SelectCandidate is that HighlightCandidate does not close the
  // candidate window while SelectCandidate closes the candidate
  // window.
  bool HighlightCandidate(mozc::commands::Command* command);

  // The internal implementation of both SelectCandidate and HighlightCandidate.
  bool SelectCandidateInternal(mozc::commands::Command* command);

  // If the command is a shortcut to select a candidate from a list,
  // Process it and return true, otherwise return false.
  bool MaybeSelectCandidate(mozc::commands::Command* command);

  // Live conversion.
  bool MaybeStartLiveConversion(mozc::commands::Command* command);
  bool MaybeStartLiveConversionInternal(
      mozc::commands::Command* command,
      PassiveSuggestionSnapshot* held_suggestion);
  bool MaybeStartLiveConversionAfterEditing(
      mozc::commands::Command* command);
  bool MaybeScheduleLiveConversion(
      mozc::commands::Command* command,
      PassiveSuggestionSnapshot* held_suggestion = nullptr);
  bool ApplyDelayedLiveConversion(mozc::commands::Command* command);
  bool ApplyDelayedLiveConversionInternal(mozc::commands::Command* command);
  bool IgnoreStaleDelayedLiveConversion(mozc::commands::Command* command);

  // SystemOne (Jev / Laya) per-piece decision: one question per Japanese
  // piece, submitted on a worker thread, applied from an APPLY_SYSTEMONE_LIVE
  // delayed callback.
  SystemOneWorker* EnsureSystemOneWorker();
  // Mozc dictionary candidates for every Japanese piece's kana, collected
  // from the dedicated piece converter and cached per reading.
  std::vector<std::vector<std::string>> CollectMozcPieceOptions(
      const std::vector<mozkey::meltype::ScoredSegmenter::Segment>& pieces);
  // Rebuilds meltype_piece_plan_ and meltype_piece_choices_ for |raw|:
  // Meltype segmentation -> piece candidates -> cached SystemOne choices.
  // False when Meltype has no opinion, in which case the caller keeps Mozc's
  // own output untouched.
  bool RebuildMeltypePiecePlan(const std::string& raw, bool final);
  // Overlays Meltype's protection on |preedit|: the segments covering an
  // English piece are rewritten to the typed letters (with the boundary
  // spaces) and a cached SystemOne choice collapses its piece's segments;
  // everything else stays Mozc's own multi-clause conversion.  Returns
  // whether anything changed and records that in meltype_display_protected_.
  // False leaves |preedit| untouched.
  bool ApplyMeltypePiecePreedit(const std::string& raw, bool final,
                                commands::Preedit* preedit);
  // Cache key of one question: the piece it targets and its reading.  The
  // left context is deliberately not part of the key — it only enriches the
  // question, while an upstream piece moving off its first candidate would
  // otherwise invalidate every downstream answer and re-ask them forever.
  static std::string PieceChoiceKey(const PieceQuestion& question);
  // Applies every answer that has arrived (or timed out) to the choice cache.
  // Returns the number of newly handled answers.
  int HarvestPieceSystemOneAnswers();
  // Submits the questions the choice cache cannot answer.  No submission when
  // the feature is off or every question is already answered.
  bool MaybeSubmitPieceSystemOne(const config::Config& config,
                                 mozc::commands::Command* command);
  bool ApplySystemOneLiveDecision(mozc::commands::Command* command);
  bool ApplySystemOneLiveDecisionInternal(mozc::commands::Command* command);
  bool IsCurrentSystemOneLiveDecisionCallback(
      const mozc::commands::Command& command) const;
  void CancelPendingSystemOneDecision();
  void OutputCurrentLiveConversionForSystemOne(
      mozc::commands::Command* command);

  // Trace helpers; no-ops when tracing is off or no sequence is active.
  void TraceStart();
  void TraceContinue(uint64_t seq, absl::Time sequence_start);
  void TraceStep(absl::string_view name, const std::string& detail);
  void TraceFlush(const std::string& detail);
  void CancelPendingLiveConversion();
  void ClearLiveConversionState();
  void CancelLiveConversionForEditing();
  // Starts prediction candidate selection from a live-conversion-backed
  // CONVERSION state.  Other conversion keys keep following their existing
  // keymap-defined conversion path, while Tab-style prediction keys focus
  // prediction candidates.
  bool PredictAndConvertFromLiveConversion(mozc::commands::Command* command);
  // Adds a non-focused suggestion candidate window to live-conversion output
  // without mutating the real converter state.  This keeps live conversion as a
  // CONVERSION state for normal conversion keys while still showing passive
  // suggestions.
  bool CaptureVisiblePassiveSuggestionSnapshot(
      PassiveSuggestionSnapshot* snapshot) const;
  bool AttachPassiveSuggestionSnapshot(
      const mozc::commands::Input& input,
      PassiveSuggestionSnapshot* snapshot,
      mozc::commands::Output* output);
  bool FocusHeldPassiveSuggestion(mozc::commands::Command* command);
  bool AttachLiveConversionSuggestionCandidateWindow(
      const mozc::commands::Input& input,
      mozc::commands::Output* output);
  bool AttachCachedLiveConversionSuggestionCandidateWindow(
      mozc::commands::Output* output);
  bool CommitLiveConversionResult(mozc::commands::Command* command);
  // Commits the visible live conversion by joining the Meltype piece values
  // that produced it.  The converter's candidate list is a different
  // segmentation (and never carries the English pieces), so the ordinary
  // commit would restore a surface the user never saw.
  bool CommitMeltypePieceLiveConversion(mozc::commands::Command* command);
  std::pair<std::string, std::string>
  GetPendingLiveConversionDisplayCommitStrings() const;
  bool CommitPendingLiveConversionDisplayDirectly(
      mozc::commands::Command* command);
  bool CommitPendingLiveConversionDisplayForSubmit(
      mozc::commands::Command* command);
  bool OutputPendingLiveConversion(mozc::commands::Command* command) const;
  bool OutputPendingLiveConversionWithPresentation(
      mozc::commands::Command* command,
      const PendingLiveConversionPresentation* presentation,
      PendingLiveConversionPresentation* stable_presentation) const;
  std::vector<StablePresentationSpan> BuildStablePresentationSpans(
      absl::string_view visible_value) const;
  std::vector<StablePresentationSpan> MergeStablePresentationSpans(
      absl::string_view visible_value,
      const std::vector<StablePresentationSpan>& previous_spans) const;
  std::optional<PendingLiveConversionPresentation>
  CapturePendingLiveConversionPresentationForContinuedInput() const;
  void AttachDelayedLiveConversionCallback(
      mozc::commands::Command* command) const;

  bool ResumeCommandSequence(mozc::commands::Command* command);
  // The single Output::Callback slot serves the async chain; whatever is
  // still pending re-attaches when a handler fires.
  void AttachPendingAsyncCallback(mozc::commands::Command* command) const;
  bool SetPendingDirectCommitLearning(
      absl::string_view key,
      absl::string_view value,
      absl::string_view reason);
  bool SetPendingDirectCommitLearningFromCommittedResult(
      const mozc::commands::Command& command,
      absl::string_view reason);
  void ConfirmPendingDirectCommitLearning(absl::string_view reason);
  void DiscardPendingDirectCommitLearning(absl::string_view reason);
  void HandlePendingDirectCommitLearningForKeyEvent(
      const mozc::commands::KeyEvent& key);
  void HandlePendingDirectCommitLearningForSessionCommand(
      mozc::commands::SessionCommand::CommandType type);

  // Fill command's output according to the current state.
  void OutputFromState(mozc::commands::Command* command);
  void Output(mozc::commands::Command* command);
  void MaybeUpdateCandidateActionFooter(mozc::commands::Output* output) const;
  void OutputMode(mozc::commands::Command* command) const;
  void OutputComposition(mozc::commands::Command* command) const;
  void OutputKey(mozc::commands::Command* command) const;

  bool ExecuteCommandSequence(
      const mozc::keymap::CommandSequence& command_sequence,
      mozc::commands::Command* command);

  bool ExecuteCommandSequenceWithInitialOutput(
      const mozc::keymap::CommandSequence& command_sequence,
      const mozc::commands::Output* initial_output,
      mozc::commands::Command* command);

  bool ExecuteDirectInputCommand(
      mozc::keymap::DirectInputState::Commands key_command,
      mozc::commands::Command* command);

  // An IME ON key can restore hiragana from temporary Shift ASCII without
  // committing composition. Keep other already-on input modes unchanged.
  bool IMEOnWhenAlreadyOn(mozc::commands::Command* command);

  bool ExecutePrecompositionCommand(
      mozc::keymap::PrecompositionState::Commands key_command,
      mozc::commands::Command* command);

  bool ExecuteCompositionCommand(
      mozc::keymap::CompositionState::Commands key_command,
      mozc::commands::Command* command);

  bool ExecuteConversionCommand(
      mozc::keymap::ConversionState::Commands key_command,
      mozc::commands::Command* command);

  bool ExecuteCommandName(const std::string& command_name,
                          mozc::commands::Command* command);

  bool SendKeyDirectInputState(mozc::commands::Command* command);
  bool SendKeyPrecompositionState(mozc::commands::Command* command);
  bool SendKeyCompositionState(mozc::commands::Command* command);
  bool SendKeyConversionState(mozc::commands::Command* command);

  bool MoveCursorToEndInternal(mozc::commands::Command* command,
                               bool clear_undo);

  // update last_command_time;
  void UpdateTime();

  // update preferences only affecting this session.
  void UpdatePreferences(mozc::commands::Command* command);

  // Modify input of SendKey, TestSendKey, and SendCommand.
  void TransformInput(mozc::commands::Input* input);

  // ensure session status is not DIRECT.
  // if session status is DIRECT, set the status to PRECOMPOSITION.
  void EnsureIMEIsOn();

  // return true if |key_event| is a triggering key_event of
  // AutoIMEConversion.
  bool CanStartAutoConversion(const mozc::commands::KeyEvent& key_event) const;

  // return true if the pending live conversion display should be committed
  // directly after inserting the configured punctuation/symbol key.
  bool CanDirectCommitPendingLiveConversionBeforeInsert(
      const mozc::commands::KeyEvent& key_event) const;

  // return true if the current composition should be committed directly
  // after typing the configured punctuation/symbol key.
  bool CanDirectCommitAfterPunctuation(
      const mozc::commands::KeyEvent& key_event) const;

  // Handles KeyEvent::activated to support indirect IME on/off.
  bool HandleIndirectImeOnOff(mozc::commands::Command* command);

  // Commits the raw text of the composition.
  bool CommitRawText(commands::Command* command);
};

}  // namespace session
}  // namespace mozc

#endif  // MOZC_SESSION_SESSION_H_
