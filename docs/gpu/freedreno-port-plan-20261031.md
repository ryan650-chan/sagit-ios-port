# 用 Freedreno 做内核侧 GPU（调研与移植清单）

2026-10-31。结论：**Freedreno 的内核侧（drm/msm + adreno a5xx）就是我们要的上电/固件/
SMMU 序列**，用户态的 GLES 部分对 iOS 桌面用不上（iOS 走 Metal），但它的 IR3 编译器
我们已经在用（`tools/gpu-compiler`，Mesa 24.3.4）。

## Freedreno 的组成

| 层 | 内容 | 许可证 |
|---|---|---|
| 内核（Linux） | `drivers/gpu/drm/msm` + `adreno/a5xx_gpu.c`、`a5xx_power.c`、`adreno_gpu.c`、`adreno_ringbuffer.c`、`msm_gem/msm_mmu` | GPL-2.0 |
| IOMMU | `drivers/iommu/arm-smmu.c` + `arm-smmu-msm8998.c` | GPL-2.0 |
| 用户态 | Mesa `src/gallium/drivers/freedreno`（a5xx 渲染、blit、纹理）、`src/freedreno/ir3`（编译器） | MIT |
| 后端 | `freedreno_msm`（DRM）与 `freedreno_kgsl`（Android kgsl ioctl） | MIT |

本仓库已有参考源码：`docs/gpu/reference/a5xx_gpu.c`（1809 行）、`a5xx_power.c`（390 行）、
`a5xx_reg.h`、`arm-smmu-msm8998.c`（4514 行）、`a5xx.xml`/`a5xx_catalog.c`（寄存器目录）。

## 关键序列（从 a5xx 源码抽出）

**上电（`a5xx_pm_resume` / adreno 电源）**
1. RPM/regulator：CX/MX 电压与 NoC 带宽；
2. GPUCC：GPU 分支时钟、GX 根；GCC：gpu 相关 gate；
3. GX 电源域开、解除 clamp/retention；
4. `a5xx_hw_init` 里的复位段（RBBM soft reset）。

**`a5xx_hw_init`（寄存器初始化，约 260 行）**
VBIF QoS → RBBM 错误上报/挂死检测 → 性能计数器 → UCHE（cache way、GMEM 映射、
trap/write-thru 基址）→ CP 配置 → VFD/SP/TP 等块 → 最后 `CP_ME_INIT`。

**固件与 ZAP**
- `a5xx_power_init`：A530/A540 才有 GPMU——先 `a540_lm_setup`、`a5xx_pc_init`，
  再 `a5xx_gpmu_init`（把 `a540_gpmu.fw2` 写进 GPMU 指令 RAM），最后 `a5xx_lm_enable`；
- `a5xx_zap_shader_init`：首次走 `adreno_zap_shader_load`（SCM + PAS ID 13 认证）；
  之后只需 `qcom_scm_set_remote_state(SCM_GPU_ZAP_SHADER_RESUME, 13)`。

**提交**
`a5xx_submit` 写 ring：CP 上下文切换 → IB 列表 → `CP_EVENT_WRITE`(timestamp/fence)
→ 更新 `memptrs->fence`；`a5xx_flush` 推进 wptr。

## 与本仓库现有实现的映射

| Freedreno | 我们 | 状态 |
|---|---|---|
| `a5xx_hw_init` 寄存器段 | `AdrenoPipeline.h`/`QcomGPUHardware.cpp`（VBIF/CP/ME_INIT 已有） | 🟡 部分，需按 a5xx_reg.h 补全 |
| `a5xx_power_init` + GPMU 固件装载 | `AdrenoStartup.h`（GPMU 寄存器已定义） | 🟡 有骨无肉，固件来自 EFI |
| `adreno_zap_shader_load`（SCM PAS） | `QcomSCM.kext` + `QcomGPUSecure` | 🟡 已有 PAS 通道 |
| `arm-smmu` + `msm_mmu` | `QcomGPUMapper` + `AdrenoAddressSpace` | ✅ 每作业页表已有 |
| `a5xx_submit` / ringbuffer | `AdrenoSubmit.h` + `QcomGPUHardware.cpp` | ✅ 已有 Fill32/Barrier/Copy32 提交 |
| 电源/时钟（regulator/devfreq） | `QcomRPM` + `QcomGPUCC` + `QcomGCC` | 🟡 驱动在，未参与 GPU 上电 |
| IR3 编译器 | `tools/gpu-compiler`（Mesa 24.3.4 IR3） | ✅ 已用于自带着色器 |

## 移植清单（内核侧，按顺序）

1. **QcomGPU：新增内核侧上电**
   `powerUp()`：向 QcomRPM 申请 domain/clock/interconnect（值来自 `tools/gpu-handoff.json`），
   经 QcomGPUCC/GCC 开时钟，开 GX、去 clamp，复位 RBBM。
2. **QcomGPU：`hwInit()`**
   按 `a5xx_reg.h` 把 `a5xx_hw_init` 的寄存器序列逐段搬进来（VBIF→RBBM→UCHE→CP→块）。
   这些寄存器名在仓库的 `docs/gpu/reference/a5xx.xml` 里有编号，可直接对表。
3. **QcomGPU：`loadFirmware()`**
   读 `a530_pm4.fw`/`a530_pfp.fw`（校验 SHA-256），装入 CP 的微码窗口；
   `a540_gpmu.fw2` 写入 GPMU 指令 RAM；顺序照 `a5xx_gpmu_ucode_init`。
4. **QcomGPU：`zapResume()`**
   经 QcomSCM 调 PAS 13（先用 `adreno_zap_shader_load` 等价流程，再走 `set_remote_state`）。
5. **QcomGPUMapper：SMMU**
   参考 `arm-smmu-msm8998.c` 配 SID 0 的 context/stream 表；我们已有页表构建器。
6. **桥只做搬运**：把固件镜像和策略写进保留内存/DT，不碰 GPU 寄存器。

## 两个必须说清楚的现实

1. **Freedreno 给的是 GL/GLES，iOS 桌面要的是 Metal。** 内核侧移植好之后，GPU 能算、
   能提交，但 SpringBoard 的合成仍走 Metal。可选三条：Metal→Vulkan（Turnip，UniXNU/
   Duo2 路线）、Metal→GLES（要自己做翻译）、或继续用本仓库已有的 `QcomMTL`（直连 IR3）。
2. **GPL 边界**：Freedreno 内核侧是 GPL-2.0，本仓库是 BSD-2-Clause。移植时要按"独立实现 +
   参考"的方式写（类似现在 `AdrenoPipeline.h` 的 MIT 声明），不要直接拷贝 GPL 代码进 BSD 仓库。
