# src/meltype/

半角・全角キーを押さずに英日を混在入力するための、ローマ字入力の自動分割モジュール。
Meltype (`A:/Meltype`, GPL-3.0-or-later) の `ScoredSegmentation` / `IsEnglishSpan` を C++20 に移植したもの。

`kyouhagoogledekensaku` を打つと、リアルタイムで
`きょうは` / `google` / `でけんさく` のように最適な境界で分割する。
日本語の区間のかなは、このあと Mozc の変換エンジンが漢字にする (`でけんさく` → `で検索`)。

## ファイル

| ファイル | 中身 |
| --- | --- |
| `word_list.h` / `word_list.cc` | 単語と、そのすべての prefix を引く辞書。どちらも平均 O(1) |
| `romaji_detector.h` / `romaji_detector.cc` | ローマ字として成立するか、かな・促音の数え上げ |
| `scored_segmenter.h` / `scored_segmenter.cc` | 動的計画法による英日最適分割 (`ScoredSegmenter::SegmentInput`) |
| `meltype_segmenter_test.cc` | 代表的な混在入力の確認 |

## ビルドと実行

```sh
clang++ -std=c++20 -Isrc \
    src/meltype/meltype_segmenter_test.cc \
    src/meltype/scored_segmenter.cc \
    src/meltype/romaji_detector.cc \
    src/meltype/word_list.cc \
    -o meltype_segmenter_test
./meltype_segmenter_test
```

辞書は `MELTYPE_DICT_DIR` (環境変数) → このディレクトリの `dictionaries/` →
`A:/Meltype/dictionaries` (参照実装) の順に探す。

## 設計

### どこで区切るか (動的計画法)

`best[i][e]` = 位置 `i` まで区切ったときの一番点の高い区切り方 (`e` は「直前の区間が
`i` で終わる英語の区間か」)。各位置で「日本語として 1 音進む」「英語の区間として
`i` から `j` まで取る」「英単語の最後の子音と助詞の `は` が 1 音 (`sha`) になっている
ときはそこで分ける」の 3 手を試し、最後に点の高いほうを取る。

先頭から見て最初に英語と言える区間を取ると戻れない (`some|teal|coholic`) ので、
あり得る区切り方を全部並べて点数で選ぶ (変換エンジンと同じ考え方)。

点の重みは `scored_segmenter.cc` の先頭の `constexpr`:

- 知っている英単語: 長さの 2 乗 (長い 1 語を短い 2 語より優先: `mo|string`)
- 辞書に無い英字の並び (`buglowers`): 長さに比例するだけ (前の日本語を巻き込まない)
- 英語の区間を 1 つ置くたび `-0.5`、英字どうしがくっついた 2 語は `-4.0`
- 日本語の区間に読めない英字が 1 文字残るたび `-1.0`

### 英語かどうか (`IsEnglishSpan`)

「ローマ字として成立しない綴り (th, l, v, 子音連続 …) は辞書が無くても英語」
「日本語としても読める語 (sushi, repo, fine) は前後の文脈で決める」など、
Meltype の `CompositionDetector.IsEnglishSpan` の規則をそのまま移植してある。

### メモリ

語と prefix は 1 つの `std::string` の塊 (block) にまとめて持ち、ハッシュのキーは
その塊を指す `std::string_view`。1 語ごとに `std::string` を持つと 4 万語で 10 MB 単位に
なるが、こうすると辞書ファイルとほぼ同量 (1 MB 程度) で済む。`std::deque` は
`push_back` しても既存要素の参照が動かないので、キーが突然の再確保で宙に浮くことがない。

O(1) より省メモリを優先するなら、語をソートした `std::vector<std::string_view>` に
2 分探索でも足りる (4 万語で 18 回ほどの比較)。

## 移植していないもの

Meltype の `CompositionDetector` は 2500 行あり、かな変換ボックス・打ち間違い直し・
ユーザー辞書・言語記憶などと一体になっている。このモジュールは区間分けだけを受け持つので、
次は移植していない (必要になったら足す):

- スペルチェッカー (Windows のもの)。`english-words.txt` を「弱い根拠」として代用
- 打ち間違い直し (`RomajiTypoCorrector`)、学習した語 (`LanguageMemory`)
- URL・ユーザー名・大文字始まりの語・数字を挟んだ略語 (`i18n`) などの「はっきりした決まり」
- ユーザーのローマ字の表 (`romaji.txt`, AZIK など)
- 強拗音・長音の数え上げ (`RomajiDetector.Analysis` の `strong_youon` など)

Meltype からの違いが 3 か所ある:

- 英文の中の空白を英語の区間に付ける (`hello world` が 1 つの英語の区間になる)。
  Meltype は空白を日本語の区間のまま通して、確定時に間を空ける
  (`AutomaticEnglishSpacing`) が、このモジュールには確定時の層が無いので。
- 英単語 + は に分かれるところの 1 音 (`medals` の後の `sha`) は、それだけで英単語に
  しない。Meltype は `IsEnglishSpan` が「前に英単語があるので英語」と通してしまうため
  (`sha` は SHA-1 として英語辞書に入っている)、点数的には `medalssha` が勝ってしまう。
  Meltype 側の `ScoredSegmentationTests.ParticleHa_AfterEnglishWord` は望ましい動きとして
  書かれているが、テストランナー (`Program.cs`) に登録されておらず未検証。このモジュールは
  助詞の分離を優先する (`split_particle_`)。
- `IsCommonJapanese` (よく使う日本語の語) は常に false。日本語の語の判定は
  `japanese.txt` だけを見る。

## 速度

区間の候補を全部調べるので、入力が長いと 3 乗に近い勢いで増える。実測 (`-O2`、1 回の
`SegmentInput` の平均):

| 入力の長さ | 時間 |
| --- | --- |
| 21 文字 | 0.05 ms |
| 56 文字 | 0.39 ms |
| 122 文字 | 2.8 ms |

変換ボックスの中身はふつう数十文字までなので、これで足りる。もっと長い入力で困るようなら、
`RomajiDetector` に Meltype と同じ解析結果のキャッシュ (`_strictCache` など) を入れる。
ただし `Analysis` のかなは入力の `string_view` を指しているので、キャッシュを持つなら
`Token` のかなを持ち方を含めて見直す必要がある。

## ライセンス (要確認)

アルゴリズムと辞書データは Meltype (GPL-3.0-or-later) 由来。このリポジトリは Mozc 由来の
Apache-2.0 なので、**このモジュールを配布物に入れるにはライセンスの確認が必要**。
辞書はビルド時に読み込むだけ (リポジトリには入れていない) が、コード自体も移植なので
同じ扱い。配布するなら、英語の語の一覧は SCOWL など互換のライセンスのものに差し替える。

## 次の作業

- Bazel のターゲット (`BUILD.bazel`) を足して、既存のビルドに乗せる
- `composer/` の変換ボックスとつなぐ (`SegmentInput` の戻り値をそのまま表示に使う)
- 辞書を Apache 互換のものに差し替える
