# mozkey-jev-bridge

Mozkey の ZNZ1 名前付きパイプを中継して、OpenCode Zen の `jev-1.13-free` に
日本語変換の曖性解消をさせ、結果を Mozkey へ返す Go 製ブリッジ。
入出力は JSONL に追記され、将来の Laya ファインチューニング用データセットになる。

```
Mozkey ──ZNZ1──> \\.\pipe\mozc_zenz_scorer ──> mozkey-jev-bridge
                                                    │
                        Mozc OSS 辞書の候補引き ──────┤
                                                    ▼
                          https://opencode.ai/zen/v1/systemone
                                                    │
                        choice + confidence <────────┘
                                                    │
                          jev_dataset.jsonl <────────┘
```

## ビルド

```powershell
cd tools\mozkey-jev-bridge
go build -o mozkey-jev-bridge.exe .
go build -o znzclient.exe .\cmd\znzclient
```

`data/candidates.tsv.gz`（Mozc OSS 辞書 + `dictionary_manual/general_override.txt` から
生成した 749,176 読みの候補インデックス）はリポジトリに同梱済み。再生成する場合:

```powershell
go run .\internal\candidates\gen -dict "..\..\src\data\dictionary_oss\dictionary0*.txt,..\..\src\data\dictionary_manual\general_override.txt" -out data\candidates.tsv.gz -max 6
```

## 実行

```powershell
$env:ZEN_API_KEY = "sk-..."   # 省略可（下記参照）
.\mozkey-jev-bridge.exe --verbose
```

起動すると `\\.\pipe\mozc_zenz_scorer` で待ち受ける。Ctrl-C でデータセットを
フラッシュして終了する。

### フラグ

| フラグ | 既定値 | 説明 |
| --- | --- | --- |
| `--pipe` | `\\.\pipe\mozc_zenz_scorer` | 待ち受けるパイプ名 |
| `--zen-key` | `$ZEN_API_KEY` | Zen API キー。空なら `Authorization` ヘッダー自体を送らない |
| `--zen-endpoint` | `https://opencode.ai/zen/v1/systemone` | System One エンドポイント |
| `--zen-model` | `jev-1.13-free` | モデル ID |
| `--session-id` | `ses_019a00000000AbCdEfGhIjKlMn` | `x-session-id` 等に載せるセッション ID |
| `--http-timeout` | `2500ms` | 1 回の Jev 呼び出しの上限 |
| `--log-path` | 実行ファイル隣の `jev_dataset.jsonl` | データセット出力先 |
| `--candidates` | 実行ファイル隣の `data\candidates.tsv.gz` | 候補インデックス |
| `--max-options` | `6` | Jev に渡す候補の最大数 |
| `--dry-run` | `false` | Jev を呼ばず辞書の最上位を返す（配線確認用） |
| `--verbose` | `false` | リクエストごとにログを出す |

### API キーについて

`jev-1.13-free` はキー無しでも応答する（実測済み）。キーを設定しない場合は
`Authorization` ヘッダーを送らないので、空キーが `Bearer ` になる事故はない。
**キーはソースに直書きしない。** このリポジトリは GitHub へ push される。

## 動作確認

Mozc ビルドなしで、Mozkey クライアントと同じ ZNZ1 フレームを叩く:

```powershell
.\znzclient.exe -reading てんき -left "きょうは いい" -right "ですね"
```

追加の検証用フラグ:

```powershell
.\znzclient.exe -reading てんき -corrupt-magic   # プロトコル違反時のエラー応答を確認
.\znzclient.exe -stall 15s                        # I/O watchdog を確認（何も書かずに保持）
```

実測結果（同一 `てんき` でも文脈で選択が変わる）:

| left | right | value | confidence | latency |
| --- | --- | --- | --- | --- |
| きょうは いい | ですね | 天気 | 0.82 | 1083ms |
| 会議の内容を | する | 転記 | 0.76 | 472ms |
| あしたの | よほう | 今日 | 0.88 | 468ms |

