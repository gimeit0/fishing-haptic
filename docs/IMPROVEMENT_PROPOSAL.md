# 触觉真实感改进方案书（基于相关文献调研）

> 2026-07-16 起草。目标：针对"当前振动阶段真实感不强"的问题，从近年相关论文中
> 提炼 5 条最有价值的改进方案。**本文档只提方案，不含代码改动。**
> 每条方案附：依据文献 / 具体做法 / 预期效果 / 实现代价。

---

## 现状诊断（为什么真实感不强）

结合硬件链路和文献，当前实现有三个结构性短板：

1. **低频被硬件链路吃掉了**。アタリ波形的 10Hz、23Hz 成分低于/接近
   MAX98357A 的 ~14Hz 内部高通滤波，也远低于 EX25FHE2-4 加振器的
   共振频率（音圈 exciter 一般 Fs≈50–60Hz，低于 Fs 输出急剧衰减）。
   实际到手上的振动很可能只剩微弱的失真残留——**"没感觉/不真实"的首要嫌疑**。
2. **波形是稳态周期信号，而真实アタリ是瞬态事件**。实测研究表明，鱼咬钩
   在竿上产生的是"敲击瞬态 + 竿体固有频率的衰减余振"，而不是连续正弦。
   实际钓竿的振动敏感频段实测集中在 13–16 / 36–43 / 73–81 / 96–111Hz
   等离散共振带（专利实测数据，另见 NAIST 竿振动鱼种判别研究）。
3. **引き感的连续非对称振动自带"嗡嗡"副作用**。CHI 2025 的研究指出，
   连续非对称振动诱发牵引力错觉的同时，不需要的振动感（buzz）本身会
   破坏真实感——这正是"感觉在震，不觉得在拉"的原因。

---

## 方案一（优先级最高）：AM 载波绕过硬件高通，让 10/23Hz 真正"到手"

- **依据**：振动触觉研究表明，人对振动的感知主要由**包络（envelope）**
  而非载波精细结构决定；用高频载波调幅出低频包络，可在执行器
  有效频段外再现低频感（envelope-modulation 渲染方法，
  Mechanical Systems and Signal Processing 2024；bioRxiv 2025
  音乐触觉转换研究同样结论：fast temporal envelope 主导感知）。
- **做法**：アタリ波形改为 `载波 sin(2π·fc·t) × 包络[10Hz+23Hz 合成]`，
  载波 fc 取加振器共振点附近（先实测 EX25FHE2-4 在竿上的频响，预计
  50–80Hz）。串口命令增加 `fc <Hz>` 便于扫频调参。
- **预期效果**：等效输入功率下体感振幅大幅提升，且保留才木ら2016 的
  10/23Hz 特征节奏。这是所有其他方案的**前置条件**——低频出不来，
  怎么调波形都白搭。
- **代价**：小。仅改 haptics.cpp 波形合成一处 + 一次实测扫频（手感或
  用第二台 M5 的 IMU 贴竿测加速度）。

## 方案二：アタリ瞬态化——"敲击 + 竿体余振"事件模型取代稳态正弦

- **依据**：USF 的钓鱼触觉训练研究指出鱼咬钩前会先"tap"饵，竿上表现为
  一串离散小敲击；NAIST 用竿上加速度数据做鱼种判别，说明真实アタリ是
  有信息量的瞬态波形而非稳态音；数据驱动触觉（Culbertson 等）反复证明
  **录制/仿真的瞬态信号显著比合成稳态信号真实**。
- **做法**：把 NIBBLE/BITE 从"连续 burst"改为**事件序列**：
  每个事件 = 短促冲击（2–5ms 宽脉冲）+ 指数衰减正弦余振
  `e^(-t/τ)·sin(2π·f_rod·t)`，f_rod 取竿共振带（实测，预计 15–40Hz 区间
  → 结合方案一用 AM 包络实现），τ≈50–150ms。NIBBLE = 稀疏单敲
  （"コツッ"），BITE = 密集连敲 + 幅度渐强（"ココココッ"）。
