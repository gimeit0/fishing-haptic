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
#define PULL_INTERVAL_DEF  17       // パルス間隔T の既定値 [ms] (12ms以上を推奨)
#define PULL_POLARITY      (+1.0f)  // 牽引方向。取付向きに合わせ ±1 で反転可
#define NIBBLE_AMP_DEF     0.55f    // アタリ振幅。才木らの主旨は「手元で知覚できない
                                    // アタリの増幅提示」なので、知覚しやすい水準を既定にする。
                                    // 知覚閾の探索実験では "na" コマンドで下げて調整。
#define MASTER_GAIN        0.90f    // クリップ余裕

// ===== loop() 側から書かれる共有パラメータ (単語アクセスなので volatile で足りる) =====
static volatile uint8_t s_mode         = HAPTIC_OFF;
static volatile float   s_pullStrength = 0.7f;
static volatile int     s_pullT        = PULL_INTERVAL_DEF;
static volatile float   s_nibbleAmp    = NIBBLE_AMP_DEF;
static volatile bool    s_irregular    = true;
static bool             s_ready        = false;

// ===== タスク内 合成状態 =====
static float  ph10 = 0, ph23 = 0;     // アタリ正弦波の位相
static int    pullPos = 0;            // 非対称波 1周期内のサンプル位置
static float  lfoPh   = 0;            // 引きの"暴れ"用 低周波ゆらぎ位相
static float  masterEnv = 0;          // モード切替クロスフェード用 0..1
static uint8_t modeApplied = HAPTIC_OFF;
// アタリのバースト管理 (サンプル数でカウント)
static int    burstLeft = 0, gapLeft = 0;
static float  burstEnv  = 0;          // バースト内エンベロープ (クリック防止)

static inline int msToSamples(int ms) { return ms * HAP_RATE / 1000; }
static inline int rnd(int lo, int hi) { return lo + (int)(esp_random() % (uint32_t)(hi - lo + 1)); }

// --- アタリ 1サンプル: 10Hz + 23Hz 合成 (才木ら 2016 の周波数ピークに基づく) ---
//     ※ MAX98357A 内蔵の ~14Hz HPF で 10Hz 成分は減衰するため若干重み付けする
static float nibbleSample(float amp, bool continuous) {
  const float W10 = 2.0f * PI * 10.0f / HAP_RATE;
  const float W23 = 2.0f * PI * 23.0f / HAP_RATE;
  ph10 += W10; if (ph10 > 2 * PI) ph10 -= 2 * PI;
  ph23 += W23; if (ph23 > 2 * PI) ph23 -= 2 * PI;
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

// --- 波形生成タスク: HAP_CHUNK フレームずつ合成して I2S へ ---
static void hapticTask(void*) {
  static int16_t buf[HAP_CHUNK * 2];             // ステレオ (L=R 同一)
  for (;;) {
    uint8_t want = s_mode;
    float   pStr = s_pullStrength;
    int     pT   = s_pullT;
    float   nAmp = s_nibbleAmp;

    for (int i = 0; i < HAP_CHUNK; i++) {
      // モード切替はクロスフェード: 一旦 0 まで落としてから切替 (ポップ音防止)
      float envTarget = (modeApplied == want) ? 1.0f : 0.0f;
      masterEnv += (envTarget - masterEnv) * 0.02f;      // ≈3ms 時定数
      if (modeApplied != want && masterEnv < 0.02f) {
        modeApplied = want;
        ph10 = ph23 = lfoPh = 0; pullPos = 0;
        burstLeft = gapLeft = 0; burstEnv = 0;
      }
      float s = 0;
      switch (modeApplied) {
        case HAPTIC_NIBBLE: s = nibbleSample(nAmp, false); break;
        case HAPTIC_BITE:   s = nibbleSample(fminf(1.0f, nAmp * 2.2f), true); break;
        case HAPTIC_PULL:   s = pullSample(pStr, pT); break;
        default:            s = 0; break;
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

bool hapticReady() { return s_ready; }
