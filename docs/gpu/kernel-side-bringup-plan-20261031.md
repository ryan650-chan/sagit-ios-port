# GPU 重做方案：改成内核侧初始化（参考 UniXNU）

2026-10-31 决定：**放弃 EFI 桥冷启动**，改为内核驱动完成 GPU 上电与初始化。
原因是 EFI 路线对前置状态（GX 已折叠、RPM 逐项 ACK）要求苛刻，失败会停在 EFI
且不留日志；两次上机都卡死在那里。

## UniXNU 的分层（参考对象）

| 仓库 | 职责 |
|---|---|
| `UniXNU/Adreno740` | GPU 内核驱动：加速器服务、CP 初始化、固件校验、GMU/HFI |
| `UniXNU/ArmSmmu4K` | 内核侧 SMMU 驱动 |
| `UniXNU/AdrenoFirmware`、`AdrenoPM4` | 固件/命令包工具 |
| `UniXNU/AdrenoMetal` | Metal 前端 + metal2vulkan 翻译 |
| `UniXNU-sm8550-common` | SoC 公共层：CPU/GIC/UFS、**GMU 操作、GPU SMMU 集成**、XNU 补丁；`kits/Sm8550Platform` 保存 `ShengGmu*`/`ShengGpuSmmu*` |
| 设备仓库 | **内存布局、外设、固件、电源策略**、打包 |
| `ShengPreflight` | 预 OS 阶段只做固件预检/暂存，不做 GPU 硬件时序 |

要点：**引导器只负责把固件、策略、内存交给内核；上电、复位、固件装载、SMMU、
CP 启动全部在内核驱动里完成。**

## 我们的映射

| UniXNU | 我们现有代码 | 现状 |
|---|---|---|
| Adreno740 | `drivers/gpu/QcomGPU`（AdrenoStartup/Recovery/Submit/Mapper） | 已有，但**依赖 EFI 的 `qcom,gpu-handoff`** |
| ArmSmmu4K | `QcomGPUMapper` + `AdrenoAddressSpace` | 已有（每作业页表） |
| SoC 公共层 | `QcomGPUCC` / `QcomGCC` / `QcomRPM` / `QcomSCM` | 已有 kext，未参与 GPU 上电序列 |
| 设备仓库 | `device_trees/xiaomi-sagit.dts` + `tools/gpu-handoff.json` | 策略 JSON 已就绪 |
| 预 OS 阶段 | `boot/bridge/gpu_handoff.cpp` | **当前在做硬件时序——要拆掉这部分** |

## 分阶段实施

**P0（立即）**：回退到 v106 行为（GPU kext 在，节点 `okay`，不带任何 GPU 桥参数）。
设备可用、桌面正常，作为后续每阶段的对照基线。

**P1 桥变被动**
- 桥只保留：把固件镜像放进保留内存、把策略记录写进设备树、预留内核 GPU 窗口；
- 删除桥内的 RPM ACK / GX 上电 / 时钟 / 复位 / ZAP / SMMU / CP 时序；
- 失败点从"EFI 挂死"变成"内核驱动报错"，日志可抓。

**P2 内核侧上电（新的核心工作）**
在 `QcomGPU` 里按顺序实现，每一步失败都打印明确错误并放弃提交：
1. 向 `QcomRPM` 申请 domain/clock/interconnect（用策略 JSON 的值）；
2. 通过 `QcomGPUCC`/`QcomGCC` 开 GPU 分支时钟与 GX 根；
3. GX/GDSC 上电、解除 clamp/retention；
4. GPU reset（`AdrenoStartup::reset`）。

**P3 内核侧固件与 ZAP**
- 从保留内存（或 Data 卷）读 `a530_pm4.fw`/`a530_pfp.fw`/`a540_gpmu.fw2`，SHA-256 校验；
- 通过 `QcomSCM` 的 PAS 通道完成 ZAP 认证；
- 装 GPMU 指令 RAM、启动 CP，等待非安全 timestamp。

**P4 SMMU + 提交**
- 用 `QcomGPUMapper` 建 GPU 页表，配置 SID 0 路由；
- 恢复既有 Fill32/Barrier/Copy32 提交与真实栅栏。

**P5 显示/合成**
- GPU 渲染表面接 DPU 扫描输出（现有 `QcomDisplay` 的 GPU 路径）。

## 验证与回退

- 每阶段单独构镜像、单独上机；上机后进 TWRP 拉日志看阶段输出；
- 任何阶段失败只回退该阶段，v106 镜像始终保留；
- 判断标准：XNU 能进桌面（证明上电没搞坏系统）→ GPU 服务可用（IORegistry）→
  提交自检 → 合成帧。

## 增量 A 结果（2026-10-31 13:15，v110）

在内核里通过 `ClockControllerRequest` 只读探测，四个控制器全部可查询：

```
controller=2 (RPM)  caps=0x68
controller=5 (GPUCC) caps=0x267   Clocks|Domains|Resets|Reporting|UserAccess|RetainedGPUState
controller=3 (SPMI) caps=0x68
controller=1 (GCC)  caps=0x2e7
```

GPU 当前状态（未上电）：

```
gpucc clock=3 gfx3d_clk_src  rate=9600000     (9.6 MHz 辅助时钟)
gpucc clock=7 gfx3d_clk      rate=9600000
gpucc clock=4/8 rbbmtimer    rate=19200000
gpucc clock=5/9 gfx3d_isense rate=19200000
gpucc domain=1 state=0        (关)
gpucc domain=2 state=0        (关)
```

结论：**内核能指挥这套时钟/电源子系统**；GPU 从未被上电（9.6 MHz + 域关闭），
这解释了为什么 EFI 冷启动对前置状态那么苛刻。

## 增量 B 设计：把桥里的 PowerStartup 搬进 kext

`boot/bridge/gpu_power.h` 里的 `PowerStartup` 就是完整的 a5xx 冷启动序列
（RPM 供电 → GCC gates → GPUCC configure/initialize → CX/GX 域 → Fabia PLL →
CRC → 目标 OPP），它依赖 `GCC::Controller`、`QcomPower::ClockHardware`、
`RPMRegulators`、`RPMTransport` —— 这些对象在 GCC/RPM 的 kext 里已经存在。

所以做法是：**把这套代码编进 kext，通过平台函数暴露**，由 QcomGPU 按 DT 策略调用。
好处：同一份经过验证的序列，但失败只在日志里报错，不会挂死 EFI。

需要的改动：
1. `ClockControllerProtocol.h` 增加一个 GPU 冷启动操作（携带 frequency/cx/mx），
   并在 `valid()` 里放行；
2. `QcomGPUCC`（或 GCC 后端）实现该操作，内部复用 `PowerStartup`；
3. `QcomGPU` 读 DT 策略并调用，逐阶段 `kprintf`；
4. 桥只保留固件/策略搬运，删除 GPU 时序。
