// =============================================================================
//  疑似釣り体験プロトタイプ  ―  完全钓鱼体验シミュレータ (単体デモ版)
//  Fishing Experience Simulator  ―  runs on a single M5StickS3
//
//  JAIST 木谷研究室 / VR 釣り具プロトタイプ
//
//  ＜このファイルについて / About this build＞
//   外付け MAX98357A + exciter 無しでも「研究ストーリー全体」を語れるよう、
//   M5StickS3 単体 (LCD + IMU + Button) で釣りの一連の流れを再現するデモ。
//   No external amplifier / exciter required ― everything is shown on the LCD.
//
//  ＝＝＝ 8 状態と研究上の意味 / 8 states & their research meaning ＝＝＝
//   1. IDLE      待機      : 開始待ち。竿は静止。               (demo拡張)
//   2. CASTING   投竿      : キャスト動作の可視化。             (demo拡張)
//   3. WAITING   待ち      : 当たりを待つ時間。水波アニメ。     (demo拡張)
//   4. NIBBLE    前アタリ  : 魚が餌を突く"微振動"。10/23Hz 提示の
//                            視覚的代替 (画面の微小シェイク)。   ★才木ら 2016
//   5. BITE      本アタリ  : 合わせ(ジャーク)を要求する勝負所。
//                            IMU で挥竿動作を検出。              ★才木ら 2016
//   6. FIGHTING  やり取り  : 牽引力錯覚 = 引き感覚。のべ竿は巻けない為、
//                            竿を上に煽る(ポンピング/上提)で寄せる。  ★高椋ら 2016
//   7. CAUGHT    キャッチ  : 釣果(魚種・サイズ)を提示。         (demo拡張)
//   8. FAILED    バラし    : 失敗。原因を提示。                 (demo拡張)
//
//   実機(exciter)版では NIBBLE=10Hz+23Hz 合成正弦波、FIGHTING=5ms高/17ms低の
//   非対称矩形波 を I2S から提示する。本デモはその"体験設計"を視覚で説明する。
// =============================================================================

#include <M5Unified.h>
#include <math.h>

// ===== 状態定義 / State machine =====
enum State {
  IDLE, CASTING, WAITING, NIBBLE, BITE, FIGHTING, CAUGHT, FAILED
};

static const char* stateName(State s) {
  switch (s) {
    case IDLE:     return "IDLE";
    case CASTING:  return "CASTING";
    case WAITING:  return "WAITING";
    case NIBBLE:   return "NIBBLE";
    case BITE:     return "BITE";
    case FIGHTING: return "FIGHTING";
    case CAUGHT:   return "CAUGHT";
    case FAILED:   return "FAILED";
  }
  return "?";
}

// ===== チューニング定数 / Tunables =====
#define CAST_MS          1000     // 投竿アニメ時間
#define WAIT_MIN_MS      3000     // 待ち時間 下限
#define WAIT_MAX_MS      8000     // 待ち時間 上限
#define NIBBLE_MS        1500     // 前アタリ持続
#define BITE_WINDOW_MS   1000     // 合わせ猶予 (この間にジャーク)
#define CAUGHT_MS        3000     // 釣果表示
#define FAILED_MS        2000     // 失敗表示

// ===== BITE: 合わせ(あおり) / hook-set jerk =====
#define JERK_THRESHOLD_G 1.5f     // |加速度| > 1.5g で「合わせ」成立

