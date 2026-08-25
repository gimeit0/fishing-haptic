

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
#include "haptics.h"   // I2S → MAX98357A → 加振器 触覚提示 (研究計画書 3.2)

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
#define NIBBLE_MIN_MS    3500     // 前アタリ持続 下限 (バースト4個以上を体感させる)
#define NIBBLE_MAX_MS    6000     // 同 上限 (毎回ランダム。BITE の到来を予測不能に)
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
#define PUMP_RISE        20.0f    // 1回の上提でマーカーが進む量 (長期戦向けに約1回/秒で維持可能に)
#define DRIFT_MIN        12.0f    // 通常の引き(減衰) 下限 [/秒] ※反応しやすく緩和
#define DRIFT_MAX        30.0f    // 通常の引き(減衰) 上限 [/秒]
#define DRIFT_SURGE      52.0f    // 突進(サージ)時の減衰 [/秒]
#define SURGE_CHANCE     10       // 再抽選毎にサージへ入る確率 [%]
#define DRIFT_REST       4.0f     // 休息(魚が引きを緩める)時の減衰 [/秒]
#define REST_CHANCE      18       // 再抽選毎に休息へ入る確率 [%]
#define DRIFT_REROLL_MIN 550      // 減衰速度の再抽選間隔 下限 [ms]
#define DRIFT_REROLL_MAX 1100     // 同 上限 [ms]
#define HOLD_TARGET_MIN  12000    // 捕獲に必要な"ゾーン維持"累計時間 下限 [ms]
#define HOLD_TARGET_MAX  18000    // 同 上限 [ms] (毎回ランダム)
                                  //   ※実測の全程体感は20-30秒。Sea of Thieves(通常魚~45秒)と
                                  //     Stardew Valley(~10-20秒)の中間を狙った値。
#define ESCAPE_LIMIT     3000     // ゾーン外が連続このmsを超えるとバラし (長期戦化に伴い緩和)
// 魚の疲労(burst-and-coast): 進度が上がるほど突進が減り休息が増える。
//   Bainbridge1958 [高椋論文の引用文献17]: 遊泳速度は尾振り「頻度」で決まり、
//   振幅は5Hz以上でほぼ一定。よって疲労は「爆発の頻度・密度の低下」として現れる
//   (振幅を均一に下げるのではなく、挙動をまばらにする)。触覚振幅は driftPerSec
//   由来なので、頻度が落ちれば手の感覚も自然に弱く・まばらになる。
//   ※実験で変数を切り分けたい時は両定数を0にすれば疲労弧線が消える。
#define FATIGUE_SURGE_FADE 0.7f   // 進度100%で突進確率が(1-0.7)=30%に減る (10%→約3%)
#define FATIGUE_REST_GAIN  20     // 進度100%で休息確率が+この値[%] (18%→約38%)
#define PUMP_GYRO_FIRE   160.0f   // 上提検出: 角速度[deg/s] 立ち上がり閾値
#define PUMP_GYRO_ARM    70.0f    // 角速度がこれ未満に戻ると次の上提を再武装
#define PUMP_REFRACT_MS  130      // 連続検出の不応期 [ms]

// ===== FIGHTING 第2モード「張力保持 (HOLD)」 (前田案, 2026-07-23 Slack) =====
//   竿を高く傾けて"保持"している間だけ魚の抵抗=振動を提示し、竿を戻すと
//   あえて無振動 (糸のテンションが抜けた状態)。傾きをセグメント時間だけ
//   保持するごとに魚が一段寄る (自責の念バー式の分段進度)。
//   魚サイズは振動の強弱で表現 (高椋ら2016: 振幅・周波数の制御で
//   魚サイズの印象を変調できる)。傾きは「静止時の重力ベクトル基線との
//   夾角」で測るため、デバイスの取付方向に依存しない。
#define TEN_TILT_ON      18.0f    // 基線から +この角度[deg] で「負荷中」判定
#define TEN_TILT_OFF     12.0f    // これ未満に戻ると「未負荷」(ヒステリシス)
#define TEN_TILT_FULL    45.0f    // この傾きで張力係数が最大 (×1.15)
#define TEN_TILT_OVER    55.0f    // これ超の保持は「強引すぎ」警告 (前田図の×)
#define TEN_OVER_MS      1200     // 強引すぎが連続このmsでラインブレイク
#define TEN_UNLOAD_LIMIT 6000     // 未負荷が連続このmsで魚に逃げられる
#define TEN_SEG_BACK     0.4f     // 未負荷中セグメント進度が戻る速度 (×実時間)
#define TEN_AMP_MIN      0.55f    // 最小サイズ魚の振幅係数 (最大サイズ=1.0)

