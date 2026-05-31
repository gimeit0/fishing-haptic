# 疑似釣り体験プロトタイプ (Fishing Haptic Prototype)

## プロジェクト概要
JAIST 木谷研究室の研究プロジェクト。
のべ竿型・非接地・小型振動装置で「アタリ」と「引き」の触覚を切替提示する VR 釣り具プロトタイプ。

## ハードウェア構成

| 部品 | 型番 | 役割 |
|------|------|------|
| メインコントローラ | M5StickS3 (ESP32-S3) | 波形生成・I2S 出力・IMU |
| デジタルアンプ | MAX98357A | I2S → アナログ + Class-D 増幅 |
| 振動子 | Dayton EX25FHE2-4 (4Ω/24W) | ボイスコイル型エキサイター |
| 電源 | Anker PowerCore 5000 | 5V 供給 |

## 配線（M5StickS3 → MAX98357A）

| M5StickS3 | MAX98357A | 信号 |
|-----------|-----------|------|
| 5V | VIN | 電源 |
| GND | GND | グランド |
| GPIO4 | BCLK | ビットクロック |
| GPIO5 | LRC | LR チャンネル選択 |
| GPIO6 | DIN | デジタル音声データ |

MAX98357A の OUT+/OUT- は EX25FHE2-4 に接続。

## 状態機（State Machine）

IDLE (待機)
└─ BtnA 押下 → NIBBLE
NIBBLE (アタリ: 10Hz + 23Hz 合成正弦波)
└─ IMU 合わせ動作検出 → PULL
PULL (引き: 5ms 高 + 17ms 低 非対称矩形波)
└─ 5 秒経過 → IDLE

## 先行研究の波形仕様

- **アタリ振動** (才木ら 2016): 10Hz と 23Hz にピークを持つ微振動
- **引き感覚** (高椋ら 2016): 5ms 高電圧 + 17–22ms 低電圧の非対称矩形波で牽引力錯覚

## 技術的注意事項

1. **MAX98357A の高通フィルタ**: 内部に約 14Hz のハイパスフィルタがあり、10Hz 信号は減衰する可能性あり
2. **I2S サンプリングレート**: 16kHz
3. **IMU**: 内蔵 BMI270 を使用、M5Unified 経由でアクセス
4. **LCD 表示**: 現在の状態（IDLE/NIBBLE/PULL）を表示

## 開発環境

- PlatformIO + Arduino framework
- Board: m5stack-stamps3
- Library: M5Unified ^0.2.2

## 開発方針

- コメントは日本語可、変数名は英語
- 波形生成は I2S buffer で行う
- 既存の main.cpp の構造を維持して機能追加

## 現在の進捗

✅ 基本動作確認完了：BtnA 押下で 200Hz 正弦波を 1 秒間出力
⬜ アタリ波形（10Hz + 23Hz）の実装
⬜ 引き波形（非対称矩形波）の実装
⬜ IMU で合わせ動作検出
⬜ 状態遷移ロジック完成

## 既知の問題

- M5StickS3 の LCD が暗い／表示が見えにくい
  → M5.Display の初期化と setBrightness を確認する必要がある