// ===== FIGHTING: やり取り (Stardew Valley / Sea of Thieves 風の"ゾーン維持") =====
//   のべ竿はリールで巻けない。竿を上に煽る(上提)とマーカーが上昇し、
//   魚の引きでマーカーは下方へ"ランダムな速度"で減衰する。
//   2本の赤線で挟んだスイートゾーン内にマーカーを保ち続け、累計が
//   ランダムな目標時間に達すると捕獲。ゾーン外が続くと逃げられる。
//   ※張力概念は廃止(ゾーンを上に外す=強引/下に外す=緩み を一本化)。
#define ROD_START        50.0f    // マーカー初期位置 0-100 (ゾーン中央)
#define ZONE_LO          32.0f    // スイートゾーン下限(左の赤線) ※横バー
#define ZONE_HI          72.0f    // スイートゾーン上限(右の赤線) ※幅40に拡大
#define PUMP_RISE        16.0f    // 1回の上提でマーカーが進む量
#define DRIFT_MIN        12.0f    // 通常の引き(減衰) 下限 [/秒] ※反応しやすく緩和
#define DRIFT_MAX        30.0f    // 通常の引き(減衰) 上限 [/秒]
#define DRIFT_SURGE      52.0f    // 突進(サージ)時の減衰 [/秒]
#define SURGE_CHANCE     10       // 再抽選毎にサージへ入る確率 [%]
#define DRIFT_REROLL_MIN 550      // 減衰速度の再抽選間隔 下限 [ms]
#define DRIFT_REROLL_MAX 1100     // 同 上限 [ms]
#define HOLD_TARGET_MIN  3000     // 捕獲に必要な"ゾーン維持"累計時間 下限 [ms]
#define HOLD_TARGET_MAX  5000     // 同 上限 [ms] (毎回ランダム)
#define ESCAPE_LIMIT     2200     // ゾーン外が連続このmsを超えるとバラし
#define PUMP_GYRO_FIRE   160.0f   // 上提検出: 角速度[deg/s] 立ち上がり閾値
#define PUMP_GYRO_ARM    70.0f    // 角速度がこれ未満に戻ると次の上提を再武装
#define PUMP_REFRACT_MS  130      // 連続検出の不応期 [ms]

// ===== 釣果テーブル / Catch table =====
static const char* FISH[] = { "Bass 23cm", "Trout 31cm", "Carp 45cm", "Perch 18cm" };
static const int   NUM_FISH = sizeof(FISH) / sizeof(FISH[0]);

// ===== グローバル状態 / Globals =====
State        state        = IDLE;
uint32_t     stateStart   = 0;       // 現状態に入った時刻
uint32_t     waitMs       = 0;       // WAITING のランダム待ち時間
float        rodPos       = ROD_START;      // FIGHTING: マーカー位置 0-100
uint32_t     holdTime     = 0;              // ゾーン内維持の累計 [ms]
uint32_t     holdTarget   = 4000;           // 捕獲に必要な維持時間 (enterで乱数)
uint32_t     escapeTime   = 0;              // ゾーン外の連続時間 [ms]
float        driftPerSec  = 28.0f;          // 現在の下方減衰速度 [/秒]
uint32_t     driftReroll  = 0;              // 次に減衰を再抽選する時刻
uint32_t     fightTick    = 0;              // 前回更新時刻 (dt算出用)
uint32_t     lastPumpAt   = 0;              // 直近の上提時刻
bool         pumpArmed    = true;           // 上提検出の再武装フラグ
uint32_t     pumpFlashAt  = 0;              // 上提演出(閃光)タイマ
int          caughtIdx    = 0;
const char*  failReason   = "";
int          runCount      = 0;       // 何回目のキャスト (会话编号)
bool         imuOk        = false;

// 描画ターゲット (オフスクリーン) / Off-screen render target
M5Canvas canvas(&M5.Display);
bool     useSprite = false;
int      W = 0, H = 0;                // 画面サイズ (回転後)
int      shakeX = 0, shakeY = 0;      // 画面シェイク量
uint16_t curBg  = BLACK;             // 現フレームの背景色

// レイアウト (setup で H から算出) / Layout bands
int TITLE_Y, SUB_Y, ZONE_Y, ZONE_BOTTOM, ZONE_H, STATUS_Y;

// =============================================================================
//  音效 / Sound effects  ―  内蔵スピーカ(M5.Speaker), 経典釣りゲーム風
//  すべて非ブロッキング。チャンネル0固定のモノ発声で旋律を作る。
// =============================================================================
bool     speakerOk = false;
uint32_t sfxTick   = 0;        // 連続音(アラーム/ティック/警告)のタイマ
bool     sfxToggle = false;    // 2音アラームのトグル

struct Note { uint16_t freq; uint16_t ms; };  // 周波数[Hz] と長さ[ms]

// 単音発声: ch0 固定・直前の音を置換 / single mono voice on channel 0
void beep(int freq, int ms) {
  if (speakerOk && freq > 0) M5.Speaker.tone((float)freq, (uint32_t)ms, 0, true);
}

// --- イベント用ジングル / one-shot jingles ---
const Note SFX_CAST[]    = {{1568,40},{1175,40},{880,40},{659,60}};            // 投竿 swoosh↓
const Note SFX_PLOP[]    = {{300,50},{180,70}};                                 // 着水 plop
const Note SFX_HOOKSET[] = {{523,55},{659,55},{784,55},{1047,150}};             // 合わせ成功↑
const Note SFX_CAUGHT[]  = {{784,110},{1047,110},{1319,110},
                            {1047,90},{1319,90},{1568,340}};                    // 勝利ファンファーレ