// ===== HOLD 改良 P0 (先行研究_HOLD模式.md, 2026-08-26) =====
//   滑り: TILT_SLIP 超で「ドラグが滑る」= クリック列 + 進度後退。断線の
//         触覚的前兆 (突然の懲罰 → 兆候のある挽回可能状態へ)。
//   適応: 負荷中の連続振動は数秒で主観強度が落ちる (振動触覚適応) ため、
//         セグメント内で振幅を漸増して補償 + haptics 側の引き込み節律。
//   サイズ: 30Hz 側の低周波は「重い」と知覚される知見に基づき、魚サイズを
//         振幅だけでなくパルス間隔T (基本周波数) にも写像する。
#define TEN_TILT_SLIP    48.0f    // ドラッグ滑り開始角 [deg] ("hd" で調整)
#define TEN_SLIP_BACK    0.6f     // 滑り中セグメント進度が戻る速度 (×実時間)
#define TEN_ADAPT_GAIN   0.18f    // セグメント充填 100% 時の振幅漸増率 (+18%)
#define TEN_T_BIAS_SML  -3        // 最小魚の T バイアス [ms] → 周期短く軽快 (~55Hz)
#define TEN_T_BIAS_BIG   5        // 最大魚の T バイアス [ms] → 周期長く重い (~29Hz)
                                  //   周期 5+T が牽引錯覚の有効域 (40Hz±) を
                                  //   大きく外れない範囲に留めること

// ===== 触覚提示のマッピング / Haptic mapping =====
//   引き感 (高椋ら 2016): 振幅=引きの強さ, パルス間隔T=粗さ(暴れ感)。
//   ゲーム側の魚の引き driftPerSec と連動させ、サージ時は強く粗い引きにする。
//   T の範囲は周期 5+T が 40Hz (T=20ms) を跨ぐように取る
//   (Tanaka ら 2020: 非対称振動の牽引錯覚は 40Hz 付近が最適)。
#define PULL_STR_MIN     0.45f    // 通常の引きの最小振幅 (0..1)
#define PULL_STR_MAX     1.00f    // サージ時の振幅
#define PULL_T_SLOW      26       // 弱い引きのパルス間隔 [ms] (周期31ms ≈ 32Hz)
#define PULL_T_FAST      16       // 強い引きのパルス間隔 [ms] (周期21ms ≈ 48Hz)

// ===== 運動結合 (方案三) / Motion-coupled pseudo force =====
//   CHI 2025: 非対称振動をユーザ自身の動作に結合すると、感覚減衰により
//   「拉かれ感」を保ったまま不要な"嗡嗡感"が減る。竿が動いている時だけ
//   フル振幅を出し、静止中は張力の底ノイズまで落とす。
#define MC_FLOOR         0.35f    // 静止時の振幅下限 (張力の底ノイズ)
#define MC_GYRO_FULL     220.0f   // この角速度[deg/s]でゲート全開
#define MC_ATTACK        0.50f    // ゲート立ち上がり係数 (即応)
#define MC_RELEASE       0.06f    // ゲート減衰係数 (~300ms で静止レベルへ)

// ===== 釣果テーブル / Catch table =====
static const char* FISH[] = { "Bass 23cm", "Trout 31cm", "Carp 45cm", "Perch 18cm" };
static const int   NUM_FISH = sizeof(FISH) / sizeof(FISH[0]);

// ===== グローバル状態 / Globals =====
State        state        = IDLE;
uint32_t     stateStart   = 0;       // 現状態に入った時刻
uint32_t     waitMs       = 0;       // WAITING のランダム待ち時間
uint32_t     nibbleMs     = 4500;    // NIBBLE のランダム持続時間
float        rodPos       = ROD_START;      // FIGHTING: マーカー位置 0-100
uint32_t     holdTime     = 0;              // ゾーン内維持の累計 [ms]
uint32_t     holdTarget   = 15000;          // 捕獲に必要な維持時間 (enterで乱数)
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

// ===== FIGHTING 第2モード (張力保持) の状態 =====
int      fightMode   = 0;             // 0=PUMP(旧・既定) 1=HOLD(張力保持, "fm 1"/BtnB)
float    fishSize01  = 0.5f;          // 今回の魚サイズ 0..1 (振幅・段数・魚種に反映)
int      tenSegCount = 4;             // セグメント数 (サイズで 3–5)
uint32_t tenSegHold  = 3000;          // 1セグメントの必要保持時間 [ms]
int      tenSegDone  = 0;             // 完了セグメント数
uint32_t tenSegMs    = 0;             // 現セグメントの保持累計 [ms]
uint32_t tenOverMs   = 0;             // 強引すぎ(過傾)の連続時間 [ms]
uint32_t tenUnloadMs = 0;             // 未負荷の連続時間 [ms]
bool     tenLoaded   = false;         // 負荷中 (傾き保持中) か
bool     tenSlipping = false;         // ドラッグ滑り中 (過傾でクリック列+進度後退)
float    tenTiltSlip = TEN_TILT_SLIP; // 滑り開始角 ("hd" で実機調整)
float    tiltDeg     = 0;             // 基線からの傾き [deg]
float    restGX = 0, restGY = 0, restGZ = 1;  // 静止時の重力ベクトル基線
bool     restGInit   = false;
float    faX = 0, faY = 0, faZ = 1;   // 加速度の低域通過値 (揺れ除去)

// ===== 触覚チューニング (シリアルコマンドで調整, 予備評価用) =====
float hapPullScale   = 1.0f;   // 引き振幅の全体スケール ("pa 0.8")
int   hapPullTOverride = 0;    // >0 ならパルス間隔T を固定 [ms] ("pt 17", 0=自動)
int   hapTestMode    = -1;     // -1=ゲーム連動, それ以外は HapticMode を強制 ("t p" 等)
bool  hapIrregular   = true;   // 不規則性 on/off ("ir 0|1")。off=文献準拠の対照条件
bool  hapMotionCouple = true;  // 運動結合 on/off ("mc 0|1")。方案三の A/B 用
bool  hapTug         = true;   // HOLD 引き込み節律 (抗適応) on/off ("ht 0|1")。A/B 用
bool  hapSizeFreq    = true;   // HOLD 魚サイズ→T バイアス on/off ("hf 0|1")。A/B 用
float gyroMag        = 0;      // 直近の角速度絶対値 [deg/s] (updateLogic が更新)

