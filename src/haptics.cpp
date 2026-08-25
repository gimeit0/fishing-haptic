// =============================================================================
//  haptics.cpp ― 波形生成 (I2S / MAX98357A / DAEX 加振器)
//
//  構成:
//   loop() 側  : hapticSetMode() / hapticSetPull() でパラメータを書くだけ
//   波形タスク : Core0 の FreeRTOS タスクが 8ms 毎にサンプルを合成し
//                i2s_write() でブロッキング送出 (描画負荷の影響を受けない)
//
//  配線 (CLAUDE.md 準拠): GPIO4=BCLK, GPIO5=LRC, GPIO6=DIN
//  ※ M5.Speaker(内蔵ブザー) と衝突しないよう I2S_NUM_1 を使用
// =============================================================================
#include "haptics.h"
#include <Arduino.h>
#include <driver/i2s.h>
#include <math.h>

// ===== ハードウェア定数 =====
#define HAP_I2S_PORT   I2S_NUM_1
#define HAP_PIN_BCLK   4
#define HAP_PIN_LRC    5
#define HAP_PIN_DIN    6
#define HAP_RATE       16000        // サンプリングレート [Hz]
#define HAP_CHUNK      128          // 1回の合成フレーム数 (=8ms)

// ===== 刺激デフォルト =====
#define PULL_HIGH_MS       5        // 非対称矩形波の高電圧区間 (高椋ら: 固定5ms)
#define PULL_INTERVAL_DEF  20       // パルス間隔T の既定値 [ms]。周期 5+20=25ms ≈ 40Hz
                                    // (Tanaka ら 2020: 非対称振動の牽引錯覚は 40Hz 付近が最適)
#define PULL_POLARITY      (+1.0f)  // 牽引方向。取付向きに合わせ ±1 で反転可
#define NIBBLE_AMP_DEF     0.55f    // アタリ振幅。才木らの主旨は「手元で知覚できない
                                    // アタリの増幅提示」なので、知覚しやすい水準を既定にする。
                                    // 知覚閾の探索実験では "na" コマンドで下げて調整。
#define CARRIER_HZ_DEF     60.0f    // AM キャリア既定値 [Hz]。EX25FHE2-4 の共振点想定
                                    // (50–80Hz)。実機で "fc" スイープして決める。
#define TAP_CLICK_MS       3        // タップ冒頭クリック幅 (2–5ms の短促冲击)
#define TAP_TAU_DEF        120      // タップ余振の減衰時定数 τ 既定値 [ms]
#define MASTER_GAIN        0.90f    // クリップ余裕

// ===== loop() 側から書かれる共有パラメータ (単語アクセスなので volatile で足りる) =====
static volatile uint8_t s_mode         = HAPTIC_OFF;
static volatile float   s_pullStrength = 0.7f;
static volatile int     s_pullT        = PULL_INTERVAL_DEF;
static volatile float   s_nibbleAmp    = NIBBLE_AMP_DEF;
static volatile bool    s_irregular    = true;
static volatile float   s_carrierHz    = CARRIER_HZ_DEF;
static volatile int     s_tapTau       = TAP_TAU_DEF;
static volatile float   s_waveAmp      = 0.20f;   // 鼓動の振幅 (0=無効)。低めが基本
static volatile int     s_waveIntMs    = 25000;   // 鼓動の間隔 [ms] (実際は±10%)
// 底流 (undercurrent): 渚の合間を埋める保活トーン。共振周波数 (Fs≈50-60Hz) より
// 十分低い周波数では加振器の機械出力が急減衰 (約-12dB/oct) するため体感は
// ほぼゼロだが、音圏インピーダンスは Re≈4Ω のままなので電流は流れ続ける。
// → 波間が何秒あってもバッテリーが「無負荷」と誤認しない。
static volatile float   s_kaAmp        = 0.0f;    // 底流の振幅 (0=無効)。
                                                  // 既定 0: 連続音は「ずっと震えて
                                                  // いて変」と不評 (2026-08-26)。
                                                  // 保活は鼓動パルスに任せ、判停が
                                                  // 再発した時だけ "wk" で復活させる