const Note SFX_FAILED[]  = {{440,160},{392,160},{330,160},{262,360}};           // バラし sad↓

// 非ブロッキング メロディ再生器 / non-blocking melody sequencer (ch0)
const Note* mel = nullptr;
int         melLen = 0, melIdx = 0;
uint32_t    melNext = 0;
void playMelody(const Note* m, int len) { mel = m; melLen = len; melIdx = 0; melNext = 0; }
void updateMelody() {
  if (!mel) return;
  if (millis() >= melNext) {
    if (melIdx >= melLen) { mel = nullptr; return; }
    const Note& n = mel[melIdx++];
    beep(n.freq, n.ms);
    melNext = millis() + n.ms;
  }
}
#define MEL(arr) playMelody(arr, sizeof(arr) / sizeof((arr)[0]))

// -----------------------------------------------------------------------------
// ログ出力 / Serial trace : 状態名・タイムスタンプ・主要パラメータ
// -----------------------------------------------------------------------------
void logState() {
  Serial.printf("[%8lu ms] STATE -> %-8s (run #%d)",
                (unsigned long)millis(), stateName(state), runCount);
  switch (state) {
    case WAITING:  Serial.printf("  waitMs=%lu", (unsigned long)waitMs); break;
    case FIGHTING: Serial.printf("  holdTarget=%lu ms", (unsigned long)holdTarget); break;
    case CAUGHT:   Serial.printf("  fish=%s", FISH[caughtIdx]);         break;
    case FAILED:   Serial.printf("  reason=%s", failReason);            break;
    default: break;
  }
  Serial.println();
}

// 状態遷移 / Enter a new state and run its init
void enterState(State s) {
  state      = s;
  stateStart = millis();
  sfxTick = 0; sfxToggle = false;                  // 連続音タイマをリセット
  switch (s) {
    case CASTING:  MEL(SFX_CAST); break;                                          // 嗖
    case WAITING:  waitMs = random(WAIT_MIN_MS, WAIT_MAX_MS + 1); MEL(SFX_PLOP); break; // 扑通
    case FIGHTING:                                                                // 叮咚↗
      rodPos = ROD_START; holdTime = 0; escapeTime = 0;
      holdTarget  = random(HOLD_TARGET_MIN, HOLD_TARGET_MAX + 1);
      driftPerSec = random((int)DRIFT_MIN, (int)DRIFT_MAX + 1);
      driftReroll = millis() + random(DRIFT_REROLL_MIN, DRIFT_REROLL_MAX + 1);
      fightTick = millis(); lastPumpAt = 0; pumpArmed = true; pumpFlashAt = 0;
      MEL(SFX_HOOKSET); break;
    case CAUGHT:   caughtIdx = random(0, NUM_FISH); MEL(SFX_CAUGHT); break;        // 胜利号角
    case FAILED:   MEL(SFX_FAILED); break;                                         // 失落音↘
    default: break;
  }
  logState();
}

// =============================================================================
//  描画ヘルパ / Drawing helpers (全て canvas に描く)
// =============================================================================

// 中央寄せテキスト / Centered text. 背景は curBg と同色で透過風に。
void textC(const char* s, int cx, int y, int size, uint16_t fg) {
  canvas.setTextSize(size);
  canvas.setTextColor(fg, curBg);
  int tw = canvas.textWidth(s);
  canvas.setCursor(cx - tw / 2, y);
  canvas.print(s);
}

// 竿先(ロッドティップ)を二次ベジェで描く / Rod tip via quadratic Bezier
//   curveX : ティップの横ずれ,  tipPull : ティップを下げる量,  bow : しなり
void drawRod(float curveX, float tipPull, float bow, uint16_t col) {
  float rootX = W / 2.0f, rootY = ZONE_BOTTOM;
  float tipX  = rootX + curveX;
  float tipY  = ZONE_Y + tipPull;
  float ctrlX = rootX + curveX * 0.5f + bow;
  float ctrlY = (rootY + tipY) * 0.5f;
  float px = rootX, py = rootY;
  for (int i = 1; i <= 16; i++) {
    float t = i / 16.0f, mt = 1.0f - t;
    float x = mt * mt * rootX + 2 * mt * t * ctrlX + t * t * tipX;
    float y = mt * mt * rootY + 2 * mt * t * ctrlY + t * t * tipY;
    canvas.drawLine((int)px, (int)py, (int)x, (int)y, col);
    canvas.drawLine((int)px + 1, (int)py, (int)x + 1, (int)y, col); // 少し太く
    px = x; py = y;
  }
  canvas.fillCircle((int)tipX, (int)tipY, 2, col); // 竿先マーカー
}

