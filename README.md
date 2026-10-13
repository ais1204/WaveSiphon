# WaveSiphon

Chrome などで流れている音を、**そのままの音質で** WAV ファイルに録音する Windows 用の小さなアプリです。

- **アプリ単位で録音**：「Google Chrome」を選べば Chrome の音だけを録音します。他のアプリの音や Windows の通知音は入りません（「PC全体の音」も選べます）。
- **劣化なし**：マイクやステレオミキサーを通さず、Windows のオーディオエンジンからデジタルのまま取り出して WAV で保存します。
- **軽い**：インストール不要の exe 1 つだけ。常駐やバックグラウンド処理はありません。
- 無音の区間も時間どおりに記録（曲間が詰まりません）
- 4 GB を超える長時間録音は自動でファイルを分割（`_part2.wav` …）
- 録音中は PC がスリープしないようにします
- 録音中に出力デバイス（スピーカー/ヘッドホン）を切り替えても録音を継続
- 「常に最前面に表示」で、Chrome の後ろにウィンドウが隠れないようにできます

## 動作環境

- Windows 11（64bit）
  - 「アプリ単位の録音」は Windows のプロセスループバック機能を使います。Microsoft のドキュメント上の要件は Build 20348 以降のため、通常の Windows 10（22H2 = Build 19045）は対象外です。

## ダウンロード

**https://github.com/ais1204/WaveSiphon/releases/latest/download/WaveSiphon.exe**

このリンクを開くと、最新版の `WaveSiphon.exe` を直接ダウンロードできます（インストール不要・GitHub のアカウント不要）。
[Releases ページ](https://github.com/ais1204/WaveSiphon/releases/latest) からもダウンロードできます。

> 署名していない exe のため、次のような警告が出ることがあります。
> - ダウンロード時にブラウザーが警告を表示した場合：ダウンロード一覧（Ctrl+J）からファイルを保存してください。
> - 初回起動時に「Windows によって PC が保護されました」と表示された場合：「詳細情報」→「実行」で起動できます。

新しいバージョンを公開するには、`v1.0.1` のようなタグを push します。GitHub Actions がビルドして、Releases に exe を自動で添付します。
コミットごとのビルドは、**Actions** タブの各実行の **Artifacts** からも取得できます（GitHub へのログインが必要）。

## 使い方

1. Chrome で録音したい音（動画・音楽など）を開いておく
2. `WaveSiphon.exe` を起動
3. 「録音する音」で **Google Chrome** を選ぶ（一覧はクリックするたびに最新になります）
4. 「保存形式」を選ぶ（迷ったら **WAV 24bit**）
5. **● 録音開始** を押してから再生 → 終わったら **■ 録音停止**
6. 「開く」で保存先フォルダ（既定：`ミュージック\WaveSiphon`）が開きます

「常に最前面に表示」にチェックを入れると、ほかのウィンドウの上に表示されたままになります。
録音する音・保存形式・保存先・この設定は、次回起動時にも引き継がれます。

ファイル名は `chrome_2026-10-12_153045.wav` のように、アプリ名と日時になります。
録音は最初の音が届いたところから始まり、何も音が来なかった場合はファイルを作りません。

### 保存形式

| 形式 | 用途 | 1 時間あたりの容量（48 kHz ステレオ） |
| --- | --- | --- |
| WAV 24bit（おすすめ） | 高音質。ほぼすべての再生・編集ソフトで開ける | 約 990 MB |
| WAV 16bit | CD と同じ形式。互換性重視 | 約 660 MB |
| WAV 32bit float | Windows 内部の音声データをビット単位でそのまま保存。編集向け | 約 1.3 GB |

サンプリングレートは、使っている出力デバイスの設定（多くは 48 kHz）に合わせます。

## よくある質問

**Q. 音が録音されない / 「音が流れるのを待っています…」のまま**
Chrome のタブがミュートになっていないか、音量が 0 になっていないか確認してください。
Chrome を選んでいる場合、Chrome 以外のアプリ（Edge など）の音は録音されません。

**Q. Windows の音量設定で、録音される音は変わる？**
「録音する音」で Chrome などのアプリを選んでいる場合：

| 設定 | 録音への影響 |
| --- | --- |
| Chrome の中の音量（YouTube のプレイヤー音量、タブのミュートなど） | **影響する**（Chrome が出す音そのものが変わるため） |
| Windows の「音量ミキサー」の Chrome 個別の音量・ミュート | **影響する**（50% にすると録音も小さくなります） |
| Windows 全体の音量（タスクバーの音量） | 影響しない |

元の音のまま録音するには、Chrome 内の音量と音量ミキサーの Chrome を 100% にしておき、聞く大きさは Windows 全体の音量で調整してください。
「PC全体の音」を選んだ場合は、Windows 全体の音量の影響を受ける環境もあります。
サンプリングレートは、出力デバイスのプロパティの「既定の形式」に従います。

**Q. Chrome を再起動したら？**
Chrome を終了すると録音は自動で停止し、それまでの分は保存されます。
Chrome を起動し直してから、もう一度「録音開始」を押してください。

**Q. 拡張機能ではだめなの？**
Chrome 拡張機能（`chrome.tabCapture` API）でもタブの音は録音でき、作り方次第で無劣化にもできます。
ただし、録音できるのは 1 タブだけ・開始のたびに拡張機能ボタンのクリックが必要・ファイルへの保存はブラウザー経由になる、などの制約があります。
このアプリは Windows の機能で Chrome 全体（すべてのタブ）の音を直接取り出し、そのままディスクに書き込みます。

録音した音声は、著作権法やサービスの利用規約の範囲内（私的利用など）で使ってください。

## ビルド方法

### Visual Studio（Windows）

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
```

`build\Release\WaveSiphon.exe` ができます。

### MinGW-w64（Linux からのクロスビルド）

```sh
sudo apt install g++-mingw-w64-x86-64-posix
./build-mingw.sh
```

`build-mingw/WaveSiphon.exe` ができます。

### テスト

WAV 書き出しとサンプル変換のテスト（GitHub Actions でも Windows 上で実行しています）：

```bat
cmake -S . -B build -A x64 -DWAVESIPHON_BUILD_TESTS=ON
cmake --build build --config Release
build\Release\wav_writer_test.exe wavtest
python tests\check_wav.py wavtest
```

## 仕組み

| ファイル | 内容 |
| --- | --- |
| `src/recorder.cpp` | WASAPI で音を取り込むスレッド。アプリ単位はプロセスループバック（`ActivateAudioInterfaceAsync` + `VAD\Process_Loopback`、子プロセスを含む）、PC全体は既定の出力デバイスのループバック |
| `src/wav_writer.cpp` | WAV への逐次書き込み。ヘッダを定期的に更新するので、強制終了しても途中まで再生可能 |
| `src/sample_convert.h` | サンプル形式の変換（16bit 化の際は TPDF ディザ） |
| `src/audio_apps.cpp` | 音を出しているアプリの一覧。Chrome の音声プロセスは親のブラウザープロセスにまとめます |
| `src/main.cpp` | 画面（Win32 API のみ、高 DPI 対応） |