static volatile float   s_kaHz         = 28.0f;   // 底流の周波数 [Hz] (Fs以下, HPF14Hz以上)
static volatile float   s_kaDrive      = 1.0f;    // 底流のソフトクリップ係数 1-4。
                                                  // 1=純正弦 (既定。実機評価で高調波の
                                                  // ブーンが気になったため既定に戻した)。
                                                  // >1 で正弦をtanhで"太らせ"、ピーク
                                                  // (=体感)据え置きのままRMS電流を稼ぐ:
                                                  // 2で電力≈1.4倍, 4で≈1.7倍。保活が
                                                  // 足りない時だけ "wd" で実験的に上げる
static volatile int     s_slackReq     = 0;   // >0: PULL を ms だけ骤停 (方案四)
static volatile float   s_tapReq       = 0;   // >0: PULL にタップを1回重畳
static volatile int     s_tapTauReq    = 0;   // タップ余振τ指定 [ms] (0=自動)
static volatile bool    s_tugOn        = false; // 引き込み節律 (HOLD 抗適応)
static volatile bool    s_slipOn       = false; // ドラッグ滑り (クリック列重畳)
static bool             s_ready        = false;

// ===== タスク内 合成状態 =====
static float  ph10 = 0, ph23 = 0;     // アタリ包絡 (10/23Hz) の位相
static float  phC  = 0;               // AM キャリアの位相
static float  phKA = 0;               // 底流の位相
static int    pullPos = 0;            // 非対称波 1周期内のサンプル位置
static float  lfoPh   = 0;            // 引きの"暴れ"用 低周波ゆらぎ位相
static float  masterEnv = 0;          // モード切替クロスフェード用 0..1
static uint8_t modeApplied = HAPTIC_OFF;
// 旧アタリ(対照条件)のバースト管理 (サンプル数でカウント)
static int    burstLeft = 0, gapLeft = 0;
static float  burstEnv  = 0;          // バースト内エンベロープ (クリック防止)
// タップイベント (方案二) の状態
static int    tapPos = -1;            // -1 = 停止中
static int    tapClickLen = 0;
static float  tapAmp = 0, tapDecay = 1, tapK = 1;
static int    tapNextIn = 0;          // 次タップまでのサンプル数 (スケジューラ)
static float  biteRamp = 0;           // BITE の漸強 0..1
// PULL のイベント状態 (方案四)
static int    slackLeft = 0;          // 骤停の残りサンプル数
static float  slackEnv  = 1;          // 骤停用エンベロープ (クリック防止)
// 引き込み節律 (抗適応) の状態
static int    tugLeft = 0;            // 現フェーズの残りサンプル数
static bool   tugHigh = true;         // true=引き込み中 / false=小休止
static float  tugEnv  = 1;            // 節律エンベロープ (平滑済み)
// ドラッグ滑り (クリック列) の状態
static int    slipPos = -1;           // -1=クリック間, >=0 クリック内位置
static int    slipNextIn = 0;         // 次クリックまでのサンプル数
static int    slipClickLen = 0;
static float  slipDuck = 1;           // 滑り中の基礎張力ダック (平滑済み)
// 渚 (WAVE) の状態
static int    wavePos   = -1;         // -1=浪間の静寂, >=0 スウェル内サンプル位置
static int    waveGap   = 0;          // 次の浪までのサンプル数
static float  waveAmpCur = 0;         // 今回の浪の振幅 (±20% ランダム)

static inline int msToSamples(int ms) { return ms * HAP_RATE / 1000; }
static inline int rnd(int lo, int hi) { return lo + (int)(esp_random() % (uint32_t)(hi - lo + 1)); }

