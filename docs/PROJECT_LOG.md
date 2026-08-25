# 项目说明与改动日志 (Project Log)

> 本文档用于说明项目整体结构，并按时间记录每次重要改动。
> 新改动请在「改动日志」最上方追加条目（倒序，最新在前）。

---

## 项目说明

**疑似釣り体験プロトタイプ (Fishing Haptic Prototype)** — JAIST 木谷研究室的研究项目。
用「延べ竿型・非接地・小型振動装置」切换提示钓鱼中"アタリ（咬钩）"和"引き（拉拽）"两种触觉的 VR 钓具原型。

### 硬件构成

| 部件 | 型号 | 作用 |
|------|------|------|
| 主控 | M5StickS3 (ESP32-S3-PICO) | 波形生成、I2S 输出、IMU、LCD |
| 数字功放 | MAX98357A | I2S → 模拟 + Class-D 放大 |
| 振动子 | Dayton EX25FHE2-4 (4Ω/24W) | 音圈式加振器 (exciter) |
| 电源 | Anker PowerCore 5000 | 5V 供电 |

接线：GPIO4→BCLK、GPIO5→LRC、GPIO6→DIN（详见 CLAUDE.md）。

### 软件结构

```
src/
├── main.cpp      8 状态钓鱼模拟器（游戏逻辑、LCD 绘制、IMU 检测、
│                 蜂鸣器音效、串口调参命令），并调用触觉模块联动振动
└── haptics.cpp/h 触觉提示模块：I2S(16kHz) → MAX98357A → 加振器
docs/
├── HANDOFF.md        交接文档
├── HAPTICS_GUIDE.md  触觉模块使用指南
├── PROJECT_LOG.md    本文档
└── changes/          改动说明文档(每次重要改动一篇,文件名 日期-主题.md)
```

### 触觉刺激设计（依据先行研究）

- **アタリ感**（才木ら 2016 + IMPROVEMENT_PROPOSAL 方案一/二）：敲击事件 =
  3ms 冲击 + 指数衰减余振，余振为 10+23Hz 包络 AM 调制 ~60Hz 载波
  （绕过功放高通/激振器共振）。NIBBLE = 稀疏单敲；BITE = 密集连敲渐强。
- **引き感**（高椋ら 2016 + 方案三/四）：5ms 高电压 + T ms 低电压的非对称矩形波
  产生牵引力错觉（T 以 40Hz 周期为中心），加陀螺仪运动耦合门控（静止时降到
  35% 底噪）与事件层（突进瞬态、休息 slack 骤停）。
- 与游戏联动：FIGHTING 中鱼的拉力 `driftPerSec` 越大 → 振幅越大（0.45→1.0）、T 越短（26→16ms）。
- 不规则性开关 `ir`：on = 上述改良设计（默认）；off = 文献准拠基线（实验对照条件）。

### 游戏状态机（main.cpp，8 状态）

IDLE → CASTING → WAITING → NIBBLE →（BITE 窗口内合わせ）→ FIGHTING → CAUGHT / FAILED → IDLE

- NIBBLE/BITE：アタリ振动；IMU 检测 |a|>1.5g 的合わせ动作。
- FIGHTING：泵竿（gyro 尖峰检测上提）把标记保持在 sweet zone，累计维持 12–18 秒即捕获。
  含鱼的疲劳模型（burst-and-coast，Bainbridge 1958）：进度越高突进越少、休息越多。

### 开发/烧录

- PlatformIO + Arduino framework，board = `m5stack-stamps3`，库 = M5Unified。
- 烧录：`pio run -t upload`（COM3）。串口监视 115200bps。

### 串口调参命令（115200bps，预备评估用）

| 命令 | 作用 |
|------|------|
| `t 0\|n\|b\|p` | 强制触觉模式 off/nibble/bite/pull；`t g` 回到游戏联动 |
| `pa <0-1>` | 引き振幅整体缩放 |
| `pt <ms>` | 固定脉冲间隔 T（`pt 0` = 自动联动 12–22ms） |
| `na <0-1>` | アタリ振幅 |
| `ir 0\|1` | 不规则性 off/on |

---

## 改动日志

### 2026-08-26 — HOLD 模式 P0 改进:泄力打滑・抗适应节律・尺寸双映射(分支 hold-mode-v3)

