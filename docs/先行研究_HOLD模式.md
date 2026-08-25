# HOLD 模式体验提升:文献调研与改进方向

> 2026-08-26 整理,分支 `hold-mode-v3`。
> 配合 `先行研究.md`(总览)与 `changes/2026-07-26-张力保持战斗模式.md`(HOLD 实装说明)使用。
> 目的:针对张力保持(HOLD)战斗模式特有的体验问题,梳理**新增**的可参考文献与研究室
> (与 `先行研究.md` 已列条目尽量不重复),并给出按落地成本排序的改进方向。

---

## 一、HOLD 模式的体验短板(问题驱动)

HOLD 的核心交互是**持续负荷**:竿保持倾斜 2.5–4s/段 × 3–5 段,负荷中连续 PULL 振动。
这带来几个 PUMP 模式没有的感知问题:

| # | 短板 | 感知机理 |
|---|------|----------|
| W1 | 连续振动几秒后**感觉变弱、发麻** | 振动触觉适应 (vibrotactile adaptation):同频持续刺激使对应机械感受器通道疲劳,检测阈值上升 |
| W2 | 用力拉住时感觉**进一步变钝** | 运动/用力门控 (tactile gating):主动肌肉收缩和运动会抑制皮肤振动感知,快于 ~25°/s 的动作抑制明显 |
| W3 | 鱼大小/张力只靠**振幅单维**传达 | 振幅 Weber 比 ~10–30%,3–5 档尺寸接近分辨极限;且振幅≠"重量感" |
| W4 | 无**方向**信息(鱼往哪边跑) | 单执行器无法渲染空间线索 |
| W5 | 过拉只有声音+画面警告,触觉上缺**"泄力打滑"**这一真实钓鱼的标志性反馈 | — |
| W6 | 持续张力的"纹理"单薄,只有振幅起伏 | — |

---

## 二、改进方向(每条:做法 → 文献依据 → 本硬件上的落地)

### 改进 1:抗适应包络(纯软件,优先)— 对应 W1

- **做法**:负荷中的 PULL 波形不要恒定输出——
  (a) 段内振幅缓升(如 2.5s 内 0.85→1.15,抵消适应带来的主观衰减);
  (b) 间歇化:250–400ms 的"拉紧脉冲群"+ 100ms 微歇,模拟鱼的发力节律,同时让感受器恢复;
  (c) **频段交替**:低频段 (20–89Hz, RA I 通道) 与高频段 (200Hz+, Pacinian 通道) 交替,
  避免单通道疲劳(适应是频率特异的,只影响同频段)。
