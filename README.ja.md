# M5Core488

[English](README.md)

M5Core488 は、AR488 を M5Stack CoreS3 から操作するための、PCレス GPIB マクロコントローラです。

古い GPIB 測定器を、もう少し気軽に使うために作りました。

AR488 の GPIB 制御機能はそのまま利用し、CoreS3 から USB Serial 経由で操作します。

```text
GPIB測定器
    |
IEEE-488 / GPIB
    |
  AR488
    |
USB Serial
    |
M5Stack CoreS3
```

microSD カードに保存したテキスト形式のマクロを実行し、測定器からの応答を CSV に記録できます。  
Wi-Fi を使用すれば、同じ LAN 上の PC やスマートフォンから Web UI を使って操作することもできます。

![M5Core488 system diagram](docs/images/config-jp.jpg)
*M5Core488 の基本構成*

## 主な機能

- microSD カード上の GPIB マクロを実行
- CoreS3 のタッチ画面からマクロを選択・実行
- Web UI から RUN / LOOP / STOP
- `[setup]` と `[loop]` による初期設定と連続実行の分離
- `@wait` による待ち時間指定
- GPIB コマンドと応答を CSV 形式で記録
- NTP / RTC による時刻管理
- USB SD MODE による PC からの microSD 編集
- マクロの Upload / Download / ON / OFF

## マクロ

マクロは通常のテキストファイルです。

```text
[setup]

++addr 22
++auto 0

[loop]

MEAS:VOLT:DC?
++read eoi

@wait 1000
```

`[setup]` は開始時に1回だけ実行されます。  
`[loop]` は RUN では1回、LOOP では停止するまで繰り返し実行されます。

測定器固有のコマンドはマクロファイル側へ記述するため、M5Core488 本体には測定器固有の処理を極力持たせない方針です。

> 上の `MEAS:VOLT:DC?` は SCPI 対応 DMM を想定した記述例です。  
> 実際のコマンドや GPIB アドレスは、使用する測定器のプログラミングマニュアルに合わせてください。

## マクロ言語について

M5Core488 のマクロは、測定器へコマンドを順番に送るための簡易な記述形式です。

Python のような汎用スクリプト言語ではありません。

現在、次のような構文は実装していません。

- `if` / `else`
- `for` / `while`
- 変数
- 関数
- `goto`

複雑な条件分岐や演算を伴う自動測定には、PC 上の Python などを使用する方が適しています。

M5Core488 は、その少し手前くらいを担当します。

## 必要なもの

- M5Stack CoreS3
- AR488
- GPIB 対応測定器
- microSD カード
- CoreS3 と AR488 を接続する USB ケーブル
- 必要に応じて CoreS3 用外部電源

## 動作モード

### AR488 MODE

CoreS3 が USB Host となり、AR488 と USB Serial で通信します。

通常のマクロ実行、Web UI、CSV Logger はこちらのモードで使用します。

### USB SD MODE

CoreS3 を USB Mass Storage として PC へ接続し、microSD カード内のファイルを編集できます。

AR488 MODE と USB SD MODE では USB の役割が異なるため、モード変更時は再起動します。

## Web UI

Web UI では、AR488 接続状態やマクロ実行状態を確認しながら、次の操作ができます。

- RUN
- LOOP
- STOP AFTER CYCLE
- Macro ON / OFF
- Macro Upload / Download
- CSV Log Download

Web UI には現在ユーザー認証機能がありません。  
信頼できる LAN 内での使用を想定しています。インターネットへ直接公開しないでください。

## ドキュメント

- [日本語ユーザーマニュアル](docs/manual_ja.md)
- [English User Manual](docs/manual_en.md)

## Status

M5Core488 は現在開発中です。

実際の GPIB 測定器を接続しながら動作確認しています。

万能な GPIB 自動測定システムを目指しているわけではありません。  
手元にある GPIB 測定器を、PC を立ち上げずにもう少し気軽に使えれば十分、というところから始まったプロジェクトです。

## Third-party projects

M5Core488 は AR488 と連携し、以下のライブラリ等を利用して開発しています。

- AR488
- M5Unified
- M5GFX
- EspUsbHost

各プロジェクトにはそれぞれのライセンスが適用されます。

## License

M5Core488 本体のコードは [MIT License](LICENSE) のもとで公開する予定です。