- 基于文献调研(新增 `docs/先行研究_HOLD模式.md`)对 HOLD 张力保持模式做三项体验改进:
  - **泄力打滑 (drag slip)**:倾角 >48°(`hd` 可调)进入打滑——"嗒嗒嗒"click train
    (55–90ms 间隔,波形级)+ 基础张力回落 55% + 段进度以 0.6× 倒退 + 1250Hz 出线音。
    断线(>55° 保持 1.2s,规则不变)从突然惩罚变为有触觉前兆的可挽回状态。
  - **抗适应**:负荷中 PULL 分节为"拉入 250–400ms + 微歇 90–140ms(降至 42%)"
    的引き込み节律(`ht`,防触觉适应导致越拉越麻)+ 段内振幅随充填渐增 +18%。
  - **尺寸→频率+振幅双映射**:大鱼 T+5ms(~29Hz,低频=重),小鱼 T−3ms(~55Hz);
    段完成瞬态的余振 τ 也按尺寸 60–140ms 缩放(`hf` 开关,依据 30Hz=重的重量错觉文献)。
- `hapticTriggerTap` 增加可选 τ 参数;新增 `hapticSetTug`/`hapticSetSlip` 接口。
- 新串口命令 `ht 0|1` / `hf 0|1` / `hd <deg>`,均只作用于 HOLD;HOLD 画面新增
  DRAG SLIPS 黄色警示。三项均可独立开关,供 A/B 评价。
- 已编译烧录至 COM3(RAM 7.0% / Flash 15.1%)。
- 详见 `docs/changes/2026-08-26-HOLD改进P0实装.md`。

### 2026-07-29 — 「渚」待机环境触觉(兼充电宝保活)+ 电源监视器

- 实测确认采购的充电宝(ELECOM DE-M04L-3200)在 M5 内置电池充满后因低负载
  自动断电;断电后游戏照跑但功放失电、振动消失。
- 对策 1「渚 + 底流」:IDLE/CAUGHT/FAILED 中每 45s±20% 一次"浪拍岸"涌动
  (sin² 主峰 + 回浪,60Hz 载波,振幅 0.35)纯做待机临场感(依据电通大
  坂本研水触感研究的"柔和舒适"方向);浪间由 28Hz"底流"连续保活——低于
  共振点故体感≈0,但电流照常流,判停窗口多短都不怕。氛围与保活解耦。
  WAITING 之后全程不出现,不污染刺激。`wa/wi/wk/wf` 四个命令在线调参。
- 对策 2 电源监视:屏幕左上角每秒显示 EXT(绿)/PWR?(黄)/BAT(橙)+
  电池电流与残量(BQ27220),BAT=外部电源已断。
- 详见 `docs/changes/2026-07-29-渚待机环境触觉与电源监视.md`。

### 2026-07-26 (2) — FIGHTING 第二模式「张力保持 (HOLD)」（前田 07-23 Slack 提案）

- 新战斗模式:竿倾角(与静止基线的夹角,安装朝向无关)保持 +18° 以上时
  为"负荷中"——振动=鱼的抵抗(倾角越高越强),保持满 2.5–4s 完成一段,
  分段横条(自責の念式)推满即捕获;**竿放回时刻意无振动**。
- 鱼尺寸每局随机(0..1)→ 振幅 0.55–1.0×、段数 3–5、CAUGHT 鱼种一致。
- 新失败:未负荷连续 6s = 跑鱼;倾角 >55° 保持 1.2s = 断线("Line broke!")。
- 旧 PUMP 模式保留且仍为默认;IDLE 按 BtnB 或串口 `fm 0|1` 切换。
- 详见 `docs/changes/2026-07-26-张力保持战斗模式.md`。

### 2026-07-26 — 按 IMPROVEMENT_PROPOSAL 落地触觉改良（方案一/二/三 + 方案四部分）

- **方案一 AM 载波**（haptics.cpp）：アタリ低频（10/23Hz）不再直接输出（会被
  MAX98357A ~14Hz 高通和激振器共振 Fs≈50–60Hz 吃掉），改为作**包络**调幅
  共振点附近的载波（默认 60Hz）。新串口命令 `fc <Hz>` 供实机扫频定共振点。
- **方案二 アタリ瞬态化**（haptics.cpp）：NIBBLE/BITE 从稳态正弦 burst 改为
  **敲击事件**（3ms 冲击 + e^(−t/τ) AM 余振）。NIBBLE=稀疏单敲(30%二连)，
  BITE=70–140ms 密集连敲+渐强。`tt <ms>` 调余振 τ（默认 120ms）。
- **方案三 运动耦合**（main.cpp）：FIGHTING 引き振幅乘以陀螺仪门控
  （静止 35% 底噪 ↔ 竿动全振幅，攻快释慢），复现 CHI 2025 的
  motion-coupled pseudo force；`mc 0|1` 开关。T 范围从 12–22ms 改为
  16–26ms，以牵引错觉最优的 40Hz（T=20ms）为中心（Tanaka ら 2020）。