// --- 位相更新: 10/23Hz 包絡とキャリアは全モード共通で回し続ける ---
static inline void advancePhases() {
  ph10 += 2.0f * PI * 10.0f / HAP_RATE; if (ph10 > 2 * PI) ph10 -= 2 * PI;
  ph23 += 2.0f * PI * 23.0f / HAP_RATE; if (ph23 > 2 * PI) ph23 -= 2 * PI;
  phC  += 2.0f * PI * s_carrierHz / HAP_RATE; if (phC > 2 * PI) phC -= 2 * PI;
  phKA += 2.0f * PI * s_kaHz / HAP_RATE; if (phKA > 2 * PI) phKA -= 2 * PI;
}

// --- 対照条件アタリ 1サンプル: 10Hz + 23Hz 合成正弦の直接出力 (才木ら 2016 準拠) ---
//     ※ MAX98357A の ~14Hz HPF と加振器共振 (Fs≈50–60Hz) で大きく減衰する。
//       改良版 (タップ+AM) との対比実験用に残してある。
static float nibbleLegacySample(float amp, bool continuous) {
  float s = 0.58f * sinf(ph10) + 0.42f * sinf(ph23);

  if (continuous) return s * amp;     // BITE: 連続提示

  // NIBBLE: 数百ms のバーストを不定期に繰り返す (実釣アタリの不定期性を模擬)
  if (burstLeft <= 0 && gapLeft <= 0) {          // 次のバースト/間隔を抽選
    burstLeft = msToSamples(rnd(200, 500));
    gapLeft   = msToSamples(rnd(250, 800));
  }
  float target = 0;
  if (burstLeft > 0)      { burstLeft--; target = 1; }
  else if (gapLeft > 0)   { gapLeft--;   target = 0; }
  // 15ms 程度のアタック/リリース (1/(0.015*16000) ≈ 0.004)
  burstEnv += (target - burstEnv) * 0.004f * 8.0f;
  return s * amp * burstEnv;
}

// --- タップイベント: 短促クリック + 指数減衰する AM 余振 (方案一+二) ---
//     クリック (3ms 半正弦) が「竿を叩かれた」瞬態、その後
//     e^(-t/τ) × [10+23Hz 包絡] × sin(2π·fc·t) が竿体の余振。
//     低域はキャリア fc の包絡として表現するので、アンプの ~14Hz HPF と
//     加振器の共振特性 (Fs 以下急減衰) を回避して"手に届く"。
static void tapStart(float amp, int tauMs) {
  tapClickLen = msToSamples(TAP_CLICK_MS);
  int tauSamples = msToSamples(tauMs < 30 ? 30 : tauMs);
  tapK     = expf(-1.0f / (float)tauSamples);
  tapDecay = 1.0f;
  tapAmp   = amp;
  tapPos   = 0;
}

static float tapSample() {
  if (tapPos < 0) return 0;
  float v;
  if (tapPos < tapClickLen) {
    v = sinf(PI * tapPos / (float)tapClickLen);           // クリック: 半正弦パルス
  } else {
    tapDecay *= tapK;
    if (tapDecay < 0.02f) { tapPos = -1; return 0; }      // ~4τ で終了
    float envLow = 0.5f + 0.5f * (0.58f * sinf(ph10) + 0.42f * sinf(ph23));
    v = tapDecay * envLow * sinf(phC);                    // AM 余振
  }
  tapPos++;
  return tapAmp * v;
}