// 釣った魚 / A little fish icon
void drawFish(int cx, int cy, int r, uint16_t col) {
  canvas.fillEllipse(cx, cy, r, r / 2, col);                              // 胴体
  canvas.fillTriangle(cx - r, cy, cx - r - r / 2, cy - r / 2,
                      cx - r - r / 2, cy + r / 2, col);                  // 尾びれ
  canvas.fillCircle(cx + r / 2, cy - r / 6, 2, BLACK);                   // 目
}

// 上部の会话编号 / top-right run counter, 下部の状態名 / bottom status label
void drawChrome() {
  char buf[24];
  snprintf(buf, sizeof(buf), "Run #%d", runCount);
  canvas.setTextSize(1);
  canvas.setTextColor(WHITE, curBg);
  int tw = canvas.textWidth(buf);
  canvas.setCursor(W - tw - 2, 2);
  canvas.print(buf);

  snprintf(buf, sizeof(buf), "[ %s ]", stateName(state));
  textC(buf, W / 2, STATUS_Y, 1, WHITE);
}

// オフスクリーンを実画面へ。シェイク量だけずらして push し、端を curBg で補う。
// Push the off-screen buffer with the current shake offset.
void present() {
  if (!useSprite) return;                 // フォールバック時は直接描画済み
  canvas.pushSprite(shakeX, shakeY);
  if      (shakeX > 0) M5.Display.fillRect(0, 0, shakeX, H, curBg);
  else if (shakeX < 0) M5.Display.fillRect(W + shakeX, 0, -shakeX, H, curBg);
  if      (shakeY > 0) M5.Display.fillRect(0, 0, W, shakeY, curBg);
  else if (shakeY < 0) M5.Display.fillRect(0, H + shakeY, W, -shakeY, curBg);
}