// ===== 電源モニタ (外部給電か内蔵電池かの判定, 充電宝テスト用) =====
//   StickS3 は 250mAh 内蔵電池を持ち、外部電源が切れても画面は消えない。
//   BQ27220 電量計を 1 秒毎に読み、充電中フラグ・電流・残量を画面左上に表示:
//   充電中 or 電流≈0 = 外部給電 / 電流が負で残量低下 = 内蔵電池で動作中。
uint32_t pwrTick = 0;
bool     pwrChg  = false;   // M5.Power.isCharging()
int      pwrLvl  = -1;      // 残量 [%]
int      pwrCur  = 0;       // 電池電流 [mA] (正=充電, 負=放電)

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
    case NIBBLE:   Serial.printf("  nibbleMs=%lu", (unsigned long)nibbleMs); break;
    case FIGHTING: Serial.printf("  holdTarget=%lu ms mode=%s size=%.2f seg=%d x %lums",
                                 (unsigned long)holdTarget, fightMode ? "HOLD" : "PUMP",
                                 fishSize01, tenSegCount, (unsigned long)tenSegHold); break;
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
    case NIBBLE:   nibbleMs = random(NIBBLE_MIN_MS, NIBBLE_MAX_MS + 1); break;
    case FIGHTING:                                                                // 叮咚↗
      rodPos = ROD_START; holdTime = 0; escapeTime = 0;
      holdTarget  = random(HOLD_TARGET_MIN, HOLD_TARGET_MAX + 1);
      driftPerSec = random((int)DRIFT_MIN, (int)DRIFT_MAX + 1);
      driftReroll = millis() + random(DRIFT_REROLL_MIN, DRIFT_REROLL_MAX + 1);
      fightTick = millis(); lastPumpAt = 0; pumpArmed = true; pumpFlashAt = 0;
      // 魚サイズ抽選 (両モード共通): 振幅・段数・釣果魚種に反映
      fishSize01  = random(0, 1001) / 1000.0f;
      tenSegCount = 3 + (fishSize01 > 0.4f) + (fishSize01 > 0.75f);   // 3–5段
      tenSegHold  = 2500 + (uint32_t)(1500.0f * fishSize01);          // 2.5–4s/段
      tenSegDone = 0; tenSegMs = 0; tenOverMs = 0; tenUnloadMs = 0;
      tenLoaded = false; tenSlipping = false; tiltDeg = 0;
      faX = restGX; faY = restGY; faZ = restGZ;
      if (fightMode == 1) holdTarget = (uint32_t)tenSegCount * tenSegHold; // 疲労/CATCH%用
      MEL(SFX_HOOKSET); break;
    case CAUGHT: {                                                                 // 胜利号角
      // 釣果はサイズ抽選と一致させる (手応え=表示)。FISH をサイズ順に参照
      static const int SIZE_ORDER[] = { 3, 0, 1, 2 };  // Perch18 < Bass23 < Trout31 < Carp45
      caughtIdx = SIZE_ORDER[(int)(fishSize01 * 3.999f)];
      MEL(SFX_CAUGHT); break;
    }
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

  // 電源モニタ (左上): EXT=外部給電 / BAT=内蔵電池で駆動中 / PWR?=満充電で判別不能
  if (millis() - pwrTick >= 1000) {
    pwrTick = millis();
    pwrChg  = M5.Power.isCharging();
    pwrLvl  = M5.Power.getBatteryLevel();
    int cur = (int)M5.Power.getBatteryCurrent();
    if (cur > -10000 && cur < 10000) pwrCur = cur;   // 非対応(異常値)は捨てる
  }
  bool onExt = pwrChg || pwrCur > 5;                 // 充電中 or 電流流入
  bool onBat = !pwrChg && pwrCur < -20;              // 明確な放電
  const char* tag = onExt ? "EXT" : onBat ? "BAT" : "PWR?";
  snprintf(buf, sizeof(buf), "%s %+dmA %d%%", tag, pwrCur, pwrLvl);
  canvas.setTextColor(onExt ? GREEN : onBat ? ORANGE : YELLOW, curBg);
  canvas.setCursor(2, 2);
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