// --- ドラッグ滑りクリック 1サンプル: 3ms 半正弦 + 18ms の短い余韻 を
//     55-90ms 間隔で繰り返す =「ドラグがジジジと糸を出す」感触。
//     タップ(サージ/セグメント完了イベント)とは状態を分離し、干渉しない。
//     開始済みクリックは s_slipOn が落ちても最後まで再生 (切断ノイズ防止)。
static float slipSample() {
  if (slipPos < 0) {
    if (!s_slipOn) { slipNextIn = 0; return 0; }
    if (slipNextIn > 0) { slipNextIn--; return 0; }
    slipClickLen = msToSamples(3);
    slipPos = 0;
  }
  int ringLen = msToSamples(18);
  if (slipPos >= slipClickLen + ringLen) {
    slipPos = -1;
    slipNextIn = msToSamples(rnd(55, 90));
    return 0;
  }
  float v;
  if (slipPos < slipClickLen) {
    v = sinf(PI * slipPos / (float)slipClickLen);            // クリック本体
  } else {
    float d = (slipPos - slipClickLen) / (float)ringLen;     // 線形に消える余韻
    v = (1.0f - d) * 0.35f * sinf(phC);
  }
  slipPos++;
  return 0.85f * v;
}

// --- 改良版アタリ: タップイベント列のスケジューラ (方案二) ---
//     NIBBLE = 稀疏な単発「コツッ」(30% で二連打),
//     BITE   = 密な連打「ココココッ」+ 幅度漸強。
static void nibbleTapSchedule(float amp, bool bite) {
  if (tapNextIn > 0) { tapNextIn--; return; }
  int tau = s_tapTau;
  if (bite) {
    biteRamp += 0.08f; if (biteRamp > 1) biteRamp = 1;
    // 連打はやや短い余振で歯切れよく。振幅は 60%→100% へ漸強 + 少しばらつき
    tapStart(amp * (0.60f + 0.40f * biteRamp) * (0.85f + 0.15f * (rnd(0, 100) / 100.0f)),
             rnd(tau / 2, tau * 3 / 4));
    tapNextIn = msToSamples(rnd(70, 140));
  } else {
    tapStart(amp * (0.80f + 0.20f * (rnd(0, 100) / 100.0f)), rnd(tau * 3 / 4, tau * 5 / 4));
    // 30% で「コツコツッ」の二連打、それ以外は 350–900ms 空ける
    tapNextIn = msToSamples(rnd(0, 99) < 30 ? rnd(90, 150) : rnd(350, 900));
  }
}

// --- 引き 1サンプル: 5ms 高電圧 + T ms 低電圧 の非対称矩形波 (高椋ら 2016) ---
//     高区間 +A, 低区間 -A*(5/T) として時間平均ゼロ (DCフリー) を保つ。
//     振幅=引きの強さ, T=振動の粗さ。
//     不規則モードでは周期毎に T±2ms のジッタ + 低周波ゆらぎを加え、
//     厳密な周期性(=電動工具感)を崩す (高椋らが limitation に挙げた点)。
static int   pullHiLen = 0, pullPerLen = 0;   // 現周期の高区間/全体サンプル数
static float pullLowAmp = 0;                   // 現周期の低区間振幅
static float pullSample(float strength, int Tms) {
  if (Tms < 6) Tms = 6;
  if (pullPos >= pullPerLen) {                 // 周期の頭でパラメータ確定
    int Tj = Tms;
    if (s_irregular) {
      Tj += (int)(esp_random() % 5) - 2;       // T±2ms ジッタ
      if (Tj < 6) Tj = 6;
    }
    pullHiLen  = msToSamples(PULL_HIGH_MS);
    pullPerLen = pullHiLen + msToSamples(Tj);
    pullLowAmp = -((float)PULL_HIGH_MS / Tj);  // ジッタ後も DC フリーを維持
    pullPos = 0;
  }
  float v = (pullPos < pullHiLen) ? 1.0f : pullLowAmp;
  pullPos++;

  // 魚の暴れ: ~0.9Hz のゆらぎで振幅を 75〜100% で変動 (不規則モードのみ)
  float wobble = 1.0f;
  if (s_irregular) {
    lfoPh += 2.0f * PI * 0.9f / HAP_RATE; if (lfoPh > 2 * PI) lfoPh -= 2 * PI;
    wobble = 0.875f + 0.125f * sinf(lfoPh);
  }
  return PULL_POLARITY * v * strength * wobble;
}