// =============================================================================
//  各状態の描画 / Per-state rendering. 背景塗り → ゾーン描画 → 文字 → chrome
// =============================================================================
void renderScene() {
  uint32_t el = millis() - stateStart;   // 現状態の経過時間
  shakeX = shakeY = 0;

  switch (state) {

    case IDLE: {
      curBg = BLACK; canvas.fillScreen(curBg);
      textC("Fishing Sim", W / 2, TITLE_Y, 2, WHITE);
      textC("Press BtnA to CAST", W / 2, SUB_Y, 1, CYAN);
      drawRod(2.0f * sinf(millis() * 0.003f), 2.0f, 0.0f, WHITE);  // 静止(微揺れ)
      break;
    }

    case CASTING: {
      curBg = BLUE; canvas.fillScreen(curBg);
      textC("CASTING...", W / 2, TITLE_Y, 2, WHITE);
      // 抛物线: 5 个暗点 + 1 个亮"ルアー"沿弧线移动
      float x0 = 20, x1 = W - 20, baseY = ZONE_BOTTOM - 4, amp = ZONE_H - 8;
      for (int i = 0; i < 5; i++) {
        float t = i / 4.0f;
        int dx = (int)(x0 + t * (x1 - x0));
        int dy = (int)(baseY - sinf(PI * t) * amp);
        canvas.fillCircle(dx, dy, 2, WHITE & 0x7BEF);             // 暗い点
      }
      float p  = (float)el / CAST_MS; if (p > 1) p = 1;
      int lx = (int)(x0 + p * (x1 - x0));
      int ly = (int)(baseY - sinf(PI * p) * amp);
      canvas.fillCircle(lx, ly, 4, YELLOW);                        // ルアー先端
      break;
    }

    case WAITING: {
      curBg = NAVY; canvas.fillScreen(curBg);
      textC("Waiting...", W / 2, TITLE_Y, 2, WHITE);
      textC("watch the float", W / 2, SUB_Y, 1, SKYBLUE);
      // 水波 2 本 (時間で位相をずらす) / two scrolling wave lines
      float ph = millis() * 0.006f;
      int y1 = ZONE_BOTTOM - 14, y2 = ZONE_BOTTOM - 4;
      for (int x = 0; x < W; x += 3) {
        int wy1 = y1 + (int)(3 * sinf(x * 0.10f + ph));
        int wy2 = y2 + (int)(3 * sinf(x * 0.13f + ph * 1.4f));
        canvas.drawPixel(x, wy1, CYAN);
        canvas.drawPixel(x, wy2, SKYBLUE);
      }
      // 浮き(bobber)が波で上下 / a bobbing float
      int bx = W / 2;
      int by = y1 + (int)(3 * sinf(bx * 0.10f + ph)) - 4;
      canvas.fillCircle(bx, by, 3, RED);
      canvas.drawLine(bx, by, bx, by + 6, WHITE);
      break;
    }

    case NIBBLE: {
      // 黄色背景 + 画面微小シェイク(±2px) = 微振動(10/23Hz)の視覚代替
      curBg = YELLOW; canvas.fillScreen(curBg);
      textC("Nibble...", W / 2, TITLE_Y, 2, BLACK);
      textC("fish is testing", W / 2, SUB_Y, 1, MAROON);
      float w = 4.0f * sinf(millis() * 0.02f);
      drawRod(w, 4.0f, w * 1.5f, BLACK);                           // 竿先 小刻みに揺れる
      shakeX = (int)roundf(2.0f * sinf(millis() * 0.04f));         // ±2px
      shakeY = (int)roundf(2.0f * sinf(millis() * 0.05f));
      break;
    }

    case BITE: {
      // 赤い点滅 + 激しいシェイク(±5px)。1 秒以内に合わせ(ジャーク)せよ
      bool flash = ((millis() / 120) % 2) == 0;
      curBg = flash ? RED : BLACK; canvas.fillScreen(curBg);
      textC("BITE!!",   W / 2, TITLE_Y, 2, flash ? WHITE : RED);
      textC("JERK NOW!", W / 2, SUB_Y, 1, flash ? YELLOW : WHITE);
      drawRod(8.0f, ZONE_H * 0.55f, 16.0f, WHITE);                 // 竿先 大きく下に曲がる
      // 合わせ猶予の残りバー / shrinking window bar
      float rem = 1.0f - (float)el / BITE_WINDOW_MS; if (rem < 0) rem = 0;
      int bw = (int)((W - 20) * rem);
      canvas.fillRect(10, ZONE_BOTTOM, bw, 4, YELLOW);
      shakeX = random(-5, 6);                                      // ±5px ガクガク
      shakeY = random(-3, 4);
      break;
    }

    case FIGHTING: {
      curBg = MAROON; canvas.fillScreen(curBg);
      textC("FIGHTING!", W / 2, 4, 2, WHITE);
      int hp = (int)(100.0f * holdTime / holdTarget); if (hp > 100) hp = 100;
      char b[24]; snprintf(b, sizeof(b), "CATCH %d%%", hp);
      textC(b, W / 2, 26, 1, WHITE);

      // 横バー (画面長辺いっぱい): 左=0, 右=100 / horizontal gauge along long edge
      int barX = 8, barW = W - 16;
      int barY = 56, barH = 40;
      float rp = rodPos < 0 ? 0 : rodPos > 100 ? 100 : rodPos;
      canvas.fillRect(barX, barY, barW, barH, BLACK);
      canvas.drawRect(barX - 1, barY - 1, barW + 2, barH + 2, WHITE);

      int xLo = barX + (int)(barW * ZONE_LO / 100.0f);     // 左の赤線
      int xHi = barX + (int)(barW * ZONE_HI / 100.0f);     // 右の赤線
      int mx  = barX + (int)(barW * rp / 100.0f);
      bool inZone = (rodPos >= ZONE_LO && rodPos <= ZONE_HI);

      // スイートゾーン帯 + 維持の充能(左から緑) / sweet zone + charge fill
      canvas.fillRect(xLo, barY, xHi - xLo, barH, inZone ? DARKGREEN : NAVY);
      int bandW = xHi - xLo;
      int fillW = (int)(bandW * holdTime / holdTarget); if (fillW > bandW) fillW = bandW;
      canvas.fillRect(xLo, barY, fillW, barH, GREEN);
      canvas.drawFastVLine(xLo, barY, barH, RED);          // 2本の赤線
      canvas.drawFastVLine(xHi, barY, barH, RED);

      // マーカー(竿/魚の位置) = 縦の太線 / marker
      bool flash = (millis() - pumpFlashAt < 90);
      canvas.fillRect(mx - 3, barY - 4, 6, barH + 8, flash ? WHITE : (inZone ? CYAN : ORANGE));

      // ゾーン外は枠を赤点滅で警告 / out-of-zone warning border
      if (!inZone) {
        uint16_t wc = ((millis() / 120) % 2) ? RED : MAROON;
        canvas.drawRect(barX - 3, barY - 4, barW + 6, barH + 8, wc);
      }

      // バー下に動作ヒント / action hint under the bar
      const char* hint = inZone ? "HOLD !" : (rodPos < ZONE_LO ? "PUMP UP ->" : "<- ease off");
      textC(hint, W / 2, barY + barH + 8, 1, inZone ? GREEN : ORANGE);

      shakeX = random(-2, 3);
      if (flash) shakeY = -3;                              // 上提でクッと
      break;
    }

    case CAUGHT: {
      curBg = DARKGREEN; canvas.fillScreen(curBg);
      textC("CAUGHT!", W / 2, TITLE_Y, 2, WHITE);
      textC(FISH[caughtIdx], W / 2, SUB_Y, 1, YELLOW);
      drawFish(W / 2, ZONE_Y + ZONE_H / 2 + 2, 18, SILVER);
      break;
    }

    case FAILED: {
      curBg = DARKGREY; canvas.fillScreen(curBg);
      textC("Fish escaped...", W / 2, TITLE_Y, 2, WHITE);
      textC(failReason, W / 2, SUB_Y, 1, RED);
      drawRod(0.0f, 1.0f, 0.0f, BLACK);                            // 力なく直立
      break;
    }
  }

  drawChrome();
}

