# CharaDock ESP32

[CharaDock](https://github.com/ochisamu/CharaDock) 用の音声・キャラクターデバイス向けファームウェアです。会話処理はPC版CharaDockが担当し、ESP32デバイスはマイク、スピーカー、ボタン、LEDや画面などの物理的な入出力を担当します。

[English](./README.md)

## 対応状況

| デバイス | 入力 | 出力 | 状況 |
| --- | --- | --- | --- |
| M5Stack ATOM Voice（旧名 ATOM Echo、C008-C） | ボタン／ハンズフリーVAD | 内蔵スピーカー＋RGB LED | 対応済み |
| M5Stack StackChan K151 | 頭部タッチ／PTT先行実装 | 画面／RGB／サーボ先行実装 | CoreS3ファーム実装中・実機検証待ち |
| Waveshare ESP32-S3-RLCD-4.2 | KEY PTT／ハンズフリーVAD／BOOT診断 | 400×300モノクロ画面＋内蔵スピーカー | Protocol v2のUSB／Wi-Fi・マイク・スピーカー・画面preview |

デバイス固有のコードを `firmware/<device>` に分け、将来の機種も同じリポジトリと共通ホストプロトコルで管理します。現行の安定版は **M5Stack ATOM Voice（旧名 ATOM Echo、商品コード C008-C）の旧ESP32-PICO-D4版** と、CharaDock v0.5.1の組み合わせを対象にしています。StackChan K151とWaveshare RLCD 4.2の実装はそれぞれ独立したディレクトリに分離し、既存ATOM用protocol v1を変更しません。RLCD 4.2にはST7305表示、5種類の原子的な画面、東雲12／16 px日本語フォント、漫画調ポートレート転送、物理操作、RTC・温湿度・電池診断、ES7210マイク入力、ES8311スピーカー再生、USB／認証付きWi-FiのDevice Protocol v2を実装しました。Chat／Work、音声認識、通常TTS、GPT-Live、BeatriceはPC版CharaDockが担当し、RLCDファームはPCMと画面状態だけを送受信します。端末内TTSエンジンやモデルは含みません。

> **製品名について:** [スイッチサイエンスの商品ページ](https://www.switch-science.com/products/6347)では、2026年4月に販売名が「ATOM Echo」から「ATOM Voice」へ変更されたと案内されています。商品コードと対応ハードウェアは同じです。CharaDock v0.5.1の画面、プロトコル、ファームウェア名では互換性のため「ATOM Echo」表記を残しています。

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

[Releases](https://github.com/ochisamu/CharaDock-ESP32/releases) から `CharaDock-ATOM-Echo-v0.5.2.bin` と `SHA256SUMS.txt` を取得します。チェックサムを確認し、COMポートを解放するためCharaDockを終了してから、結合済みbinをアドレス `0x0` に書き込みます。

```powershell
py -m pip install --upgrade esptool
py -m esptool --chip esp32 --port COM3 erase_flash
py -m esptool --chip esp32 --port COM3 --baud 460800 write_flash 0x0 .\CharaDock-ATOM-Echo-v0.5.2.bin
```

`COM3` は実際のポートへ置き換えてください。`erase_flash` を実行すると、以前保存したWi-Fi情報とペアリング情報も消去されます。

## CharaDockと接続する

1. [CharaDock v0.5.1以降](https://github.com/ochisamu/CharaDock/releases)を起動します。
2. **設定 → ESP32デバイス → ATOM Echo** を開きます。
3. USB接続した状態で有効化し、自動検出されなければCOMポートを選びます。
4. 無線で使う場合は、USB接続中にPCが接続しているWi-Fi名とパスワードを入力して一度だけ設定します。
5. ボタン／ハンズフリー、マイク閾値、全体音量を調整します。
6. PC側で通常TTSまたはGPT-Liveを選びます。Chat／Work、キャラクター、音声、Beatriceの設定はPC版と共通です。

Wi-Fiパスワードとランダムなペアリング鍵はESP32のNVSに保存され、PCへ読み戻されません。信頼できるプライベートLAN専用です。ルーターでポートを外部公開しないでください。

通常TTSではIrodori TTSなどPCMを生成できる音声を選んでください。Windowsシステム音声はATOM Echoへ転送できません。GPT-LiveにはCodex app-server接続が必要です。ATOM EchoのLiveだけを最後の会話から5分で終了するオプションもあり、初期状態はOFFです。

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

Python 3.10以降と[PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html)を用意します。依存バージョンは各デバイスの `platformio.ini` に個別に固定しています。RLCDのArduino-ESP32 3.x package cacheはM5系ターゲットの2.x cacheから分離しているため、StackChanやATOMのビルドがRLCDのtoolchainを置き換えません。

```powershell
pio run --project-dir firmware/atom-echo
pio run --project-dir firmware/atom-echo --target upload --upload-port COM3
pio device monitor --port COM3 --baud 500000

# StackChan K151 / CoreS3の先行実装をビルド
pio run --project-dir firmware/stackchan

# Waveshare ESP32-S3-RLCD-4.2の表示／マイク／音声／Wi-Fiファームをビルド
pio run --project-dir firmware/waveshare-rlcd-4.2
```

配布用の結合済みbinは次のコマンドで作成できます。

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build-release.ps1
```

成果物とSHA-256一覧は `dist/` に生成され、Git管理からは除外されます。

StackChanの結合済みpreviewイメージは、正式対応済みATOMの成果物を置き換えないよう別スクリプトで生成します。

```powershell
powershell -ExecutionPolicy Bypass -File `
  .\scripts\build-stackchan-release.ps1 `
  -Version 0.1.0-preview
```

`dist\stackchan` に、bootloaderとpartitionを含むアドレス`0x0`書き込み用binとSHA-256一覧が生成されます。

RLCD 4.2も正式対応済みATOMの成果物を置き換えないpreview用スクリプトで、アドレス`0x0`書き込み用binとSHA-256一覧を生成します。

```powershell
powershell -ExecutionPolicy Bypass -File `
  .\scripts\build-rlcd42-release.ps1 `
  -Version 0.2.0-preview
```

RLCD接続には使用中経路のheartbeat、最終キャラクターを残すOffline画面、上限付き再接続backoff、再接続後の完全Snapshot復元、16 kHzマイク入力、512 KiB PSRAMに先読みするPC生成音声の再生を含みます。初回Wi-Fi設定にはUSBを使い、相互HMACペアリング後もUSBをフォールバックとして残します。書き込み、起動診断、漫画調ポートレート、マイク、Wi-Fi、スピーカーの確認は [`firmware/waveshare-rlcd-4.2/README.md`](./firmware/waveshare-rlcd-4.2/README.md) を参照してください。

## 音声品質の設計

デバイスからPCへは16 kHz PCM16 monoで送信します。音声認識、音声合成、必要なLive／Beatrice処理、保護、リサンプルはPC側で行い、PCMを端末へ戻します。ATOM用の小型スピーカーDSPは維持しつつ、RLCDは別のほぼ等倍プロファイル（100 Hzの軽いハイパスと最終リミッターのみ）に分け、ES8311経路へATOM固有の減衰や強い圧縮をかけません。ファームウェアはWi-Fi省電力を無効化し、PSRAM先読み、ACK付きフロー制御、停止前ドレインを行います。

## 通信

| 用途 | 通信 |
| --- | --- |
| デバイス探索 | UDP 41721 |
| PC通知 | UDP 41723 |
| 認証・制御・PCM | TCP 41722 |
| USB設定／フォールバック | Serial 500000 baud |

無線接続では、設定済み256-bit鍵でHMAC-SHA256の相互認証を行います。CharaDockがRLCDの応答を検証し、RLCDもPCの証明を確認してから制御・PCMを受け付けます。フレームには固定ヘッダー、シーケンス、長さ制限、CRC-32があり、スピーカーチャンクにはACKを返します。詳細は [docs/protocol.md](./docs/protocol.md) を参照してください。

## トラブルシューティング

- **接続待ちのまま:** USBを接続し、CharaDockが起動していることとCOMポートを確認してください。
- **Wi-Fiに接続できない:** アクセスポイント変更後は再設定します。端末間通信を遮断するゲストWi-Fiは使用できません。
- **Wi-Fi再生にプツプツ音が入る:** v0.5.2以降へ更新してください。省電力遅延、先読み不足、チャンク単位のACK待ちをまとめて緩和しています。改善しない場合はアクセスポイントとの距離と2.4 GHz帯の混雑を確認します。
- **近づかないとハンズフリー開始しない:** 画面のRMS／ノイズフロアを見ながら閾値を少しずつ下げます。通常の室内ノイズ以下にはしないでください。
- **認識後にアンバーのまま:** CharaDockとファームを最新版にし、PCやスマートフォンが別のLive接続を保持していないか確認します。
- **通常TTSで赤になる:** PCM対応の通常TTSとモデル準備状態を確認します。
- **音量が小さい:** CharaDockの全体ゲインを上げます。上げすぎると内蔵スピーカー固有の歪みが増えます。
- **書き込み時にポートを開けない:** CharaDockとシリアルモニターを終了します。

## セキュリティとプライバシー

Wi-Fiパスワード、ペアリング鍵、APIキー、ユーザー固有パス、ローカルログはリポジトリにも配布binにも含めません。クラウド接続と料金はPC版CharaDockの設定に従います。このファームウェアにはsanoTTS／Open JTalkのモデルもクラウド認証情報もなく、OpenAIへ直接接続しません。

## ライセンス

本リポジトリのソースコードは [Apache License 2.0](./LICENSE) です。PlatformIOが取得する外部ライブラリと同梱フォントにはそれぞれのライセンスが適用されます。詳細は [`THIRD_PARTY_NOTICES.md`](./THIRD_PARTY_NOTICES.md) に記載しています。