// --- 鼓動 1サンプル: 25s 毎の「トク・トクン」心拍様2連パルス (待機触覚) ---
//     lub (120ms sin²) + 180ms 間 + dub (110ms sin², 0.75×) をキャリアに
//     乗せる。sin² の角のない包絡で、低振幅でも存在だけ伝わる設計。
//     副次効果: 判停窓 (30-60s) より短い間隔で電流が流れ、モバイル
//     バッテリーの低負荷自動判停 (充電完了誤認) を防ぐ。
//     (旧: 2.6s の渚スウェル + 連続底流。連続音が不評のため置換)
static float waveSample() {
  float amp = s_waveAmp;
  if (amp <= 0.001f) { wavePos = -1; return 0; }   // "wa 0" で無効
  if (wavePos < 0) {
    if (waveGap > 0) { waveGap--; return 0; }
    wavePos = 0;                                    // 新しい鼓動 (振幅±10%)
    waveAmpCur = amp * (0.90f + 0.10f * (rnd(0, 100) / 100.0f));
  }
  float t = wavePos / (float)HAP_RATE;
  wavePos++;
  if (t >= 0.42f) {                                 // 鼓動終了 → 次まで 間隔±10%
    wavePos = -1;
    int im = s_waveIntMs;
    waveGap = msToSamples(rnd(im * 9 / 10, im * 11 / 10));
    return 0;
  }
  float env = 0;
  if (t < 0.12f) {                                  // トク (lub)
    float x = sinf(PI * t / 0.12f);            env = x * x;
  } else if (t >= 0.30f && t < 0.41f) {             // トクン (dub, 少し弱く)
    float x = sinf(PI * (t - 0.30f) / 0.11f);  env = 0.75f * x * x;
  }
  return waveAmpCur * env * sinf(phC);
}

