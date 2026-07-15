// =============================================================================
//  haptics.h ― ボイスコイル型加振器 触覚提示モジュール (I2S → MAX98357A)
//
//  研究計画書 v3.0.0 の刺激設計を実装する:
//   - アタリ感 : 10Hz + 23Hz 合成正弦波の短バースト (才木ら 2016)
//   - 引き感   : 5ms 高電圧 + T ms 低電圧 の非対称矩形波 (高椋ら 2016)
//  波形はサンプル単位で合成し I2S (16kHz) からアンプへ出力する。
// =============================================================================
#pragma once
#include <stdint.h>

enum HapticMode : uint8_t {
  HAPTIC_OFF = 0,   // 無振動 (IDLE / CASTING / WAITING / CAUGHT / FAILED)
  HAPTIC_NIBBLE,    // 前アタリ: 微弱バースト (ランダム間隔)
  HAPTIC_BITE,      // 本アタリ: 同波形を連続・強めに提示
  HAPTIC_PULL,      // 引き: 非対称矩形波による牽引力錯覚
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

// 不規則性 on/off: on = パルス間隔±2msジッタ + 低周波ゆらぎ (魚の暴れ),
// off = 高椋ら2016準拠の厳密な周期刺激 (実験の対照条件用)
void hapticSetIrregular(bool on);

// I2S 初期化に成功していれば true (配線無しでもデモは動く)
bool hapticReady();
