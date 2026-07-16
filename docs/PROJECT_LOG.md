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
└── PROJECT_LOG.md    本文档
```

### 触觉刺激设计（依据先行研究）

- **アタリ感**（才木ら 2016）：10Hz + 23Hz 合成正弦波短 burst。
  NIBBLE = 弱、随机间隔；BITE = 连续、更强。
- **引き感**（高椋ら 2016）：5ms 高电压 + T ms 低电压的非对称矩形波产生牵引力错觉。
  振幅 = 拉力强度，脉冲间隔 T = 粗糙度（12ms 以上真实感较高）。
- 与游戏联动：FIGHTING 中鱼的拉力 `driftPerSec` 越大 → 振幅越大（0.45→1.0）、T 越短（22→12ms）。
- 不规则性开关：on = ±2ms 抖动 + 低频起伏（模拟鱼挣扎）；off = 严格周期刺激（实验对照条件）。

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
