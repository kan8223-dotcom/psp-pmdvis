# PSP PMD Visualizer (PMDVIS)

PSP 上で PC-98 の FM音源 (YM2608) をリアルタイム再生するプレイヤー。
メインCPUがビジュアライザ描画を担当する一方、Media Engine を 333MHz フル稼働させて FM 合成を処理する。

## 機能

- **PMD (.M) 再生** — pmdmini (ymfm fork) によるFM 6ch + SSG 3ch + リズム
- **Media Engine オフロード** — 333MHz 等速 FM 合成
- **リングバッファ 256 ブロック** — 長時間再生でも安定
- **ピアノロール / FMPスタイル ビジュアライザ** — チャンネルごとのステータス表示
- **WAV エクスポート** — ループ回数・フェードアウト設定付き
- **メモリースティック再帰スキャン (SEARCH)** — ストレージ全体から .M を自動検出
- **チャンネル別ソロ / ミュート** — 全 11 パート対応 (FM×6, SSG×3, ADPCM, リズム)
- **○/× ボタン自動リージョン判定**
- **ループキャッシュ** — 60 秒ぶんの PCM 事前デコードでギャップレス再生

## 動作確認済み機種

| 機種 | ファームウェア |
|---|---|
| PSP-1000 | 6.60 PRO-B10 |
| PSP Go (N1000) | ARK-4 CXPL |

## 必要環境

- カスタムファームウェア導入済みの PSP (6.60 PRO-B10 / ARK-4 など)
- PMD ファイル用の空きがあるメモリースティック
- (任意) リズム音源再生用の YM2608 ADPCM リズム ROM

## インストール

1. `EBOOT.PBP` を `ms0:/PSP/GAME/PMDVIS/` にコピー
2. (任意) YM2608 ADPCM ROM を `ms0:/PSP/GAME/PMDVIS/ym2608_adpcm_rom.bin` に配置。
   このファイルが無くてもリズムチャンネルが無音になるだけで、プレイヤー本体は問題なく動作する
3. PMD (.M) ファイルをメモリースティック上のどこでも好きな場所に配置する。
   PMDVIS が再帰的にスキャンして検出する。
   または `ms0:/PSP/GAME/PMDVIS/songs/` に直接置いてもよい

## YM2608 ROM についての注意

YM2608 ADPCM リズム ROM はヤマハ株式会社の著作物であり、
本プロジェクトには **同梱されていない**。
リズム再生を有効にしたいユーザーは、正規のソースから自分で ROM を用意する必要がある。
ROM が無くても PMDVIS は動作する — リズムチャンネルが無音になるだけ。

## 操作

### リスト画面

| ボタン | 機能 |
|---|---|
| ○ | 選択中の曲を再生 |
| × | アプリ終了 |
| ↑↓ | カーソル移動 |
| L / R | ページ送り |
| □ | WAV エクスポート設定 |
| △ | 曲を削除 (システムダイアログで確認) |
| START | ME ON/OFF 切替 (警告ダイアログ表示) |
| SELECT | ストレージから .M ファイルを検索 |

### 再生画面

| ボタン | 機能 |
|---|---|
| ○ | 一時停止 / 再開 |
| × | リストに戻る |
| ←→ | ビジュアライザ切替 (キーボード ⇔ FMP) |
| ↑↓ | ソロカーソル移動 |
| △ | トラックミュート切替 |
| □ | 全トラック復帰 (ミュート解除) |
| L / R | 次の曲 / 前の曲 |
| L+R | スクリーンショット保存 |

### WAV エクスポート設定

| ボタン | 機能 |
|---|---|
| △ | ループ回数切替 (1 ⇔ 2) |
| □ | フェードアウト ON/OFF |
| ○ | エクスポート開始 |
| × | キャンセル |

## ソースからのビルド

[docs/BUILD.md](docs/BUILD.md) を参照。

## ライセンス

GPL-3.0。サードパーティコンポーネントの帰属については
[LICENSE](LICENSE) と [NOTICE](NOTICE) を参照。

## クレジット

- **pmdmini / PMD サウンドエンジン**:
  - M.Kajihara (PMD原作、PC-98)
  - C60 (PMDWin Windows 移植 — pmdwincore, pmdwin, opnaw, p86drv, ppsdrv, table, util, sjis2utf)
  - UKKY (PPZ8 PCM ドライバ原作)
  - BouKiCHi (pmdmini ライブラリラッパー)
- **ymfm**: Aaron Giles (YM2608 FM/SSG 合成、BSD-3-Clause)
- **Media Engine ライブラリ**: m-c/d (mcidclan) —
  [psp-media-engine-custom-core](https://github.com/mcidclan/psp-media-engine-custom-core)
  (MECC, MIT ライセンス)。本プロジェクトは pspdev/psp-packages の
  [libmecore v2.0.1](https://github.com/pspdev/psp-packages/blob/master/libmecore/PSPBUILD)
  をベースに、環境互換性のためのローカルパッチ 4 点を適用したビルドを
  同梱しています。詳細は [NOTICE](NOTICE) 参照。
- **pspdev コミュニティ**: pspsdk およびツールチェーン

## 作者

Tanaka ([@kan8223-dotcom](https://github.com/kan8223-dotcom))