- **预期效果**：从"手机来电般的嗡嗡"变成"竿尖被点了一下"的物理事件感，
  是对"アタリ不像アタリ"最直接的对症下药。
- **代价**：中。波形生成器加一个事件包络状态机；参数（τ、f_rod、事件
  间隔分布）需实机调参，可复用现有串口命令框架。

## 方案三：引き感升级为运动耦合非对称振动（motion-coupled pseudo force）

- **依据**：CHI 2025《Motion-Coupled Asymmetric Vibration for Pseudo
  Force Rendering in VR》：把非对称振动**与用户自身运动耦合**（只在
  用户动作时按运动相位发放脉冲），利用感觉衰减（sensory attenuation）
  机制，在保留力错觉的同时把"嗡嗡感"降低约等效 30% 振幅，用户在
  射箭/举重等场景显著更偏好该方式（开源实现在 GitHub sensint）。
  另有 Sensors 2020 系统研究表明非对称振动牵引力错觉在 **40Hz 附近最优**
  ——现行 5ms+17ms（≈45Hz）已接近，但值得以 40Hz 为中心重新扫参。
- **做法**：FIGHTING 中用 BMI270 陀螺仪相位门控 PULL 脉冲：
  用户上提泵竿（已有 gyro 尖峰检测）时同步发放高幅非对称脉冲串，
  静止段回落到低幅"张力底噪"；同时把 `pt` 扫参范围以 40Hz（T≈20ms）
  为中心细化。注意非对称脉冲的极性方向要与"鱼往下拉"一致（错觉有
  方向性，Amemiya & Gomi 2014）。
- **预期效果**："震"变"拉"。这是文献里当前最先进的免接地牵引力
  渲染方式，直接命中"感觉在振动、不觉得有鱼在拽"的痛点。
- **代价**：中。IMU 数据已在采，主要是门控逻辑 + 相位对齐调参。

## 方案四：FIGHTING 双层渲染——连续"张力纹理" + 离散"事件词汇表"

- **依据**：Stanford CHARM 的触觉钓鱼 demo 经验："鱼活着"的感觉来自
  行为库——出线、挣扎、猛冲、渐疲；SIGGRAPH Asia 2024《Interactive
  Virtual Fishing》证明**突然的力消失（line slack）**是极强的真实感线索；
  现有 burst-and-coast 疲劳模型已是好底子，缺的是触觉层的事件多样性。
- **做法**：把 PULL 渲染拆成两层：
  - **底层张力纹理**：随 `driftPerSec` 缩放的连续低幅随机纹理
    （1/f 起伏，替代现行固定 ±2ms 抖动），表达"线绷着"的持续负载感；
  - **事件层**（叠加在底层上）：
    - *甩头 (head shake)*：2–4Hz 包络的强 AM 脉冲串，2–3 个一组；
    - *突进 onset*：一次大幅瞬态 + 幅度阶跃（配合现有 surge 逻辑）；
    - *脱力 (slack)*：振动**骤停 100–300ms** 再恢复——廉价但文献证明
      极有效的"鱼还在/差点跑了"信号；
    - *出线*：快速棘轮式 click 序列（模拟卸力声/感）。
  - 事件由现有疲劳模型驱动、随机文法组合，保证每条鱼手感不同。
- **预期效果**：战斗从"单调的周期振动 12–18 秒"变成有叙事起伏的
  博弈过程，主观真实感和重玩性同时提升。
- **代价**：中偏高。但事件都是波形层小积木，可逐个加、逐个 A/B。

## 方案五：音触一致 + IMU 实录数据校准（低成本大回报的"外挂"）

- **依据**：多感官一致性研究表明，与振动同步且语义一致的声音显著提升
  临场感与真实感（audio-haptic congruence；Woojer 触觉背带 VR 试验等）；
  数据驱动触觉的通用结论：**用真实录制信号（或据其校准的合成参数）
  比纯手调合成更真实**。
