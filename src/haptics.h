// =============================================================================
//  haptics.h ― ボイスコイル型加振器 触覚提示モジュール (I2S → MAX98357A)
//
//  研究計画書 v3.0.0 の刺激設計 + IMPROVEMENT_PROPOSAL.md の改良を実装する:
//   - アタリ感 : 「クリック + 竿体余振」のタップイベント列 (方案二)。
//                余振は 10Hz+23Hz 包絡 (才木ら 2016) を高周波キャリアで
//                AM 変調し、加振器/アンプの低域遮断を回避する (方案一)。
//   - 引き感   : 5ms 高電圧 + T ms 低電圧 の非対称矩形波 (高椋ら 2016)。
//                slack (骤停) やサージ瞬態のイベントを重畳可能 (方案四)。
//  hapticSetIrregular(false) で従来の文献準拠波形に戻る (実験の対照条件)。
//  波形はサンプル単位で合成し I2S (16kHz) からアンプへ出力する。
// =============================================================================
#pragma once
#include <stdint.h>

enum HapticMode : uint8_t {
  HAPTIC_OFF = 0,   // 無振動 (CASTING / WAITING)
  HAPTIC_NIBBLE,    // 前アタリ: 微弱バースト (ランダム間隔)
  HAPTIC_BITE,      // 本アタリ: 同波形を連続・強めに提示
  HAPTIC_PULL,      // 引き: 非対称矩形波による牽引力錯覚
  HAPTIC_WAVE,      // 渚: 待機中の環境触覚。12-18s 毎に「浪が竿に寄せる」
                    //     2.6s のスウェル (主峰+回浪)。UEC坂本研の知見に基づき
                    //     "ふわふわ"側の柔らかい包絡。副次効果として待機電流を
                    //     維持しモバイルバッテリーの自動判停を防ぐ
};

// I2S ドライバ初期化 + 波形生成タスク起動。成功で true。
bool hapticInit();

// 現在の提示モードを切替 (いつ呼んでも良い。波形タスク側でクリック無く遷移)
void hapticSetMode(HapticMode m);
HapticMode hapticGetMode();

// 引き波形パラメータ: strength 0..1 (振幅=引きの強さ),
// intervalMs = パルス間隔T [ms] (高椋ら: 12ms以上でリアリティ高)
void hapticSetPull(float strength, int intervalMs);
float hapticPullStrength();
int   hapticPullInterval();

// アタリ振動の振幅 0..1 (微弱刺激なので小さめが基本)
void  hapticSetNibbleAmp(float amp);
float hapticNibbleAmp();

// 不規則性 on/off:
//  on  = 改良版: アタリ=タップイベント列(AM), 引き=ジッタ+ゆらぎ (既定)
//  off = 文献準拠の対照条件: アタリ=10+23Hz正弦バースト,
//        引き=高椋ら2016の厳密な周期刺激
void hapticSetIrregular(bool on);

// AM キャリア周波数 [Hz] (方案一)。加振器の共振点付近に合わせる。
// 実測スイープ用にシリアル "fc <Hz>" から変更可。30–150Hz にクランプ。
void  hapticSetCarrier(float hz);
float hapticCarrier();

// タップ余振の時定数 τ [ms] (方案二)。30–400ms にクランプ。
void hapticSetTapTau(int ms);
int  hapticTapTau();

// 渚 (WAVE) の振幅 0..1。0 で無効。既定 0.45 ("wa" コマンドで調整)
void  hapticSetWaveAmp(float amp);
float hapticWaveAmp();

// 渚の平均間隔 [秒] (実際は±20%ランダム)。既定 45s。5-120s にクランプ。
void hapticSetWaveInterval(int sec);
int  hapticWaveInterval();

// 底流 (undercurrent): 渚の合間を埋める保活トーン。共振点以下の低周波なので
// 体感ほぼゼロのまま電流だけ流れ、バッテリーの低負荷判停を防ぐ。
// amp 0..1 (0=無効, 既定0.35, "wk"), 周波数 16-45Hz (既定28Hz, "wf")
void  hapticSetUndercurrent(float amp);
float hapticUndercurrent();
void  hapticSetUndercurrentHz(float hz);
float hapticUndercurrentHz();

// 底流のソフトクリップ係数 1-4 ("wd")。ピーク(体感)据え置きで RMS 電流を
// 増やす: 1≈純正弦, 2(既定)で電力≈1.4倍, 4で≈1.7倍。上げすぎると高調波
// (3f≈84Hz) が微かに感じられることがある
void  hapticSetUndercurrentDrive(float k);
float hapticUndercurrentDrive();

// 引き提示中のイベント (方案四)。PULL モード以外では無視される:
//  Slack: ms の間 振動を骤停→復帰 (「糸がふっと緩む」合図)
//  Tap  : サージ開始などの瞬態を1回重畳 (amp 0..1)
void hapticTriggerSlack(int ms);
void hapticTriggerTap(float amp);

// I2S 初期化に成功していれば true (配線無しでもデモは動く)
bool hapticReady();
