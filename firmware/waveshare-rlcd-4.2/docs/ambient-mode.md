# 時計待機画面

マイク・接続の寿命とは独立した表示だけの切り替えです。

- 無操作・無発話30秒で時計へ。ハンズフリーの無言監視も対象。
- 発話検出・録音・音声再生でキャラ画面へ戻る。
- Thinking（認識・返事待ち）は時計とホストからの処理状態を表示。
- KEY短押しで待機中の時計／キャラを切り替え。処理中の短押しは従来どおり中断。
- エラー、承認、オフライン、作業中は専用表示を優先。
- 定期的なシーン更新では無発話タイマーを延長しない。
- PC側Liveの5分無操作切断は別管理であり変更しない。

表示方針のホストテスト（リポジトリルートから）:

```sh
g++ -std=c++17 -Ifirmware/waveshare-rlcd-4.2/include firmware/waveshare-rlcd-4.2/test/ambient_policy_test.cpp -o /tmp/charadock-ambient-test
/tmp/charadock-ambient-test
```