// =============================================================================
//  状態ロジック / Per-state logic & transitions
// =============================================================================
void updateLogic() {
  uint32_t el = millis() - stateStart;

  switch (state) {

    case IDLE:
      if (M5.BtnA.wasPressed()) { runCount++; enterState(CASTING); }
      break;

    case CASTING:
      if (el >= CAST_MS) enterState(WAITING);
      break;

    case WAITING:
      if (el >= waitMs) enterState(NIBBLE);
      break;

    case NIBBLE:
      // 魚が餌を突く"嘀…嘀…"ティック / soft nibble ticks
      if (millis() - sfxTick >= 280) { beep(2200, 22); sfxTick = millis(); }
      if (el >= NIBBLE_MS) enterState(BITE);
      break;

    case BITE: {
      // 緊急アラーム ピピピ! (2音トグル) / urgent two-tone bite alarm
      if (millis() - sfxTick >= 85) {
        beep(sfxToggle ? 2500 : 1900, 85);
        sfxToggle = !sfxToggle; sfxTick = millis();
      }
      // IMU で合わせ(ジャーク)動作を検出。|加速度| > 1.5g で成立。
      float ax = 0, ay = 0, az = 0;
      if (imuOk) M5.Imu.getAccel(&ax, &ay, &az);
      float mag = sqrtf(ax * ax + ay * ay + az * az);
      // BtnA はバックアップ (IMU 不調・展示時の保険)
      if (mag > JERK_THRESHOLD_G || M5.BtnA.wasPressed()) {
        Serial.printf("    >> JERK detected  |a|=%.2fg\n", mag);
        enterState(FIGHTING);
      } else if (el >= BITE_WINDOW_MS) {
        failReason = "Too slow!";
        enterState(FAILED);
      }
      break;
    }

    case FIGHTING: {
      uint32_t now = millis();
      uint32_t dt  = now - fightTick; fightTick = now;
      if (dt > 100) dt = 100;                            // 取りこぼし時のクランプ

      // --- 魚の引き(下方減衰)をランダムに再抽選 / re-roll random drift ---
      if (now >= driftReroll) {
        if ((int)random(0, 100) < SURGE_CHANCE) driftPerSec = DRIFT_SURGE;   // 突進
        else driftPerSec = random((int)DRIFT_MIN, (int)DRIFT_MAX + 1);
        driftReroll = now + random(DRIFT_REROLL_MIN, DRIFT_REROLL_MAX + 1);
      }
      rodPos -= driftPerSec * dt / 1000.0f;              // マーカーは下へ

      // --- 上提(煽り)検出: 角速度スパイク + ヒステリシス + 不応期 ---
      float gx = 0, gy = 0, gz = 0;
      if (imuOk) M5.Imu.getGyro(&gx, &gy, &gz);
      float gmag = sqrtf(gx * gx + gy * gy + gz * gz);   // [deg/s]
      if (gmag < PUMP_GYRO_ARM) pumpArmed = true;        // 竿が戻ったら再武装
      bool pumped = false;
      if (pumpArmed && gmag > PUMP_GYRO_FIRE && now - lastPumpAt > PUMP_REFRACT_MS) {
        pumped = true; pumpArmed = false;
      }
      if (M5.BtnA.wasPressed()) pumped = true;           // バックアップ(展示保険)
      if (pumped) { rodPos += PUMP_RISE; lastPumpAt = now; pumpFlashAt = now; }

      if (rodPos > 100) rodPos = 100;
      if (rodPos < 0)   rodPos = 0;

      // --- ゾーン判定 & タイマ ---
      uint32_t el = now - stateStart;
      bool inZone = (rodPos >= ZONE_LO && rodPos <= ZONE_HI);
      if (inZone) {
        holdTime += dt; escapeTime = 0;
        if (pumped && el > 350) {                        // 維持が進むほど高音=爽快
          int f = 900 + (int)(900.0f * holdTime / holdTarget);
          if (f > 1900) f = 1900;
          beep(f, 20);
        }
      } else {
        escapeTime += dt;
        if (pumped && el > 350) beep(520, 22);           // ゾーン外の上提は鈍い音
        if (el > 350 && now - sfxTick >= 220) {          // 逃走警告
          beep(330, 55); sfxTick = now;
        }
      }

      // --- 判定 ---
      if (holdTime >= holdTarget) {
        enterState(CAUGHT);
      } else if (rodPos <= 0.0f || escapeTime >= ESCAPE_LIMIT) {
        Serial.printf("    >> FISH RAN OFF (escapeTime=%lu ms)\n", (unsigned long)escapeTime);
        failReason = "Ran off!"; enterState(FAILED);
      }
      break;
    }

    case CAUGHT:
      if (el >= CAUGHT_MS) enterState(IDLE);
      break;

    case FAILED:
      if (el >= FAILED_MS) enterState(IDLE);
      break;
  }
}

