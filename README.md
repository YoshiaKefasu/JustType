<p align="center">
  <img src="src/data/images/icon.svg" width="112" height="112" alt="JustType icon">
</p>

<h1 align="center">JustType（ジャストタイプ）</h1>

<p align="center">
  <strong>Mozc をベースに、Meltype 英日混在ピンポイント保護・SystemOne 判断型AI・美しい内蔵フォントを統合した<br>次世代ハイブリッド日本語入力 IME</strong>
</p>

<p align="center">
  <a href="https://github.com/YoshiaKefasu/JustType/releases"><img alt="Releases" src="https://img.shields.io/github/v/release/YoshiaKefasu/JustType?include_prereleases&label=release"></a>
  <img alt="Based on Mozc" src="https://img.shields.io/badge/based%20on-Mozc-88A2DD">
  <img alt="Meltype Hybrid" src="https://img.shields.io/badge/engine-Meltype%20Hybrid-FF6B6B">
  <img alt="SystemOne" src="https://img.shields.io/badge/AI-SystemOne%20Decision-53D4C7">
  <img alt="Release build" src="https://img.shields.io/badge/release-Windows%20MSI-178B8B">
</p>

<br>

**JustType（ジャストタイプ）** は、[google/mozc](https://github.com/google/mozc) をベースに開発された次世代の日本語入力システム（IME）です。

日常のプログラミング、チャット、執筆において「英語と日本語の混在入力」「タイポ（誤字）」「文脈に応じた同音異義語の選択」を、**0ms の軽快な打鍵感** と **ハルシネーション（捏造）ゼロの判断型 AI** によって快適に解決します。

<br>

主な特長
--------

### 1. Meltype 英日混在ピンポイント保護
従来の IME では、ローマ字入力中に英単語が混ざると「後おgぇで」「ファセボおk」のように不自然な漢字やカタカナに化けてしまう問題がありました。
JustType は入力されたローマ字列を解析し、英語部分をピンポイントで英字のまま保護します。

- `kyouhagooglede` → **`今日は google で`**
- `anofacebook` → **`あの facebook`**
- `dlsiteni` → **`DLsite に`**
- `discordsa-ba` → **`discord サーバ`**
- `stopshita` → **`stop した`**
- `minutetteiuego` → **`minute っていう英語`**

後ろに助詞（「で」「に」「は」など）や助動詞（「した」「して」など）が続いても、英語が日本語に巻き込まれたり、逆に日本語が英語に吸い取られたりすることなく、自然な境界スペース付きで滑らかに入力できます。

### 2. SystemOne 判断型 AI（Jev / Laya）完全非同期統合
一般的な生成型 LLM（文章を自由に生成するモデル）とは異なり、辞書候補やタイポ救済候補の中から「前後の文脈に最も合致する表記を 1 つ選択する」**判断型決定モデル（Decision Model）** を採用しています。

- **ハルシネーションゼロ**: 提示された正当な選択肢の中からのみ判定するため、AI が変な文字や記号を捏造する事故が原理的に 100% 発生しません。
- **入力遅延 0ms**: キー入力パスには推論や通信を一切挟まず、Mozc のデフォルト辞書候補を 0ms で即座に画面へ描画。AI の判定結果はバックグラウンドワーカースレッドから非同期に届き、画面の候補をインプレースで最新化します。

### 3. Gen Interface JP Display フォント同梱
候補ウィンドウおよびライブ変換ルビ表示の視認性を極限まで高めるため、モダンな UI ディスプレイフォント **`Gen Interface JP Display`**（Regular / Bold）をインストーラーに同梱。
DirectWrite および GDI 双方で美しくアンチエイリアス描画されます。

### 4. 下位互換プロファイル動的切替
設定や学習データの保存先（`AppData\LocalLow`）を自動判別します：
- **新規インストール環境**: 美しい `AppData\LocalLow\JustType` を新規作成して使用。
- **既存の Mozc / Mozkey 環境**: 既存の `AppData\LocalLow\Mozc` をそのまま引き継ぎ、辞書や学習履歴を完全保持。

### 5. 徹底的な DEBLOAT と軽量化
肥大化の原因となっていたローカル GGUF モデルや旧式のライブ補正パイプライン（40ファイル以上）を完全に撤去。インストーラーのファイルサイズを **約 35MB** まで大幅に軽量化しました。

<br>

ダウンロード / インストール
--------------------------

Windows 向けのインストーラー（MSI）は [Releases](https://github.com/YoshiaKefasu/JustType/releases) からダウンロードできます。

### インストール方法
`JustType64.msi` をダウンロードし、ダブルクリックするか以下のコマンドを実行してください：

```powershell
Start-Process msiexec.exe -ArgumentList "/i JustType64.msi /norestart" -Verb RunAs
```

※ インストーラーには `REBOOT=ReallySuppress` が設定されており、インストール完了時に OS の勝手な再起動は一切発生しません。

<br>

ビルド方法 (Windows)
--------------------

### 必要環境
- Windows 10 (1809 以降) / Windows 11 (x64)
- Visual Studio Build Tools (C++ ATL コンポーネント必須)
- Python 3.x
- .NET SDK (WiX v5 パッケージング用)
- Bazel (bazelisk 経由で USE_BAZEL_VERSION=9.0.2 を推奨)

### ビルドコマンド
```powershell
# 一時ディレクトリおよび環境変数の設定
$env:TEMP = "D:\OSS_ProgramFiles\tmp"
$env:TMP = "D:\OSS_ProgramFiles\tmp"
$env:DOTNET_ROOT = "D:\OSS_ProgramFiles\tools\dotnet"
$env:PATH = "D:\OSS_ProgramFiles\tools\dotnet;D:\OSS_ProgramFiles\tools\bin;" + $env:PATH

# MSI インストーラーのビルド
Set-Location "src"
bazelisk --batch build package --config release_build
```

ビルド完了後、`src/bazel-bin/win32/installer/JustType64.msi` に成果物が生成されます。

<br>

アーキテクチャ詳細
------------------
パイプラインの詳細な設計仕様書については [docs/ARCHITECTURE_SYSTEMONE_FINAL.md](docs/ARCHITECTURE_SYSTEMONE_FINAL.md) を参照してください。

<br>

ライセンス
----------
本プロジェクトは [google/mozc](https://github.com/google/mozc) のオープンソースライセンス（BSD 3-Clause）および [Meltype](https://github.com/koyasi777/Meltype) のライセンスに基づいて公開されています。詳細については [LICENSE](LICENSE) および [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) を参照してください。