- **做法**：
  1. **音触同步**：蜂鸣器音效与振动事件严格同帧触发（甩头配水花声、
     出线配卸力"咔咔"声）；加振器本身在 >100Hz 也可发声，可让波形
     高频成分兼作音效。
  2. **实录校准**：把 M5StickS3 绑在真钓竿上（或请研究室钓鱼经验者
     模拟敲竿/拉线），用 BMI270 录加速度样本，提取真实アタリ/引き的
     包络与频谱，反过来校准方案一/二的 fc、τ、事件间隔——正好衔接
     TODO 里的"预备评估"，还能作为论文里的设计依据写进方法章节。
- **预期效果**：几乎不动渲染架构，把"合成参数拍脑袋"变成"有实测依据"，
  对研究报告的说服力也有直接贡献。
- **代价**：小～中。录制脚本 + 一次数据采集会。

---

## 建议实施顺序

| 顺序 | 方案 | 理由 |
|------|------|------|
| 1 | 方案一 AM 载波 | 根因修复，其余方案的前置 |
| 2 | 方案二 アタリ瞬态化 | 对"真实感不强"最对症，代价可控 |
| 3 | 方案三 运动耦合引き感 | 效果上限最高，依赖 IMU 门控调参 |
| 4 | 方案五 音触同步+实录校准 | 可与 2/3 并行，服务预备评估 |
| 5 | 方案四 事件词汇表 | 锦上添花，逐个事件增量添加 |

## 主要参考文献

- 才木ら 2016（アタリ 10/23Hz）、高椋ら 2016（非对称矩形波）——项目既有依据
- Tanabe ら, [非対称振動刺激による牽引力錯覚の研究動向](https://www.jstage.jst.go.jp/article/tvrsj/25/4/25_291/_article/-char/ja/), 日本VR学会論文誌 25(4), 2020
- [Motion-Coupled Asymmetric Vibration for Pseudo Force Rendering in VR](https://doi.org/10.1145/3706598.3713358), CHI 2025（[开源代码](https://github.com/sensint/Motion-Coupled-Asymmetric-Vibration)）
- Tanaka ら, [Effects of Asymmetric Vibration Frequency on Pulling Illusions](https://doi.org/10.3390/s20247086), Sensors 20(24), 2020（40Hz 最优）
- Culbertson ら, [WAVES: Wearable Asymmetric Vibration Excitation System](https://dl.acm.org/doi/10.1145/3025453.3025741), CHI 2017
- [Interactive Virtual Fishing with Rendering Continuous Reel Weight and Line Slack](https://dl.acm.org/doi/full/10.1145/3681755.3688950), SIGGRAPH Asia 2024 E-Tech
- 福田ら, [釣竿の振動データに基づく魚種判別手法](https://ubi-naist.github.io/paper/IPSJ/202011_DPSWS_fukuda_SmartFishing.pdf), IPSJ DPSWS 2020
- [竿先振動を利用した魚釣り支援システム](https://www.jstage.jst.go.jp/article/ieejeiss/136/7/136_1033/_article/-char/ja/), 電気学会論文誌C 136(7), 2016
- Lin, [A Haptic Interface for Fishing Training](http://reedlab.eng.usf.edu/photos/HapticsDemos2011/2011Lin.pdf), USF 2011
- [Stanford CHARM Lab Fishing Simulator](https://charm.stanford.edu/ME327/2024-Group14), ME327 2024
- [A patterned vibrotactile method using envelope modulation with high resolution and low perceptual frequency](https://www.sciencedirect.com/science/article/abs/pii/S0888327024003704), MSSP 2024
- [Impact of an audio-haptic strap to augment immersion in VR video gaming](https://dl.acm.org/doi/fullHtml/10.1145/3616195.3616202), AudioMostly 2023
- 服部ら, [視覚刺激を用いた牽引力錯覚の知覚変化を提示するVRシステム](https://www.interaction-ipsj.org/proceedings/2026/data/pdf/2B50.pdf), INTERACTION 2026（VR钓鱼投掷+牵引力错觉）