// =============================================================================
//  setup / loop
// =============================================================================
void setup() {
  auto cfg = M5.config();
  cfg.internal_spk = true;                  // 内蔵スピーカを有効化 / enable buzzer
  M5.begin(cfg);
  Serial.begin(115200);

  M5.Display.setRotation(1);
  M5.Display.setBrightness(255);            // 最亮 / max brightness
  W = M5.Display.width();
  H = M5.Display.height();

  // レイアウト帯を画面高から算出 / derive layout bands from height
  TITLE_Y     = (int)(H * 0.13f);
  SUB_Y       = (int)(H * 0.30f);
  ZONE_Y      = (int)(H * 0.42f);
  ZONE_BOTTOM = H - 14;
  ZONE_H      = ZONE_BOTTOM - ZONE_Y;
  STATUS_Y    = H - 10;

  // オフスクリーン確保 (16bit→失敗時 8bit) / off-screen buffer
  canvas.setColorDepth(16);
  if (canvas.createSprite(W, H)) {
    useSprite = true;
  } else {
    canvas.setColorDepth(8);
    useSprite = canvas.createSprite(W, H);
  }
  canvas.setTextWrap(false);

  imuOk = M5.Imu.begin();                   // 内蔵 IMU (BMI270)

  speakerOk = M5.Speaker.begin();           // 内蔵スピーカ / buzzer
  M5.Speaker.setVolume(180);                // 0-255。爽快感のため大きめ

  randomSeed(esp_random());

  Serial.println("=== Fishing Experience Simulator (single-device demo) ===");
  Serial.printf("Display %dx%d  sprite=%s  IMU=%s  SPK=%s\n",
                W, H, useSprite ? "on" : "off",
                imuOk ? "ok" : "N/A", speakerOk ? "ok" : "N/A");

  enterState(IDLE);
}

void loop() {
  M5.update();
  updateLogic();
  updateMelody();                           // ジングルを1ノートずつ進める
  renderScene();
  present();
  delay(16);                                // ~60fps
}