- **依据**:振动触觉适应为频率特异、通道特异([biorxiv 2024 综述部分](https://www.biorxiv.org/content/10.1101/2024.12.11.627964.full.pdf));
  20–89Hz 段可部分绕开快适应通道。
- **落地**:`haptics.cpp` 的 PULL 包络生成处加段内斜坡 + 节律门控即可,无硬件改动。

### 改进 2:用力门控补偿 — 对应 W2

- **做法**:负荷越深(倾角越大 = 用力越大),振幅在张力系数之上再乘一个**门控补偿系数**
  (如 1.0→1.3);且信息性内容(段完成、突进)尽量用**瞬态敲击**而非连续音传达——
  瞬态比稳态更抗门控。
- **依据**:主动肌肉收缩期间皮肤振动感知被系统性抑制
  ([BMC Neuroscience 2020](https://link.springer.com/article/10.1186/s12868-020-00592-2)、
  [Exp Brain Res 2009](https://link.springer.com/article/10.1007/s00221-009-2050-8)、
  [Scholarpedia: Tactile suppression](http://www.scholarpedia.org/article/Tactile_suppression))。
- **落地**:main.cpp 已有 tilt→张力系数管线,再乘一条 tilt→补偿曲线;一行改动可 A/B。

### 改进 3:鱼大小 = 频率 + 振幅**双映射**(重量感)— 对应 W3

- **做法**:大鱼不只振幅大,**载波频率也向 30Hz 低频段下移**;小鱼上移 (~80–120Hz)。
  低频大振幅在手持物体上会系统性地增加**主观重量**和握力输出——正是"大鱼手感"。
- **依据**:30Hz 刺激最"重"、最能诱发握力增加
  ([Frontiers in VR 2022: Weight illusion by presenting vibration to the fingertip](https://www.frontiersin.org/journals/virtual-reality/articles/10.3389/frvir.2022.797993/full));
  已引的 CHI 2024 动态重量 + [DualVib (VRST 2020)](https://dl.acm.org/doi/10.1145/3385956.3418964)
  证明"伪力 + 纹理"双层叠加能渲染动态质量感。
- **落地**:`fc` 命令已能扫载波频率;把 `fishSize01` 同时映射到 fc 与振幅即可。
  注意与共振点(执行器灵敏度)交互,需实机标定等响度曲线。

### 改进 4:泄力打滑 (drag slip) 事件 — 对应 W5、W6

- **做法**:倾角进入 45–55° 危险区时,先不断线,而是触发**"嗒嗒嗒" click train**
  (泄力器出线声的触觉版,每 click 复用 `hapticTriggerTap`)+ 当前段进度倒退 +
  出线音效。把"断线"从突然惩罚变成**有触觉前兆的可挽回状态**,同时天然成为
  过拉警告的触觉通道。
- **依据**:瞬态事件序列是数据驱动真实感的核心手法(Kuchenbecker 事件触觉一系,已在
  `先行研究.md`);[SIGGRAPH Asia 2024 虚拟钓鱼](https://dl.acm.org/doi/full/10.1145/3681755.3688950)
  同样把 reel 出线作为核心反馈通道;音触一致(Choi)提升可信度。
- **落地**:纯软件,复用现有 tap 接口 + 一个状态机分支。**性价比最高的一条**。

### 改进 5:屏幕伪触觉(视觉增强抵抗感)— 对应 W3、W6

- **做法**:LCD 上画一根**弯曲的竿 + 张力条**,其响应对倾角故意做非线性/滞后映射
  (拉得越狠,画面上竿弯曲的增益越低)——视觉 C/D 比操控可让同一振动"感觉更重/更硬"。
- **依据**:C/D 比 <1 使物体感觉更重
  ([Samad et al., CHI 2019](https://dl.acm.org/doi/fullHtml/10.1145/3290605.3300550));
  不可察觉的伪触觉可渲染旋钮阻力
  ([Turn-It-Up, CHI 2023](https://www.researchgate.net/publication/375062708_Turn-It-Up_Rendering_Resistance_for_Knobs_in_Virtual_Reality_through_Undetectable_Pseudo-Haptics));
  [Pseudo-stiffness, CHI 2023](https://dl.acm.org/doi/10.1145/3544548.3581223)。
- **落地**:纯软件(drawFighting 改绘制),与触觉改动正交,适合独立 A/B。

### 改进 6:腱振动(小硬件扩展)— 对应 W2、W3

- **做法**:腕带上加第二个振子,压在**手腕屈肌腱**上,负荷中施加 ~70–110Hz 振动。
  腱振动诱发肌梭错觉,在**主动用力时**会让真实重量感觉更重、抵抗感觉更大——
  与 HOLD"鱼在持续拉你"完全同构。
- **依据**:**梶本研(電通大)牛山ら, EuroHaptics 2022**:
  [Increasing Perceived Weight and Resistance by Applying Vibration to Tendons During Active Arm Movements](https://link.springer.com/chapter/10.1007/978-3-031-06249-0_11)——主动挥动中腱振动使真实重量/阻力被感知得更大;
  [Hirao et al., IEEE ToH 2023](https://arxiv.org/pdf/2209.00435):腱振动使视觉伪触觉增益可再提高约 13% 不被察觉;
  [CHI 2025: Tendon Vibration for Creating Movement Illusions in VR](https://dl.acm.org/doi/full/10.1145/3706598.3714003);
  雨宮研 2024 ToH(牵引错觉+腱振动叠加,已在 `雨宫.md`)。
- **落地**:I2S 双声道第二路即可驱动;这是**现有文献网(雨宮×梶本)交汇点上最自然的下一步**,
  也是最有"研究味"的扩展——牵引错觉(竿上)× 腱振动(腕上)的叠加在钓鱼语境未见先例。

### 改进 7:双执行器方向渲染(小硬件扩展)— 对应 W4

- **做法**:竿前段/后段各一个执行器,用 phantom sensation(强度插值)/ apparent motion
  (时序错开 SOA)渲染"拉力朝竿尖移动"、鱼左右窜逃时的方向瞬态。
- **依据**:[Tactile Brush (Israr & Poupyrev, CHI 2011)](https://www.cise.ufl.edu/~eragan/papers/Tang_VR2015.pdf) 一系;
  funneling/phantom sensation 文献
  ([Sci Rep 2025 方向线索对比](https://www.nature.com/articles/s41598-025-11436-6))。
- **落地**:同样吃 I2S 第二声道;与改进 6 二选一(声道资源冲突),建议先做 6。

### 改进 8:EMS 电刺激力反馈(远期)

- **做法**:对**拮抗肌**(持竿姿势下的肱三头肌等)按张力比例施加 EMS,产生真实的
  "被往下拉"的力——不是错觉,是真力。
- **依据**:[Lopes et al., CHI 2018](https://dl.acm.org/doi/10.1145/3173574.3174020)
  的弹弓/弩炮张力渲染与 HOLD 的张力保持同构;
  [EMS kinesthetic feedback 系统综述 (MDPI 2024)](https://www.mdpi.com/2414-4088/8/2/7)。
- **落地**:需要 EMS 硬件与安全设计,作为 proposal/远期路线写进研究计划即可,短期不做。

### 改进 9:实录数据校准(方案五的钓竿特化)

- **做法**:用板载 IMU 在**真实钓鱼**(或水桶+活鱼/人拉线模拟)时录竿上加速度谱,
  用实录谱校准 HOLD 的张力纹理与突进瞬态,替代手调参数。
- **依据**:NAIST 已证明钓竿振动数据足够丰富到能**判别鱼种**
  ([福田ら, DPSWS 2020: 釣竿の振動データに基づく魚種判別](https://ubi-naist.github.io/paper/IPSJ/202011_DPSWS_fukuda_SmartFishing.pdf));
  方法论来自 Kuchenbecker/Culbertson 实录-回放路线(已在 `先行研究.md`)。
- **落地**:板子已有 IMU + 串口,写个 200Hz dump 命令即可开录;数据本身也是将来论文素材。

---

## 三、新增研究室/团队(相对 `先行研究.md` 的增量)

| 团队/人物 | 机构 | 与 HOLD 的关联 | 对应改进 |
|-----------|------|----------------|----------|
| **南澤孝太** Embodied Media | 慶應 KMD | [Gravity Grabber (SIGGRAPH 2007)](https://dl.acm.org/doi/10.1145/1278280.1278289):指腹变形渲染重量感;TECHTILE toolkit:实录触觉分享。日本国内、可访问 | 3、9 |
| **Pedro Lopes** Human Computer Integration Lab | UChicago | [EMS 力反馈 (CHI 2018)](https://dl.acm.org/doi/10.1145/3173574.3174020),弹弓张力渲染与 HOLD 同构 | 8 |
| **William Provancher** / Tactical Haptics | Utah / 工业界 | [皮肤拉伸游戏手柄 (IEEE Haptics 2013)](https://ieeexplore.ieee.org/document/6486973/)、Reactive Grip;[做过 VR 钓鱼竿 demo](https://www.auganix.org/tactical-haptics-unveils-new-developments-for-its-virtual-reality-haptics-offering-including-a-vr-fishing-rod/)——工业界最接近"钓鱼拉扯感"的产品线 | 6 方向、远期硬件 |
| **Anatole Lécuyer** Hybrid team | Inria Rennes | 伪触觉 (pseudo-haptics) 奠基者,C/D 比操控 | 5 |
| **Ali Israr** | Disney Research→Meta | Tactile Brush:多执行器 apparent motion 的标准算法 | 7 |
| **荒川豊・松田裕貴研(ユビキタス)** | NAIST | [SmartFishing:真实钓竿振动数据](https://ubi-naist.github.io/paper/IPSJ/202011_DPSWS_fukuda_SmartFishing.pdf)——国内唯一做"竿上真实振动信号"的组 | 9 |

已有条目的**具体论文补充**(研究室已在 `先行研究.md`,论文是新的):

- 梶本研:牛山ら EuroHaptics 2022 腱振动×主动运动(改进 6 的核心依据)。
- 高椋ら 2016 有完整期刊版:**高椋慎也・雨宮智浩・伊藤翔・五味裕章
  《[VR魚釣りにおける牽引力錯覚の表現と応用](https://www.jstage.jst.go.jp/article/his/18/2/18_87/_pdf)》
  ヒューマンインタフェース学会論文誌 Vol.18 No.2 (2016)**——比会议稿信息量大,
  且直接就是"VR 钓鱼 + 牵引力错觉",引用时应引这个版本。⭐ 必读

---

## 四、优先级建议

| 优先级 | 改进 | 成本 | 一句话理由 |
|--------|------|------|-----------|
| **P0** | 4 泄力打滑 | 纯软件,~1 天 | 复用 tap 接口;补上真实钓鱼最标志性的反馈,顺便解决过拉警告 |
| **P0** | 1 抗适应包络 | 纯软件 | HOLD 最根本的感知缺陷(越拉越麻),文献依据扎实 |
| **P0** | 3 尺寸→频率+振幅双映射 | 纯软件 | `fc`/`na` 管线现成,30Hz=重的效应量大 |
| P1 | 2 门控补偿 | 一条曲线 | 便宜,但效应需实机 A/B 确认 |
| P1 | 5 屏幕伪触觉 | 纯软件(绘制) | 与触觉正交,适合做对照实验变量 |
| **P1.5** | 6 腕带腱振动 | +1 振子 | 学术增量最大:牵引错觉×腱振动叠加在钓鱼语境无先例 |
| P2 | 7 双执行器方向 | +1 振子 | 与 6 抢声道,后做 |
| P3 | 8 EMS / 9 实录校准 | 硬件/外采 | 写进研究计划,不急于实装 |

---

## 五、检索记录(2026-08-26)

主要检索词:vibrotactile adaptation countermeasure / vibration perceived weight handheld /
haptic fishing simulator 2024-2025 / skin stretch pulling sensation / EMS force feedback /
pseudo-haptics C/D ratio resistance / tendon vibration active movement / tactile gating
muscle contraction / 釣り 触覚提示 引き感(日本語)。
未单独展开但值得后续跟进:VibraForge(CHI 2025,多执行器原型工具链)、
Grabity(UIST 2017,非对称振动+重量)。