テスト:

```powershell
go test ./...
go test -race ./...
go test -race ./internal/pipe/   # 実名前付きパイプの E2E と watchdog
```

## 設計上の判断

### 候補は Mozc OSS 辞書から引く

ZNZ1 プロトコルには候補が乗っていない。`zenz_prompt_builder.cc` が生成する
prompt は `\uee02` 左文脈 `\uee07` 右文脈 `\uee00` カタカナ読み `\uee01` のみで、
Mozc の変換候補は含まれない。Jev の `choice` は criteria のキーを選ぶので、
キーをこちらで用意する必要がある。

`src/data/dictionary_oss/dictionary0*.txt`（`reading<TAB>LID<TAB>RID<TAB>cost<TAB>surface`）
をコスト昇順で読み、読みごとに上位 6 面をインデックス化した。コストは Mozc 自身の
候補順位そのものなので、辞書の首位は実用上の首位と一致する（`てんき` → `天気`、
`わたし` → `私`）。

**副作用として、このブリッジは Mozc 自身の候補順位を辞書の順位で上書きする。**
曖昧な読みでは Jev が辞書の上位 6 面から選ぶので、Mozc が 7 位以下に出していた
候補は選ばれない。これは「Jev に決めさせる」要件の帰結で、プロトコル上
Mozc の候補列は取得できない。

### 3 つの経路は Jev を呼ばない

| 条件 | 返り値 | debug |
| --- | --- | --- |
| prompt に読みが無い | status=Error, 空 | `no_reading` |
| 辞書に 1 面しか無い（曖昧でない） | その 1 面 | `single_candidate` |
| 辞書に読みが無い | 入力どおりのひらがな | `no_candidates` |

曖昧でない読みで Jev を呼んでも辞書の答えが変わるだけなので、レイテンシと
コストを無駄にしない。辞書に無い読みは「選ぶものがない」ので、推測で漢字を
捏ねるよりひらがなを返して Mozkey 側の変換に任せる（この場合は `options: []` が
データセットに残り、辞書カバレッジの測定に使える）。

### 直列処理

1 接続ずつ `create → connect → read → process → write → flush → disconnect` を
回す。`src/zenz_scorer/main.cc` と同じ形。Mozkey は 1 リクエストを投げて応答を
待つだけなので、連続した接続は追加のパイプインスタンスでキューイングされ、
クライアント側 3.5 秒のフェイルセーフが通常の変換フォールバックに落とす。
並列化する場合は `internal/pipe/server.go` の `serveConnection` を goroutine に
すればよいが、同時実行数を絞らないとレート制限に当たる。

### I/O watchdog

非 overlapped ハンドルの同期 ReadFile は、クライアントが繋いだまま何も書かないと
**無限にブロックする**。accept ループが単一スレッドなので、1 つの滞留クライアントで
ブリッジ全体が永久に停止する。`internal/pipe/io_deadline.go` は
`src/zenz_scorer/synchronous_io_deadline.h` の移植で、これを防ぐ。

| 対象 | タイムアウト | 根拠 |
| --- | --- | --- |
| read / process / write | 8000ms | `kPipeIoSafetyTimeoutMsec` = 1500 + 5000 + 1500 |
| `FlushFileBuffers` | 1000ms | `kPipeFlushSafetyTimeoutMsec` |

発火時は `CancelIoEx` と `CancelSynchronousIo` を 10ms 間隔で繰り返す。同期 ReadFile を
止められるのは `CancelSynchronousIo` だけなので、x/sys にエクスポートが無いこの API は
`kernel32.dll` から直接引いている。

Go 固有の注意が 2 点:

- **`Serve` は `runtime.LockOSThread()` する。** `CancelSynchronousIo` は*スレッド*を
  指定するので、goroutine が別の OS スレッドにマイグレーションすると無関係なスレッドを
  キャンセルしてしまう。サーバーループ全体を 1 スレッドに固定する。
