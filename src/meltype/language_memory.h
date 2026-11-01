// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The Mozkey Authors
//
// Meltype (GPL-3.0-or-later, src/Meltype.Core/Composition/LanguageMemory.cs) から
// 移植した、ユーザーが確定した語の軽量な実行時メモリ。
//
// 原本は「英語とも日本語とも読める語を、ユーザーが自分で英字 / かなに直したら
// 覚えておく」ためのもの。ここではそのうち「英字で確定した語を英語として覚える」
// だけを移植している: 同梱辞書に無い語 (Kasou、チャppiー の ppi 以外の部分) は
// 何度確定しても次の入力で日本語に戻ってしまうので、確定を証拠として覚える。
//
// 永続化はしない (原本は %LOCALAPPDATA%\Meltype\languages.json に書くが、ここは
// プロセスの寿命ぶんだけの実行時追加)。サーバーは常駐するので、一度確定すれば
// そのセッション中は効き続ける。
//
// ただし常駐ぶんだけ際限なく育ってはいけない: 書き込みはどちらの表も
// copy-on-write で丸ごと複製するので、件数が増えるほど確定 1 回のコストが
// 単調に上がり、メモリも解放されない。そこで kMaxEntries で有界にし、
// あふれたら最後に使われたのが最も古いものから捨てる。

#ifndef MOZKEY_MELTYPE_LANGUAGE_MEMORY_H_
#define MOZKEY_MELTYPE_LANGUAGE_MEMORY_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace mozkey {
namespace meltype {

// ユーザーが英字で確定した語の集合。
//
// 読み側 (セグメンタ) はキー入力のたびに何度も引くので、書き込みのたびに
// 表ごと差し替えて読み手はロックを取らずに済ませる (copy-on-write)。
// 書き込みは確定のときだけで、読み取りに比べて桁違いに少ない。
class LanguageMemory {
 public:
  // それぞれの表が覚えておく件数の上限。
  //
  // 確定のたびに表を丸ごと複製するので、ここを有界にしないと常駐サーバーの
  // 確定コストとメモリが単調増加する。1000 件は 1 件あたり数十バイトでも
  // 数十 KB に収まり、日常の使用ではまず到達しない。
  static constexpr std::size_t kMaxEntries = 1000;

  // |word| を英字の語として覚える。英字以外を含む語は無視する。
  // 同じ語を何度覚えても集合は変わらないので、その場合は何もしない。
  void RememberEnglish(std::string_view word);

  // |lower| (小文字化済み) を英字の語として覚えているか。
  bool IsEnglish(std::string_view lower) const;

  // ユーザーが候補窓から選んで確定した表記を覚える (読み -> 表記)。
  //
  // 「ド忘れ」「色は」のように、Mozc の素の変換では第 1 候補にならない表記を
  // 一度選んだら、次から同じ読みでその表記をライブ変換の先頭に出す。Mozc の
  // UserHistoryPredictor は学習するが、ライブ変換のプレディット先頭には
  // 出てこないので、ここで覚えておく。
  void RememberSelection(std::string_view reading, std::string_view surface);

  // |reading| に対して覚えている表記。無ければ nullptr。
  const std::string* LookupSelection(std::string_view reading) const;

  std::size_t SelectionCount() const;

  std::size_t Count() const;

 private:
  // 追い出しの順番を決めるための、最後に触った順の通し番号。
  // 値が大きいほど新しい。|write_mutex_| が守る。
  using UsedAt = std::uint64_t;

  struct Selection {
    std::string surface;
    UsedAt used = 0;
  };

  using WordMap = std::unordered_map<std::string, UsedAt>;
  using SelectionMap = std::unordered_map<std::string, Selection>;

  mutable std::mutex write_mutex_;
  UsedAt next_used_ = 1;
  std::atomic<std::shared_ptr<const WordMap>> words_{
      std::make_shared<const WordMap>()};
  std::atomic<std::shared_ptr<const SelectionMap>> selections_{
      std::make_shared<const SelectionMap>()};
};

// プロセスで 1 つのインスタンス。Session が確定時に書き、セグメンタと
// MeltypeBridge が読む。
LanguageMemory& SharedLanguageMemory();

}  // namespace meltype
}  // namespace mozkey

#endif  // MOZKEY_MELTYPE_LANGUAGE_MEMORY_H_
