# CharaDock ESP32 v0.6.0

[CharaDock v0.6.0](https://github.com/ochisamu/CharaDock/releases/tag/v0.6.0)向けのファームです。

- ATOM Echo / ATOM Voice：既存Protocol-v1音声端末。
- Waveshare ESP32-S3-RLCD-4.2：Protocol-v2、キャラ・日本語字幕、瞬き・口パク、時計、センサー、マイク・スピーカー。
- StackChan K151：独立した実験的Protocol-v2ターゲット。RLCDの実機確認はStackChanの動作保証を意味しません。

RLCDはUSB接続が有効な間はUSBを優先し、切断時は認証済みWi-Fiへ戻ります。
切り替え時の録音・再生を停止し、待機側によるマイク設定の干渉を防ぎます。
物音候補では時計を維持し、PC側の声判定後にポートレートへ戻ります。

## 書き込み注意

機種名が一致するファームだけを使用してください。ATOM用binをESP32-S3へ流用しないでください。
添付の結合binは**初期導入・復旧用**です。書き込む範囲の設定が消えます。
RLCDの通常更新はリポジトリの分割書き込み手順（`scripts/flash-rlcd42.ps1`）を使用し、
Wi-Fi・ペアリングを保存するNVSを保持してください。
モデル・辞書・会話ログ・認証情報は含まれません。音声はPC側で生成します。

対応ソース、固定依存関係、ビルド手順、第三者ライセンス通知を同じタグに収録しています。