// --- 波形生成タスク: HAP_CHUNK フレームずつ合成して I2S へ ---
static void hapticTask(void*) {
  static int16_t buf[HAP_CHUNK * 2];             // ステレオ (L=R 同一)
  for (;;) {
    uint8_t want = s_mode;
    float   pStr = s_pullStrength;
    int     pT   = s_pullT;
    float   nAmp = s_nibbleAmp;

    // PULL 中のイベント要求を取り込む (方案四)。他モードでは破棄。
    if (modeApplied == HAPTIC_PULL) {
      if (s_slackReq > 0) { slackLeft = msToSamples(s_slackReq); s_slackReq = 0; }
      if (s_tapReq   > 0) {
        tapStart(s_tapReq, s_tapTauReq > 0 ? s_tapTauReq : rnd(60, 100));
        s_tapReq = 0; s_tapTauReq = 0;
      }
    } else {
      s_slackReq = 0; s_tapReq = 0; s_tapTauReq = 0;
    }

    for (int i = 0; i < HAP_CHUNK; i++) {
      // モード切替はクロスフェード: 一旦 0 まで落としてから切替 (ポップ音防止)
      float envTarget = (modeApplied == want) ? 1.0f : 0.0f;
      masterEnv += (envTarget - masterEnv) * 0.02f;      // ≈3ms 時定数
      if (modeApplied != want && masterEnv < 0.02f) {
        modeApplied = want;
        ph10 = ph23 = phC = lfoPh = 0; pullPos = 0;
        burstLeft = gapLeft = 0; burstEnv = 0;
        tapPos = -1; tapNextIn = 0; biteRamp = 0;
        slackLeft = 0; slackEnv = 1;
        tugLeft = 0; tugHigh = true; tugEnv = 1;
        slipPos = -1; slipNextIn = 0; slipDuck = 1;
        wavePos = -1;                                // 鼓動: 入場後 1.5-4s で最初の一拍
        waveGap = msToSamples(rnd(1500, 4000));
      }
      advancePhases();
      float s = 0;
      switch (modeApplied) {
        case HAPTIC_NIBBLE:
          if (s_irregular) { nibbleTapSchedule(nAmp, false); s = tapSample(); }
          else             s = nibbleLegacySample(nAmp, false);
          break;
        case HAPTIC_BITE:
          if (s_irregular) { nibbleTapSchedule(fminf(1.0f, nAmp * 1.6f), true); s = tapSample(); }
          else             s = nibbleLegacySample(fminf(1.0f, nAmp * 2.2f), true);
          break;
        case HAPTIC_PULL: {
          // slack: 振動を数百ms 骤停→復帰。「糸が緩んだ/まだ居る」の合図
          //        (SIGGRAPH Asia 2024: 突然の力消失は極めて強い実在感キュー)
          float target = 1.0f;
          if (slackLeft > 0) { slackLeft--; target = 0.0f; }
          slackEnv += (target - slackEnv) * 0.01f;        // ≈6ms 時定数

          // 引き込み節律 (HOLD 抗適応): 250-400ms 引く / 90-140ms ふっと緩む。
          // 小休止は 0.42 止まり (完全無音は slack の合図と紛れるため)
          float tugT = 1.0f;
          if (s_irregular && s_tugOn) {
            if (tugLeft <= 0) {
              tugHigh = !tugHigh;
              tugLeft = msToSamples(tugHigh ? rnd(250, 400) : rnd(90, 140));
            }
            tugLeft--;
            tugT = tugHigh ? 1.0f : 0.42f;
          } else { tugLeft = 0; tugHigh = true; }
          tugEnv += (tugT - tugEnv) * 0.004f;             // ≈16ms 時定数

          // ドラッグ滑り: 基礎張力を ~55% にダック (糸が出て張力が抜ける)
          slipDuck += ((s_slipOn ? 0.55f : 1.0f) - slipDuck) * 0.01f;

          s = pullSample(pStr, pT) * slackEnv * tugEnv * slipDuck;
          s += tapSample();                               // サージ瞬態の重畳
          s += slipSample();                              // ドラッグのクリック列
          if (s > 1) s = 1; if (s < -1) s = -1;
          break;
        }
        case HAPTIC_WAVE: {
          // 鼓動 (25s毎の心拍パルス) + 底流 (既定off, "wk" で復活可) の重畳。
          // wd=1 (既定) は純正弦。>1 でソフトクリップ (ピーク一定でRMS増)
          float k  = s_kaDrive;
          float uc = (k <= 1.001f) ? sinf(phKA)
                                   : tanhf(k * sinf(phKA)) / tanhf(k);
          s = waveSample() + s_kaAmp * uc;
          if (s > 1) s = 1; if (s < -1) s = -1;
          break;
        }
        default: s = 0; break;
      }
      int16_t v = (int16_t)(s * masterEnv * MASTER_GAIN * 32767.0f);
      buf[i * 2] = v; buf[i * 2 + 1] = v;
    }
    size_t written = 0;
    i2s_write(HAP_I2S_PORT, buf, sizeof(buf), &written, portMAX_DELAY);
  }
}

bool hapticInit() {
  i2s_config_t cfg = {};
  cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate          = HAP_RATE;
  cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count        = 6;
  cfg.dma_buf_len          = HAP_CHUNK;
  cfg.use_apll             = false;
  cfg.tx_desc_auto_clear   = true;                // アンダーラン時は無音を出す

  if (i2s_driver_install(HAP_I2S_PORT, &cfg, 0, nullptr) != ESP_OK) return false;

  i2s_pin_config_t pins = {};
  pins.mck_io_num   = I2S_PIN_NO_CHANGE;
  pins.bck_io_num   = HAP_PIN_BCLK;
  pins.ws_io_num    = HAP_PIN_LRC;
  pins.data_out_num = HAP_PIN_DIN;
  pins.data_in_num  = I2S_PIN_NO_CHANGE;
  if (i2s_set_pin(HAP_I2S_PORT, &pins) != ESP_OK) {
    i2s_driver_uninstall(HAP_I2S_PORT);
    return false;
  }
  i2s_zero_dma_buffer(HAP_I2S_PORT);

  // 波形合成タスク: Arduino loop (Core1) と分離して Core0 に固定
  xTaskCreatePinnedToCore(hapticTask, "haptic", 4096, nullptr, 5, nullptr, 0);
  s_ready = true;
  return true;
}

