# CharaDock ESP32

[CharaDock](https://github.com/ochisamu/CharaDock) 用の音声・キャラクターデバイス向けファームウェアです。会話処理はPC版CharaDockが担当し、ESP32デバイスはマイク、スピーカー、ボタン、LEDや画面などの物理的な入出力を担当します。

[English](./README.md)

## 対応状況

| デバイス | 入力 | 出力 | 状況 |
| --- | --- | --- | --- |
| M5Stack ATOM Echo（旧ESP32版） | ボタン／ハンズフリーVAD | 内蔵スピーカー＋RGB LED | 対応済み |
| StackChan | 検討中 | 検討中 | 実機到着後に評価予定 |
| M5Stack RLCD 4.2 | 検討中 | 検討中 | 実機到着後に評価予定 |

デバイス固有のコードを `firmware/<device>` に分け、将来の機種も同じリポジトリと共通ホストプロトコルで管理します。最初の正式版は **M5Stack ATOM Echo / ATOM Voice C008-Cの旧ESP32-PICO-D4版** と、Windows版CharaDock v0.5.0の組み合わせを対象にしています。

## できること

- USBでの初期設定と音声フォールバック
- 同一プライベートWi-Fi内での認証付き音声転送
- プッシュ・トゥ・トーク／ハンズフリーVAD
- PC側で選んだChat／Work
- 通常TTS、GPT-Live、Beatrice 2変換後の音声再生
- CharaDock画面からの出力ゲイン・マイク閾値調整
- 思考中・再生中のボタン割り込み
- 小型内蔵スピーカー向けのPC側DSPとI2S出力補正

音声は半二重です。ウェイクワード、音響エコーキャンセル、インターネット越しの公開中継、OTA更新は未対応です。

## ビルド済みファームを書き込む

[Releases](https://github.com/ochisamu/CharaDock-ESP32/releases) から `CharaDock-ATOM-Echo-v0.5.1.bin` と `SHA256SUMS.txt` を取得します。チェックサムを確認し、COMポートを解放するためCharaDockを終了してから、結合済みbinをアドレス `0x0` に書き込みます。

```powershell
py -m pip install --upgrade esptool
py -m esptool --chip esp32 --port COM3 erase_flash
py -m esptool --chip esp32 --port COM3 --baud 460800 write_flash 0x0 .\CharaDock-ATOM-Echo-v0.5.1.bin
```

`COM3` は実際のポートへ置き換えてください。`erase_flash` を実行すると、以前保存したWi-Fi情報とペアリング情報も消去されます。

## CharaDockと接続する

1. [CharaDock v0.5.0以降](https://github.com/ochisamu/CharaDock/releases)を起動します。
2. **設定 → ESP32デバイス → ATOM Echo** を開きます。
3. USB接続した状態で有効化し、自動検出されなければCOMポートを選びます。
4. 無線で使う場合は、USB接続中にPCが接続しているWi-Fi名とパスワードを入力して一度だけ設定します。
5. ボタン／ハンズフリー、マイク閾値、全体音量を調整します。
6. PC側で通常TTSまたはGPT-Liveを選びます。Chat／Work、キャラクター、音声、Beatriceの設定はPC版と共通です。

Wi-Fiパスワードとランダムなペアリング鍵はESP32のNVSに保存され、PCへ読み戻されません。信頼できるプライベートLAN専用です。ルーターでポートを外部公開しないでください。

通常TTSではIrodori TTSなどPCMを生成できる音声を選んでください。Windowsシステム音声はATOM Echoへ転送できません。GPT-LiveにはCodex app-server接続が必要です。ATOM EchoのLiveだけを最後の会話から5分で終了するオプションもありますが、初期状態はOFFです。

## LEDと操作

| LED | 状態 |
| --- | --- |
| 緑 | 接続済み・待機中 |
| 青 | 音声入力中 |
| アンバー | 認識後の処理中 |
| 紫 | キャラクター音声を再生中 |
| 赤 | 接続・認識・再生のエラー |
| 暗い点滅 | 接続先を探索中 |

ボタン方式ではATOMボタンを押したまま話し、離すと送信します。思考中または再生中に押すと割り込みます。ハンズフリー方式は音声レベルをデバイス内で常時監視しますが、閾値を超えた発話だけをペアリング済みPCへ送ります。ハンズフリーを有効にしただけではGPT-Liveは開始されず、Live料金も継続発生しません。

## ソースからビルド

Python 3.10以降と[PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html)を用意します。依存バージョンは [`firmware/atom-echo/platformio.ini`](./firmware/atom-echo/platformio.ini) に固定しています。

```powershell
pio run --project-dir firmware/atom-echo
pio run --project-dir firmware/atom-echo --target upload --upload-port COM3
pio device monitor --port COM3 --baud 500000
```

配布用の結合済みbinは次のコマンドで作成できます。

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build-release.ps1
```

成果物とSHA-256一覧は `dist/` に生成され、Git管理からは除外されます。

## 音声品質の設計

デバイスからPCへは16 kHz PCM16 monoで送信します。CharaDockはPC側で音声向けハイパス、ピーク保護、ゲイン、リサンプルを行います。ファームウェアは再生開始前のバッファ、monoサンプルのI2S左右スロット複製、大きめのDMAキュー、停止前のドレインを行います。これにより旧ESP32のmono I2Sで起きる音量・スロット問題を避けます。内蔵0.5〜0.8 W級スピーカーの低音や最大音量には物理的な限界があります。

## 通信

| 用途 | 通信 |
| --- | --- |
| デバイス探索 | UDP 41721 |
| PC通知 | UDP 41723 |
| 認証・制御・PCM | TCP 41722 |
| USB設定／フォールバック | Serial 500000 baud |

無線接続では、設定済みデバイスIDとHMAC-SHA256チャレンジで認証します。フレームには固定ヘッダー、シーケンス、長さ制限、CRC-32があり、スピーカーチャンクにはACKを返します。詳細は [docs/protocol.md](./docs/protocol.md) を参照してください。

## トラブルシューティング

- **接続待ちのまま:** USBを接続し、CharaDockが起動していることとCOMポートを確認してください。
- **Wi-Fiに接続できない:** アクセスポイント変更後は再設定します。端末間通信を遮断するゲストWi-Fiは使用できません。
- **近づかないとハンズフリー開始しない:** 画面のRMS／ノイズフロアを見ながら閾値を少しずつ下げます。通常の室内ノイズ以下にはしないでください。
- **認識後にアンバーのまま:** CharaDockとファームを最新版にし、PCやスマートフォンが別のLive接続を保持していないか確認します。
- **通常TTSで赤になる:** PCM対応の通常TTSとモデル準備状態を確認します。
- **音量が小さい:** CharaDockの全体ゲインを上げます。上げすぎると内蔵スピーカー固有の歪みが増えます。
- **書き込み時にポートを開けない:** CharaDockとシリアルモニターを終了します。

## セキュリティとプライバシー

Wi-Fiパスワード、ペアリング鍵、APIキー、ユーザー固有パス、ローカルログはリポジトリにも配布binにも含めません。クラウド接続と料金はPC版CharaDockの設定に従い、このファームウェアがOpenAIへ直接接続することはありません。

## ライセンス

本リポジトリのソースコードは [Apache License 2.0](./LICENSE) です。PlatformIOが取得する外部ライブラリにはそれぞれのライセンスが適用されます。