// -----------------------------------------------------------------------------
// FIGHTING (HOLD モード) の描画: 自責の念バー式の分段進度 + 傾きヒント
// -----------------------------------------------------------------------------
void renderFightTension() {
  curBg = MAROON; canvas.fillScreen(curBg);
  textC("FIGHTING!", W / 2, 4, 2, WHITE);
  int hp = (int)(100.0f * holdTime / holdTarget); if (hp > 100) hp = 100;
  char b[32];
  snprintf(b, sizeof(b), "CATCH %d%%  [%s]", hp,
           fishSize01 > 0.75f ? "BIG" : fishSize01 > 0.4f ? "MID" : "SML");
  textC(b, W / 2, 26, 1, WHITE);

  // 分段バー: 完了=緑 / 現セグメント=充填中 (負荷中は橙・未負荷は暗色) / 残り=黒
  int barX = 8, barW = W - 16, barY = 56, barH = 40, gap = 3;
  canvas.drawRect(barX - 1, barY - 1, barW + 2, barH + 2, WHITE);
  float segW = (barW - gap * (tenSegCount - 1)) / (float)tenSegCount;
  for (int i = 0; i < tenSegCount; i++) {
    int x = barX + (int)(i * (segW + gap));
    int w = (int)segW;
    if (i < tenSegDone)       canvas.fillRect(x, barY, w, barH, GREEN);
    else if (i == tenSegDone) {
      canvas.fillRect(x, barY, w, barH, NAVY);
      int fw = (int)(w * (float)tenSegMs / tenSegHold); if (fw > w) fw = w;
      canvas.fillRect(x, barY, fw, barH, tenLoaded ? ORANGE : OLIVE);
    } else                    canvas.fillRect(x, barY, w, barH, BLACK);
  }
  if (millis() - pumpFlashAt < 120)                       // セグメント完了の白閃
    canvas.drawRect(barX - 3, barY - 3, barW + 6, barH + 6, WHITE);

  bool over = tenLoaded && tiltDeg > TEN_TILT_OVER;
  bool slip = tenLoaded && !over && tenSlipping;
  const char* hint = over ? "TOO HIGH! ease off"
                   : slip ? "DRAG SLIPS! ease off"
                   : tenLoaded ? "HOLD... feel the fish"
                   : "RAISE the rod !";
  textC(hint, W / 2, barY + barH + 8, 1,
        over ? RED : slip ? YELLOW : (tenLoaded ? GREEN : ORANGE));

  if (hapticReady()) {
    snprintf(b, sizeof(b), "A:%d%% T:%dms tilt:%d",
             (int)roundf(hapticPullStrength() * 100), hapticPullInterval(), (int)tiltDeg);
    textC(b, W / 2, barY + barH + 20, 1, SKYBLUE);
  }
  if (tenLoaded) {                                        // 負荷中だけ画面も揺れる
    shakeX = random(-2, 3);
    if (over) { shakeX = random(-4, 5); shakeY = random(-3, 4); }
  }
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
      char mb[28];
      snprintf(mb, sizeof(mb), "mode:%s (BtnB)", fightMode ? "HOLD" : "PUMP");
      textC(mb, W / 2, SUB_Y + 14, 1, fightMode ? ORANGE : GREENYELLOW);
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
      if (fightMode == 1) { renderFightTension(); break; }   // 第2モード (張力保持)
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

      // 触覚刺激の現在値 (振幅% / パルス間隔T) ― 実験時の条件確認用
      if (hapticReady()) {
        snprintf(b, sizeof(b), "A:%d%% T:%dms",
                 (int)roundf(hapticPullStrength() * 100), hapticPullInterval());
        textC(b, W / 2, barY + barH + 20, 1, SKYBLUE);
      }

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
//  張力保持モード (HOLD) のヘルパ / Tension-hold fight mode helpers
// =============================================================================

// 静止時の重力ベクトルを学習する (竿を構えて待っている姿勢が基線になる)。
// 角速度が大きい間は学習しない。FIGHTING 中は凍結。
void updateRestPose() {
  if (!imuOk) return;
  float ax, ay, az, gx, gy, gz;
  M5.Imu.getAccel(&ax, &ay, &az);
  M5.Imu.getGyro(&gx, &gy, &gz);
  if (sqrtf(gx * gx + gy * gy + gz * gz) > 30.0f) return;   // 動作中は学習しない
  if (!restGInit) { restGX = ax; restGY = ay; restGZ = az; restGInit = true; return; }
  restGX += (ax - restGX) * 0.03f;
  restGY += (ay - restGY) * 0.03f;
  restGZ += (az - restGZ) * 0.03f;
}

// FIGHTING (fightMode=1) の1フレーム更新。
//  ・傾き = 現在の重力ベクトルと基線の夾角 (取付方向フリー)
//  ・保持で現セグメントが充填 → 完了で魚が一段寄る。全段で CAUGHT
//  ・竿を戻すと進度が少し戻される + 未負荷が続くと魚が逃げる
//  ・過傾 (強引すぎ, 前田図の×) が続くとラインブレイク
void tensionFightUpdate(uint32_t now, uint32_t dt) {
  float ax = 0, ay = 0, az = 1, gx = 0, gy = 0, gz = 0;
  if (imuOk) { M5.Imu.getAccel(&ax, &ay, &az); M5.Imu.getGyro(&gx, &gy, &gz); }
  gyroMag = sqrtf(gx * gx + gy * gy + gz * gz);
  faX += (ax - faX) * 0.15f;                       // 揺れ除去の低域通過
  faY += (ay - faY) * 0.15f;
  faZ += (az - faZ) * 0.15f;
  float dot = faX * restGX + faY * restGY + faZ * restGZ;
  float n   = sqrtf(faX * faX + faY * faY + faZ * faZ) *
              sqrtf(restGX * restGX + restGY * restGY + restGZ * restGZ) + 1e-6f;
  float c = dot / n; if (c > 1) c = 1; if (c < -1) c = -1;
  tiltDeg = acosf(c) * 57.2958f;

  // 負荷判定 (ヒステリシス)。IMU 不調/卓上試験用に BtnA 長押しでも負荷扱い
  if (!tenLoaded && tiltDeg > TEN_TILT_ON)  tenLoaded = true;
  if ( tenLoaded && tiltDeg < TEN_TILT_OFF) tenLoaded = false;
  if (M5.BtnA.isPressed()) tenLoaded = true;

  uint32_t el = now - stateStart;
  if (tenLoaded) {
    tenUnloadMs = 0;
    // ドラッグ滑り: 滑り角を超えている間は進度が進まず逆に戻る (糸が出て行く)。
    // クリック列と張力ダックは haptics 側 (hapticSetSlip) が重畳する。
    tenSlipping = tiltDeg > tenTiltSlip;
    if (tenSlipping) {
      uint32_t back = (uint32_t)(dt * TEN_SLIP_BACK);
      tenSegMs = (tenSegMs > back) ? tenSegMs - back : 0;
      if (tiltDeg <= TEN_TILT_OVER && now - sfxTick >= 110) {  // 出線のジジジ音
        beep(1250, 26); sfxTick = now;
      }
    } else {
      tenSegMs += dt;
    }
    if (tiltDeg > TEN_TILT_OVER) {                 // 強引すぎ: 警告→ラインブレイク
      tenOverMs += dt;
      if (now - sfxTick >= 150) { beep(2800, 60); sfxTick = now; }
      if (tenOverMs >= TEN_OVER_MS) {
        Serial.printf("    >> LINE BROKE (tilt=%.0f deg)\n", tiltDeg);
        failReason = "Line broke!"; enterState(FAILED); return;
      }
    } else tenOverMs = 0;
    if (!tenSlipping && tenSegMs >= tenSegHold) {  // 1セグメント完了: 魚が一段寄る
      tenSegDone++; tenSegMs = 0;
      // 「グッと寄った」瞬態。大きい魚ほど余振が長い=重い手応え
      hapticTriggerTap(1.0f, 60 + (int)(80.0f * fishSize01));
      beep(900 + 150 * tenSegDone, 60);
      pumpFlashAt = now;
      Serial.printf("    >> SEGMENT %d/%d done\n", tenSegDone, tenSegCount);
      if (tenSegDone >= tenSegCount) { enterState(CAUGHT); return; }
    }
  } else {
    tenOverMs = 0;
    tenSlipping = false;
    tenUnloadMs += dt;
    uint32_t back = (uint32_t)(dt * TEN_SEG_BACK); // 糸が緩み進度が少し戻される
    tenSegMs = (tenSegMs > back) ? tenSegMs - back : 0;
    if (el > 600 && now - sfxTick >= 700) { beep(330, 50); sfxTick = now; }  // 催促音
    if (tenUnloadMs >= TEN_UNLOAD_LIMIT) {
      Serial.printf("    >> FISH RAN OFF (slack %lu ms)\n", (unsigned long)tenUnloadMs);
      failReason = "Ran off!"; enterState(FAILED); return;
    }
  }
  holdTime = (uint32_t)tenSegDone * tenSegHold + tenSegMs;   // 疲労モデル/CATCH%用
}

// =============================================================================
//  状態ロジック / Per-state logic & transitions
// =============================================================================
void updateLogic() {
  uint32_t el = millis() - stateStart;

  // 戦闘前は静止姿勢 (重力基線) を学習しておく → HOLD モードの傾き基準
  if (state == IDLE || state == CASTING || state == WAITING || state == NIBBLE)
    updateRestPose();

  switch (state) {

    case IDLE:
      if (M5.BtnA.wasPressed()) { runCount++; enterState(CASTING); }
      if (M5.BtnB.wasPressed()) {                  // 戦闘モード切替 (PUMP⇔HOLD)
        fightMode ^= 1;
        beep(fightMode ? 1400 : 800, 60);
        Serial.printf("  fight mode = %s\n", fightMode ? "HOLD (tension)" : "PUMP (classic)");
      }
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
      if (el >= nibbleMs) enterState(BITE);
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
      //   突進(強く粗い引き) / 休息(引きが緩む) / 通常 の3態。触覚振幅も
      //   この driftPerSec から導くため、画面と手の感覚が常に一致する。
      if (now >= driftReroll) {
        // 疲労で「行動」を変える: 進むほど突進が稀になり休息が増える(burst-and-coast)。
        // 単発振幅は変えないので、突発的な引きの峰は終盤まで残る。
        float progress = (float)holdTime / holdTarget; if (progress > 1) progress = 1;
        int surgeCh = (int)(SURGE_CHANCE * (1.0f - FATIGUE_SURGE_FADE * progress)); // 10→約3%
        int restCh  = REST_CHANCE + (int)(FATIGUE_REST_GAIN * progress);            // 18→約38%
        float prevDrift = driftPerSec;
        int roll = (int)random(0, 100);
        if      (roll < surgeCh)          driftPerSec = DRIFT_SURGE;  // 突進(終盤は稀)
        else if (roll < surgeCh + restCh) driftPerSec = DRIFT_REST;   // 休息(終盤は頻繁)
        else driftPerSec = random((int)DRIFT_MIN, (int)DRIFT_MAX + 1); // 通常
        driftReroll = now + random(DRIFT_REROLL_MIN, DRIFT_REROLL_MAX + 1);
        // 行動遷移を触覚イベント化 (方案四): サージ突入=瞬態1発,
        // 休息突入=slack(振動骤停→復帰)で「糸がふっと緩む」を伝える
        if (hapIrregular && driftPerSec != prevDrift) {
          if      (driftPerSec == DRIFT_SURGE) hapticTriggerTap(1.0f);
          else if (driftPerSec == DRIFT_REST)  hapticTriggerSlack(random(120, 301));
        }
      }

      // ＝＝ 第2モード (張力保持) はここで分岐。魚行動の再抽選は共通
      //     (driftPerSec が触覚の振幅テクスチャ/イベントを駆動し続ける) ＝＝
      if (fightMode == 1) { tensionFightUpdate(now, dt); break; }

      rodPos -= driftPerSec * dt / 1000.0f;              // マーカーは下へ

      // --- 上提(煽り)検出: 角速度スパイク + ヒステリシス + 不応期 ---
      float gx = 0, gy = 0, gz = 0;
      if (imuOk) M5.Imu.getGyro(&gx, &gy, &gz);
      float gmag = sqrtf(gx * gx + gy * gy + gz * gz);   // [deg/s]
      gyroMag = gmag;                                    // 触覚の運動結合ゲート用
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
//  触覚提示の更新 / Haptic update  (研究計画書 3.2-3.3 + IMPROVEMENT_PROPOSAL)
//   NIBBLE/BITE → アタリ (タップイベント列, 10+23Hz 包絡の AM),
//   FIGHTING    → 非対称矩形波 (引き感) + 運動結合ゲート + slack/サージ瞬態
// =============================================================================
void updateHaptics() {
  float strength;
  int   Tms;

  if (hapIrregular) {
    // ＝＝ 不規則モード: 魚行動×張力の2層モデル ＝＝
    // [魚行動層] driftPerSec (突進52/通常12-30/休息4) → 振幅の土台。
    //   画面のマーカー減衰と同じ値なので視覚と触覚が必ず一致する。
    float s01 = driftPerSec / DRIFT_SURGE;
    if (s01 < 0.12f) s01 = 0.12f;                    // 休息中もライン張力は残る
    if (s01 > 1.0f)  s01 = 1.0f;

    if (state == FIGHTING && fightMode == 1) {
      // ＝＝ HOLD モード: 傾き=張力が門控そのもの (前田案) ＝＝
      // 竿を高く保持するほど張力大 (×0.85–1.15)。未負荷時はモード側で無振動に
      // なるので、運動結合(mc)は適用しない (傾き門控が action-coupling の代替)。
      float t01 = (tiltDeg - TEN_TILT_ON) / (TEN_TILT_FULL - TEN_TILT_ON);
      if (t01 < 0) t01 = 0; if (t01 > 1) t01 = 1;
      float loadFac = 0.85f + 0.30f * t01;
      // 魚サイズ→振幅 (高椋ら2016: 振幅・周波数で魚サイズの印象を変調可能)
      float sizeFac = TEN_AMP_MIN + (1.0f - TEN_AMP_MIN) * fishSize01;
      // 抗適応の漸増: セグメント充填に沿って +18% まで振幅を上げ、
      // 触覚適応 (連続刺激で主観強度が数秒で低下) を補償する
      float adaptFac = 1.0f + TEN_ADAPT_GAIN * ((float)tenSegMs / (float)tenSegHold);
      strength = s01 * loadFac * sizeFac * adaptFac * hapPullScale;
    } else {
      // ＝＝ PUMP モード (旧) ＝＝
      // [張力層1] 竿の姿勢: 竿を立てている(マーカー高)ほど張力大 ×0.85-1.15
      float posFac = 0.85f + 0.30f * (rodPos / 100.0f);

      // [張力層2] 上提の瞬間: pump 直後 400ms は最大+35%の張力スパイク
      uint32_t dtP = millis() - lastPumpAt;
      float pumpFac = (state == FIGHTING && lastPumpAt != 0 && dtP < 400)
                    ? 1.0f + 0.35f * (1.0f - dtP / 400.0f) : 1.0f;

      // [運動結合層] (方案三, CHI 2025): 竿が動いている間だけフル振幅、
      //   静止中は MC_FLOOR まで減衰 → 感覚減衰により"嗡嗡感"を抑えつつ
      //   引かれ感を残す。立ち上がりは即応、戻りは ~300ms でゆっくり。
      static float mcGate = 0;
      float mcFac = 1.0f;
      if (hapMotionCouple && state == FIGHTING) {
        float g01 = gyroMag / MC_GYRO_FULL;
        if (g01 > 1) g01 = 1;
        mcGate += (g01 - mcGate) * (g01 > mcGate ? MC_ATTACK : MC_RELEASE);
        mcFac = MC_FLOOR + (1.0f - MC_FLOOR) * mcGate;
      } else {
        mcGate = 0;
      }

      strength = s01 * posFac * pumpFac * mcFac * hapPullScale;
    }

    // パルス間隔T: 引きが強いほど粗く(22→12ms)。休息中は細かく弱い振動。
    float k = (driftPerSec - DRIFT_MIN) / (DRIFT_SURGE - DRIFT_MIN);
    if (k < 0) k = 0; if (k > 1) k = 1;
    Tms = hapPullTOverride > 0
        ? hapPullTOverride
        : (int)roundf(PULL_T_SLOW - k * (PULL_T_SLOW - PULL_T_FAST));
    // HOLD: 魚サイズ→基本周波数バイアス。大きい魚ほど T 長 (低周波=重い),
    // 小さい魚ほど T 短 (高周波=軽快)。"pt" で T 固定中は適用しない。
    if (state == FIGHTING && fightMode == 1 && hapSizeFreq && hapPullTOverride <= 0)
      Tms += TEN_T_BIAS_SML + (int)roundf((TEN_T_BIAS_BIG - TEN_T_BIAS_SML) * fishSize01);
  } else {
    // ＝＝ 規則モード(対照条件): 高椋ら2016と同じ一定振幅・一定間隔 ＝＝
    strength = 0.7f * hapPullScale;
    Tms = hapPullTOverride > 0 ? hapPullTOverride : PULL_T_SLOW - (PULL_T_SLOW - PULL_T_FAST) / 2;
  }
  hapticSetIrregular(hapIrregular);
  hapticSetPull(strength, Tms);
  // HOLD 中のみ: 引き込み節律 (抗適応) と ドラッグ滑り (クリック列+張力ダック)
  hapticSetTug(hapTug && state == FIGHTING && fightMode == 1);
  hapticSetSlip(state == FIGHTING && fightMode == 1 && tenLoaded && tenSlipping);

  if (hapTestMode >= 0) {                    // シリアルからの強制モード (ベンチ試験)
    hapticSetMode((HapticMode)hapTestMode);
    return;
  }
  switch (state) {                           // ゲーム状態 → 提示モード
    case NIBBLE:   hapticSetMode(HAPTIC_NIBBLE); break;
    case BITE:     hapticSetMode(HAPTIC_BITE);   break;
    case FIGHTING:
      // HOLD モード: 竿を戻している間は"あえて無振動" (糸のテンション無し)。
      // 引かれている(保持中)タイミングだけ振動する — 前田案の核心。
      hapticSetMode((fightMode == 1 && !tenLoaded) ? HAPTIC_OFF : HAPTIC_PULL);
      break;
    case IDLE:
    case CAUGHT:
    case FAILED:
      // 渚: 待機/結果画面の環境触覚 ("竿が水辺にある" 感)。12-18s 毎の
      // スウェルが待機電流も維持し、モバイルバッテリーの自動判停を防ぐ。
      // WAITING 以降は入れない (刺激条件を汚さない)。"wa 0" で無効化。
      hapticSetMode(HAPTIC_WAVE);
      break;
    default:       hapticSetMode(HAPTIC_OFF);    break;
  }
}

// =============================================================================
//  シリアルコマンド / Serial tuning commands (予備評価でのパラメータ探索用)
//    t 0|n|b|p  : 触覚モードを強制 (off/nibble/bite/pull)   t g : ゲーム連動へ戻す
//    pa <0-1>   : 引き振幅スケール      pt <ms> : パルス間隔T固定 (pt 0 で自動)
//    na <0-1>   : アタリ振幅            fc <Hz> : AMキャリア周波数 (共振点スイープ用)
//    tt <ms>    : タップ余振τ           mc 0|1  : 運動結合 on/off
//    fm 0|1     : 戦闘モード 0=PUMP(旧) 1=HOLD(張力保持, 前田案)。IDLE の BtnB でも切替
//    wa <0-1>   : 渚(待機の環境触覚)の振幅。0=無効。既定0.35
//    wi <sec>   : 渚の平均間隔 [秒]。既定45s
//    wk <0-1>   : 底流(無感の保活トーン)振幅。既定0.35   wf <Hz> : 同周波数 (既定28)
//    wd <1-4>   : 底流ソフトクリップ係数 (既定1=純正弦。保活不足時のみ上げる)
//    ht 0|1     : HOLD 引き込み節律 (抗適応) on/off。既定 on。A/B 用
//    hf 0|1     : HOLD 魚サイズ→周波数バイアス on/off。既定 on。A/B 用
//    hd <deg>   : ドラッグ滑り開始角 [deg]。既定48。30-55 にクランプ
// =============================================================================
void handleCommand(char* line) {
  char cmd = line[0];
  char* arg = line[1] ? line + 2 : (char*)"";
  switch (cmd) {
    case 't':
      if (line[1] == 't') {                              // "tt <ms>" タップ余振τ
        hapticSetTapTau(atoi(arg));
        Serial.printf("  tap tau = %d ms\n", hapticTapTau());
        break;
      }
      if      (arg[0] == '0') hapTestMode = HAPTIC_OFF;
      else if (arg[0] == 'n') hapTestMode = HAPTIC_NIBBLE;
      else if (arg[0] == 'b') hapTestMode = HAPTIC_BITE;
      else if (arg[0] == 'p') hapTestMode = HAPTIC_PULL;
      else                    hapTestMode = -1;          // 't g' などはゲーム連動
      Serial.printf("  haptic test mode = %d (-1=game)\n", hapTestMode);
      break;
    case 'f':
      if (line[1] == 'c') {
        hapticSetCarrier(atof(arg));
        Serial.printf("  carrier = %.1f Hz\n", hapticCarrier());
      } else if (line[1] == 'm') {             // "fm 0|1" 戦闘モード切替
        fightMode = (atoi(arg) != 0) ? 1 : 0;
        Serial.printf("  fight mode = %s\n", fightMode ? "HOLD (tension)" : "PUMP (classic)");
      }
      break;
    case 'm':
      if (line[1] == 'c') {
        hapMotionCouple = (atoi(arg) != 0);
        Serial.printf("  motion coupling = %s\n", hapMotionCouple ? "ON" : "OFF");
      }
      break;
    case 'p':
      if (line[1] == 'a') { hapPullScale = atof(arg); Serial.printf("  pull scale = %.2f\n", hapPullScale); }
      if (line[1] == 't') { hapPullTOverride = atoi(arg); Serial.printf("  pull T = %d ms (0=auto)\n", hapPullTOverride); }
      break;
    case 'n':
      if (line[1] == 'a') { hapticSetNibbleAmp(atof(arg)); Serial.printf("  nibble amp = %.2f\n", hapticNibbleAmp()); }
      break;
    case 'w':
      if (line[1] == 'a') {                      // "wa <0-1>" 渚の振幅 (0=無効)
        hapticSetWaveAmp(atof(arg));
        Serial.printf("  wave amp = %.2f%s\n", hapticWaveAmp(),
                      hapticWaveAmp() < 0.001f ? " (off)" : "");
      } else if (line[1] == 'i') {               // "wi <sec>" 渚の平均間隔
        hapticSetWaveInterval(atoi(arg));
        Serial.printf("  wave interval = %d s (+/-20%%)\n", hapticWaveInterval());
      } else if (line[1] == 'k') {               // "wk <0-1>" 底流(保活トーン)振幅
        hapticSetUndercurrent(atof(arg));
        Serial.printf("  undercurrent amp = %.2f%s\n", hapticUndercurrent(),
                      hapticUndercurrent() < 0.001f ? " (off)" : "");
      } else if (line[1] == 'f') {               // "wf <Hz>" 底流の周波数
        hapticSetUndercurrentHz(atof(arg));
        Serial.printf("  undercurrent = %.1f Hz\n", hapticUndercurrentHz());
      } else if (line[1] == 'd') {               // "wd <1-4>" 底流ソフトクリップ係数
        hapticSetUndercurrentDrive(atof(arg));
        Serial.printf("  undercurrent drive = %.1f\n", hapticUndercurrentDrive());
      }
      break;
    case 'h':
      if (line[1] == 't') {                      // "ht 0|1" 引き込み節律 (抗適応)
        hapTug = (atoi(arg) != 0);
        Serial.printf("  hold tug rhythm = %s\n", hapTug ? "ON" : "OFF");
      } else if (line[1] == 'f') {               // "hf 0|1" サイズ→周波数バイアス
        hapSizeFreq = (atoi(arg) != 0);
        Serial.printf("  size->freq bias = %s\n", hapSizeFreq ? "ON" : "OFF");
      } else if (line[1] == 'd') {               // "hd <deg>" 滑り開始角
        float d = atof(arg);
        if (d < 30) d = 30; if (d > 55) d = 55;
        tenTiltSlip = d;
        Serial.printf("  drag slip tilt = %.0f deg\n", tenTiltSlip);
      }
      break;
    case 'i':
      if (line[1] == 'r') {
        hapIrregular = (atoi(arg) != 0);
        Serial.printf("  irregularity = %s\n", hapIrregular
                      ? "ON (improved: tap events + AM + fish model)"
                      : "OFF (literature baseline: Saiki/Takamuku-style)");
      }
      break;
    default:
      Serial.println("  cmds: t 0|n|b|p|g / pa <0-1> / pt <ms> / na <0-1> / ir 0|1"
                     " / fc <Hz> / tt <ms> / mc 0|1 / fm 0|1 / wa <0-1>"
                     " / ht 0|1 / hf 0|1 / hd <deg>");
  }
}

void pollSerial() {
  static char line[24]; static int len = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') { line[len] = 0; if (len) handleCommand(line); len = 0; }
    else if (len < (int)sizeof(line) - 1) line[len++] = c;
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

  M5.Display.setRotation(0);                // 逆时针 90° (旧: 1)
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

  // I2S 触覚出力 (MAX98357A)。配線が無くても失敗せず、デモは継続する。
  bool hapOk = hapticInit();

  Serial.println("=== Fishing Experience Simulator + Haptics ===");
  Serial.printf("Display %dx%d  sprite=%s  IMU=%s  SPK=%s  I2S=%s\n",
                W, H, useSprite ? "on" : "off",
                imuOk ? "ok" : "N/A", speakerOk ? "ok" : "N/A",
                hapOk ? "ok" : "N/A");
  Serial.println("serial cmds: t 0|n|b|p|g / pa <0-1> / pt <ms> / na <0-1> / ir 0|1"
                 " / fc <Hz> / tt <ms> / mc 0|1 / fm 0|1 / wa <0-1>"
                 " / ht 0|1 / hf 0|1 / hd <deg>");

  enterState(IDLE);
}

void loop() {
  M5.update();
  pollSerial();                             // 触覚チューニングコマンド
  updateLogic();
  updateHaptics();                          // 状態→触覚刺激のマッピング
  updateMelody();                           // ジングルを1ノートずつ進める
  renderScene();
  present();
  delay(16);                                // ~60fps
}



