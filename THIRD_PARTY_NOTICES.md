# Third-Party Notices

本リポジトリのうち、著作者が作成した部分は MIT License（`LICENSE` 参照）です。
以下は第三者の成果物で、それぞれの条件に従います。

## 同梱している第三者コード

### IBonDriver2.h

- 出典: BonDriver SDK（BonDriver インターフェース定義ヘッダ）
- 用途: `pc-bondriver/BonDriver_WebOS.cpp` が実装するインターフェースの定義
- ライセンス: 配布元の条件に従います。**本リポジトリの MIT License は適用されません。**
- 備考: BonDriver 互換ドライバを実装するために必要なヘッダです。再配布が許諾されない場合は
  当該ファイルを削除し、利用者が各自の SDK から用意してください。

## 仕様の参照（コードのコピーは含みません）

### Mirakurun

- 出典: https://github.com/Chinachu/Mirakurun
- ライセンス: Apache License 2.0
- 参照範囲: Mirakurun 互換 REST API の仕様（エンドポイント構成、JSON フィールド、
  `X-Mirakurun-Priority` の意味論）
- 本リポジトリは Mirakurun のコードを含みません。API 互換実装を独自に C で実装しています。

## 同梱していないもの

LG webOS ファームウェア由来のバイナリ（`tvservice`、`libhal_lg115x.so`、`libkadaptor.so`、
`libc-2.24.so`、`ld-linux.so.3` など）は**再配布していません**。
ビルドに必要な共有ライブラリは、利用者自身の TV から取得してください（`tv_libs/README.md` 参照）。