void hapticSetMode(HapticMode m)  { s_mode = m; }
HapticMode hapticGetMode()        { return (HapticMode)s_mode; }

void hapticSetPull(float strength, int intervalMs) {
  if (strength < 0) strength = 0; if (strength > 1) strength = 1;
  if (intervalMs < 6)  intervalMs = 6;
  if (intervalMs > 60) intervalMs = 60;
  s_pullStrength = strength;
  s_pullT        = intervalMs;
}
float hapticPullStrength() { return s_pullStrength; }
int   hapticPullInterval() { return s_pullT; }

void hapticSetNibbleAmp(float amp) {
  if (amp < 0) amp = 0; if (amp > 1) amp = 1;
  s_nibbleAmp = amp;
}
float hapticNibbleAmp() { return s_nibbleAmp; }

void hapticSetIrregular(bool on) { s_irregular = on; }

void hapticSetCarrier(float hz) {
  if (hz < 30)  hz = 30;
  if (hz > 150) hz = 150;
  s_carrierHz = hz;
}
float hapticCarrier() { return s_carrierHz; }

void hapticSetTapTau(int ms) {
  if (ms < 30)  ms = 30;
  if (ms > 400) ms = 400;
  s_tapTau = ms;
}
int hapticTapTau() { return s_tapTau; }

void hapticSetWaveAmp(float amp) {
  if (amp < 0) amp = 0; if (amp > 1) amp = 1;
  s_waveAmp = amp;
}
float hapticWaveAmp() { return s_waveAmp; }

void hapticSetWaveInterval(int sec) {
  if (sec < 5)   sec = 5;
  if (sec > 120) sec = 120;
  s_waveIntMs = sec * 1000;
}
int hapticWaveInterval() { return s_waveIntMs / 1000; }

void hapticSetUndercurrent(float amp) {
  if (amp < 0) amp = 0; if (amp > 1) amp = 1;
  s_kaAmp = amp;
}
float hapticUndercurrent() { return s_kaAmp; }

void hapticSetUndercurrentHz(float hz) {
  if (hz < 16) hz = 16;                 // MAX98357A の ~14Hz HPF より上
  if (hz > 45) hz = 45;                 // 共振点 (Fs≈50-60Hz) より下に保つ
  s_kaHz = hz;
}
float hapticUndercurrentHz() { return s_kaHz; }

void hapticSetUndercurrentDrive(float k) {
  if (k < 1) k = 1;
  if (k > 4) k = 4;
  s_kaDrive = k;
}
float hapticUndercurrentDrive() { return s_kaDrive; }

void hapticSetTug(bool on) { s_tugOn = on; }
bool hapticTug()           { return s_tugOn; }

void hapticSetSlip(bool on) { s_slipOn = on; }

void hapticTriggerSlack(int ms) {
  if (ms < 50)  ms = 50;
  if (ms > 500) ms = 500;
  s_slackReq = ms;
}

void hapticTriggerTap(float amp, int tauMs) {
  if (amp < 0) amp = 0; if (amp > 1) amp = 1;
  if (tauMs < 0) tauMs = 0; if (tauMs > 400) tauMs = 400;
  s_tapTauReq = tauMs;          // 先に τ を置いてから amp で発火 (タスク側は amp を見る)
  s_tapReq = amp;
}

bool hapticReady() { return s_ready; }
