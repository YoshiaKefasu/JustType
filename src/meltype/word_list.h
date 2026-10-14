// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The Mozkey Authors
//
// Meltype (GPL-3.0-or-later, src/Meltype.Core/Detection/WordList.cs) から移植した
// 単語辞書。ローマ字入力中の「この綴りは英単語か」「英単語の途中か」をキーフックの
// スレッドから引くので、どちらも平均 O(1) のハッシュ参照で答える。

#ifndef MOZKEY_MELTYPE_WORD_LIST_H_
#define MOZKEY_MELTYPE_WORD_LIST_H_

#include <cstddef>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace mozkey {
namespace meltype {

// 単語集合と、そのすべての prefix の集合。
//
// 語そのものと prefix を別のハッシュセットに持つ (Meltype の WordList と同じ)。
// 文字列は 1 塊 (block) にまとめて持ち、キーはその塊を指す string_view にする。
// 1 語ごとに std::string を持つと数万語でも数 MB になるので、ここでは辞書ファイル
// とほぼ同量のメモリで済ませている (16 バイトの string_view + ハッシュノード 1 個)。
class WordList {
 public:
  WordList() = default;
  // コピーは禁止。キーは自分の blocks_ を指す string_view なので、コピーすると
  // コピー元のバッファを指したキーができてしまう (ムーブなら blocks_ ごと移るので安全)。
  WordList(const WordList&) = delete;
  WordList& operator=(const WordList&) = delete;
  WordList(WordList&&) = default;
  WordList& operator=(WordList&&) = default;

  // 語を 1 つ追加する。空語・重複は無視。語のすべての prefix も同時に登録する。
  // 戻り値は新規に登録できたか。
  bool AddWord(std::string_view word);

  // text と一致する語があるか。
  bool ContainsWord(std::string_view text) const { return words_.find(text) != words_.end(); }

  // text で始まる語があるか (text 自体が語のときも true)。
  bool HasPrefix(std::string_view text) const {
    return prefixes_.find(text) != prefixes_.end();
  }

  std::size_t WordCount() const { return words_.size(); }

  // 登録した語の一覧 (読み込み時だけ使う。順序は不定)。
  std::vector<std::string> Words() const;

  // 空白・タブ・カンマ区切りの語を読み込む。# 以降はコメント。
  // 英小文字以外を含む語は無視する (大文字は小文字にしてから渡すこと)。
  // 戻り値は追加できた語数。
  std::size_t AddText(std::string_view text);

  // ファイルを読んで AddText する。読めなければ 0。
  std::size_t AddFile(const std::string& path);

  // AddText と同じ但し、すべて小文字にしてから追加する (固有名詞の一覧など)。
  std::size_t AddTextLowercased(std::string_view text);

 private:
  // block (blocks_ の要素) の [offset, offset + length) を指すキーとして登録する。
  // block は blocks_ の要素なので、この string_view は deque が動かない限り有効。
  // 戻り値は新規に登録できたか。
  bool AddWordOwned(const std::string& block, std::size_t offset, std::size_t length);

  // 所有する文字列の塊。deque は push_back でも既存要素の参照が動かないので、
  // 塊を指す string_view を安全にハッシュのキーにできる。
  std::deque<std::string> blocks_;
  std::unordered_set<std::string_view> words_;
  std::unordered_set<std::string_view> prefixes_;
};

}  // namespace meltype
}  // namespace mozkey

#endif  // MOZKEY_MELTYPE_WORD_LIST_H_