- **watchdog は必ず join してからハンドルを閉じる。** 閉じたハンドル値は次の
  `CreateNamedPipe` で使い回されることがあり、遅れたキャンセルが後続の接続に当たる。
  `release()` がこれを行う。

実測（`znzclient -stall 15s` で何も書かないクライアントを繋いだ場合）:

```
17:02:20 serving PIPENAME
17:02:31 pipe: read request: znz: read header: ... The I/O operation has been aborted
          because of either a thread exit or an application request.
gen=901 status=OK latency=1084ms value="箸" debug="jev_ok"
```

8 秒で解除され、直後のリクエストを通常通り処理している。修正前はここで無限ブロック。

### タイムアウト予算

`zenz_named_pipe_client.h` の `kDefaultTransportSafetyTimeoutMsec = 3500` が
クライアント側の IPC フェイルセーフで、リクエストが 5 秒を要求しても 3.5 秒で
見捨てられる。既定の `--http-timeout 2500ms` はここに収まるよう引いてある。
実測レイテンシは 470〜1083ms。

### パイプの DACL

`D:P(A;;GRGW;;;<現在ユーザーSID>)(A;;GA;;;SY)`。Everyone も低整合性ラベルも
付けない。タイプした日本語テキストを外部モデルへ送るので、別アカウントや別
整合性レベルから到達できてはいけない。

### 単一インスタンス

同じパイプ名で 2 つ起動しても Windows は 2 組目のパイプインスタンスを普通に
作り、どちらにもエラーが出ない。リクエストが 2 つのバックエンドに分割される。
`Local\MozkeyJevBridgeSingleInstance` ミューテックスで 2 台目を落とす
（mozc_zenz_scorer の `Local\MozcZenzScorerSingleInstance` と同じ考え方）。

## データセット

`jev_dataset.jsonl` に 1 行 1 リクエスト:

```json
{"timestamp":"2026-10-13T09:17:37.046Z","generation":2,"left_context":"会議の内容を","reading":"てんき","right_context":"する","options":["天気","天鬼","転機","転記","転帰","てんき"],"choice":"転記","confidence":0.76,"latency_ms":472}
```

`options` は常に配列（空でも `[]`）。失敗したリクエストも記録されるので、
「何を見て何を選んだか」が落ちない。1 行ごとにフラッシュするのでプロセスが
落ちても途中まで残る（fsync はしない。電源断で末尾が失われる可能性はあるが、
学習データとしては許容範囲）。

## 注意

- **タイプ内容が外部に送られる。** `jev_dataset.jsonl` とビルド物は
  `.gitignore` 済み。データセットには入力した日本語テキストが乗るので
  push しないこと。
- `tools/` 配下なので MSI や `src/bazel-bin` には入らず、
  `.github/workflows/secure-offline.yaml` のオフライン保証チェック対象外。
- **候補はリポジトリ内の辞書データのカバレッジに律速される。** `dictionary_oss`
  は最小構成で、`かいます` の候補が `買い増す / 貝増` しか無い（`買います` が
  辞書源に存在しない）など、一般的な活用形が抜ける。精度を上げるなら
  `dictionary_koyasi` の生成物（`mozcdic-ut-*.txt`、gitignore 済みで未同梱）を
  `-dict` に足すのが次の手。
- 実測精度はおおむね 4〜7 割。文脈で候補が明確に分かれる読み（`はし`→箸、
  `かみ`→紙、`あめ`→雨、`てんき`→天気/転記）は安定して当たるが、`わたし` が
  ひらがなのまま残るなど、free モデルの弱いケースもある。データセットの
  `confidence` がその指標になる。
- 辞書に無い読みはフォールバックするため、長い複合語や固有名词では
  ブリッジが効かない。データセットの `options: []` 行がその比率を示す。
