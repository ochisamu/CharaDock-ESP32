# CharaDock ESP32 v0.7.1

[CharaDock v0.7.1](https://github.com/ochisamu/CharaDock/releases/tag/v0.7.1)に合わせたバージョン更新です。機能はv0.7.0と同一で、PCのGPT-6 Sol／Luna対応のために実機を再書き込みする必要はありません。

## ダウンロード

- `CharaDock-ATOM-Echo-v0.7.1.bin` — ATOM Voice／ATOM Echo
- `CharaDock-Waveshare-RLCD-4.2-v0.7.1.bin` — Waveshare ESP32-S3-RLCD-4.2
- `CharaDock-StackChan-K151-v0.7.1.bin` — StackChan K151（実験的）
- `SHA256SUMS.txt`

機種が一致するファームだけを使用してください。結合binは**初期導入・復旧用**で、書き込み範囲の設定を上書きします。RLCDの通常更新には`./scripts/flash-rlcd42.ps1`の分割書き込みを使うとNVSを保持できます。

今回の公開作業では実機へ書き込んでいません。Wi-Fi資格情報・ペアリング秘密・会話ログ・sanoTTS・音声モデルは配布物に含めません。音声認識・合成・任意のJev判定はPC側で行います。

[Full changelog](https://github.com/ochisamu/CharaDock-ESP32/compare/v0.7.0...v0.7.1)

---

Version-only companion release for CharaDock v0.7.1. Firmware behavior is unchanged from v0.7.0; reflashing is not required for GPT-6 Sol/Luna model selection on the PC. Includes ATOM Voice/Echo, Waveshare RLCD 4.2 and experimental StackChan K151 images.

Use only the image matching your hardware and verify SHA-256. Merged images overwrite settings in the written range; use the documented split RLCD update to preserve NVS. No hardware is flashed during this release task. Credentials, private logs and speech models are not embedded; speech processing runs on the PC.