- **方案四（部分）事件层**：突进 onset 叠加一次瞬态敲击
  （`hapticTriggerTap`）；休息 onset 振动骤停 120–300ms 再恢复
  （`hapticTriggerSlack`，line-slack 线索）。
- **对照条件**：`ir 0` 现在整体回退到文献准拠基线（才木ら原版正弦 burst +
  高椋ら恒定周期刺激，无事件层），供"基线 vs 改良"组内比较。
- 文档：HAPTICS_GUIDE.md 同步更新（映射表、新命令 4.6–4.8、波形规格）；
  详细说明见 `docs/changes/2026-07-26-触觉改良实装.md`（changes/ 文件夹新建，
  此后每次重要改动在其中放一篇说明）。

### 2026-07-16 — 文献调研 + 真实感改进方案书（docs/IMPROVEMENT_PROPOSAL.md 新建）

- 调研牵引力错觉（CHI 2025 motion-coupled、Sensors 2020 40Hz 最优）、真实竿振动实测（NAIST 鱼种判别等）、包络调制渲染、音触一致性等文献。
- 提出 5 条改进方案：① AM 载波绕过 14Hz 高通/加振器共振限制 ② アタリ瞬态化（敲击+竿体余振）③ 运动耦合非对称振动引き感 ④ FIGHTING 双层渲染（张力纹理+事件词汇表）⑤ 音触同步+IMU 实录校准。
- 新增 `docs/先行研究.md`：整理该领域主要研究团队（雨宮研/NTT、sensint@MPI-INF、Kuchenbecker@MPI-IS、Culbertson@USC、Choi@POSTECH、梶本研等）及其与五条方案的对应关系。
- 新增 `docs/雨宫.md`：调研雨宮智浩（東大）全部研究（178 篇），筛出可应用于本设备的技术并分 A/B/C 三类汇总；发现"弱侧低于知觉阈值"定标准则、重量感错觉（ToH 2008）等可直接落地项。
- 仅文档，未改代码。

### 2026-07-08 — 游戏手感调参 + 触觉联动完成（main.cpp）

- **引き感与游戏联动**：FIGHTING 中按 `driftPerSec` 映射振幅（PULL_STR 0.45–1.0）和脉冲间隔（PULL_T 22–12ms），突进时更强更粗糙。
- **战斗时长拉长**：HOLD_TARGET 3–5 秒 → **12–18 秒**（对标 Sea of Thieves ~45s 与 Stardew Valley ~10-20s 之间，全程体感 20–30 秒）；配套 ESCAPE_LIMIT 2.2s→3s、PUMP_RISE 16→20。
- **鱼的疲劳模型**（burst-and-coast，Bainbridge 1958）：新增 FATIGUE_SURGE_FADE / FATIGUE_REST_GAIN，进度越高突进概率越低、休息概率越高，触觉随之自然变弱变稀疏。两常数设 0 可关闭（实验变量切分用）。
- **NIBBLE 随机化**：固定 1500ms → 随机 3500–6000ms（保证体感 ≥4 个 burst，咬钩时机不可预测）。
- **新增休息状态**：DRIFT_REST 4.0/s、REST_CHANCE 18%。
- FIGHTING 画面实时显示当前触觉振幅% 和 T 值（实验条件确认用）。
- 新增 `docs/HANDOFF.md` 交接文档。

### 2026-07-07 — 触觉模块实装（src/haptics.cpp/h 新建）

- I2S(16kHz) → MAX98357A 波形生成模块，独立 FreeRTOS 任务，模式切换无爆音。
- 4 模式：OFF / NIBBLE / BITE / PULL。
- アタリ波形：10Hz+23Hz 合成正弦（才木ら 2016），burst + 随机间隔。
- 引き波形：5ms 高 + T ms 低非对称矩形波（高椋ら 2016），DC-free。
- 不规则性开关（`ir`）：±2ms 抖动 + 低频起伏 vs 严格周期（对照条件）。
- 串口调参命令 `t / pa / pt / na / ir`。
- 新增 `docs/HAPTICS_GUIDE.md`；platformio.ini 解除 M5Unified 版本锁（^0.2.2 → 最新）。

### 2026-05-31 — 初始提交 (02b9e63)

- 单机 demo：8 状态钓鱼模拟器（LCD + IMU + 蜂鸣器），运行于单台 M5StickS3。
- BtnA 施放 → 等待 → 咬钩 → 合わせ → 泵竿战斗 → 捕获/失败 的完整游戏循环。

---

## 待办 (TODO)

- ⬜ 实机验证加振器振动，振幅/脉冲间隔调参（预备评估）
- ⬜ LCD 偏暗问题：确认 M5.Display 初始化与 setBrightness
