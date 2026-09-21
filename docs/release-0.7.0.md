# CharaDock ESP32 v0.7.0

[CharaDock v0.7.0](https://github.com/ochisamu/CharaDock/releases/tag/v0.7.0)向けのファームです。

- **ATOM Voice / ATOM Echo**: Protocol-v1音声端末。バージョン表記を更新。
- **Waveshare ESP32-S3-RLCD-4.2**: USB音声転送・長文再生、会議用ミュート、再生中表示を改善。
- **StackChan K151（実験的）**: Wi-Fi・会話接続と専用の顔表示を拡張。RLCDの実機実績はStackChanの動作保証ではありません。

PC側のJev呼びかけ判定は通常TTSのハンズフリー向けです。既存VADで検出・認識した文をPCで判定します。ファームにJevキーやsanoTTS、音声モデルを組み込みません。

## ダウンロードと注意

- `CharaDock-ATOM-Echo-v0.7.0.bin`
- `CharaDock-Waveshare-RLCD-4.2-v0.7.0.bin`
- `CharaDock-StackChan-K151-v0.7.0.bin`
- `SHA256SUMS.txt`

機種が一致するファームだけを使用してください。結合binは**初期導入・復旧用**で、書き込み範囲の設定が消えます。RLCDの通常更新は`./scripts/flash-rlcd42.ps1`の分割書き込みでNVSを保持してください。公開作業では接続中の実機への書き込みは行っていません。

Wi-Fi資格情報・ペアリング秘密・会話ログは配布物に含まれません。音声認識と音声合成はPC側です。

---

Companion firmware for CharaDock v0.7.0: ATOM Voice/Echo (Protocol v1), Waveshare RLCD 4.2 (USB playback, meeting mute and now-playing improvements), and experimental StackChan K151 (Wi-Fi conversation and native face).

Use only the image matching your hardware. Merged images are for initial installation/recovery and overwrite settings in the written range. Use the documented split RLCD update to preserve NVS. No connected hardware is flashed as part of publishing this release. Speech processing and optional Jev filtering run on the PC; credentials and speech models are not embedded.
