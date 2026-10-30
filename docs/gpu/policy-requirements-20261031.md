# GPU 交接策略：重建清单（2026-10-31）

`scripts/build-bridge.py --gpu-handoff-profile <policy.json>` 读取一份平台保留策略。
上游用完把文件删了，这里把**格式、约束、每一项的来源**写清楚，照着填即可。

## 校验器给的结构（`configure_gpu_handoff`）

```json
{
  "version": 2,
  "iova_first": "0x10000000",
  "iova_bytes": "0x4000000",
  "average_bandwidth": 100000000,
  "peak_bandwidth": 400000000,
  "rpm_domains":      [[id, active_level, sleep_level], ...],   // 四条
  "rpm_clocks":       [[index, active, sleep], ...],            // 27 条
  "rpm_clock_limits": [[index, active_max, sleep_max], ...],    // 12 条，身份 0..8,12,19,26
  "rpm_interconnect": [[id, max, active_floor, sleep_floor], ...], // 八条
  "firmware_power": { "frequency_hz": 257000000, "voltage_uv": 800000 },
  "gfx_regulator":  { "id": 1, "sid": 5, "minimum_uv": 524000, "maximum_uv": 1100000 },
  "zap_memory": [ "0x80000000", "0x1000000" ]
}
```

硬约束（不满足会直接 `ValueError` 并指出字段）：

- `iova_first >= 0x1000000`，`iova_bytes` 4 KiB 对齐，`0x800000 <= size <= 0x20000000`；
- `0 < average <= peak`，且 peak 换算到 NoC 后不能超过 `rpm_interconnect` 里给的上限；
- **冷启动**时 `firmware_power.frequency_hz` 必须是 MSM8998 的 OPP 之一：
  `257000000 / 342000000 / 414000000 / 515000097 / 596000097 / 670000048 / 710000097`，
  并且 CX/MX 的 active level 不低于该 OPP 对应的 corner
  （`48 / 64 / 128 / 192 / 256 / 320 / 384`）；
- `gfx_regulator` 必须是 PM8005 S1：`id=1`、`sid<=15`、`524000 <= uv <= 1100000`、
  且 `firmware_power.voltage_uv` 落在区间内并且 4000 的整数倍；
- `zap_memory` 在低 4 GiB、4 KiB 对齐、不超过 16 MiB。

## 每一项的来源

| 字段 | 来源 |
|---|---|
| `rpm_domains` | MSM8998 的 CX / MX / EBI 电压域在 RPM 里的资源与 corner 表（下游 `msm8998.dtsi` 的 `rpm-regulator`/rpmpd 节点） |
| `rpm_clocks` / `rpm_clock_limits` | RPM 时钟命名空间（27 条时钟、12 条限制，身份 0..8、12、19、26） |
| `rpm_interconnect` | RPM 互联八条；驱动里已内置 `0x2000 aggr1`、`0x2001 SNOC`、`0x2002 BIMC` 与四条 A1NoC/SNOC/BIMC/EBI 路由 |
| `iova_*` | 给 GPU 的空闲 IOVA 窗口；构造时要求为空且与保留页表不重叠 |
| `average/peak_bandwidth` | GPU 的 BIMC 平均/峰值需求，必须落在 interconnect 上限之内 |
| `firmware_power` | GPU 冷启动目标 OPP（频率 + PM8005 S1 电压） |
| `gfx_regulator` | PM8005 S1 的路由与电压窗口 |
| `zap_memory` | ZAP 预留的低 DRAM 区域（若不用 EFI 自带映射） |

## 验证顺序（每一步失败都会明确报错）

```sh
python3 scripts/build-bridge.py ... --gpu-handoff-profile tools/gpu-handoff.json
# 再逐步加：
#   --gpu-zap-firmware <a540_zap.mdt 等>
#   --gpu-firmware <目录，含 a530_pm4.fw / a530_pfp.fw / a540_gpmu.fw2>
#   --gpu-smmu-init
#   --gpu-cold-start
```

策略值错误的典型报错：`cold GPU startup requires a supported OPP and sufficient
CX/MX policy corners`、`GPU NoC budget exceeds RPM policy`、
`GPU GFX supply is outside the PM8005 S1 voltage policy`。

## 固件来源

| 文件 | 来源 | 现状 |
|---|---|---|
| `a530_pm4.fw` | linux-firmware `qcom/` | ✅ 已下载（19,572 字节） |
| `a530_pfp.fw` | linux-firmware `qcom/` | ✅ 已下载（16,144 字节） |
| `a540_gpmu.fw2` | MIUI ROM 的 system/vendor 镜像 | ⏳ 从 V11.0.5.0.PCACNXM recovery ROM 提取 |
| `a540_zap.mdt`（+ 签名分段） | 同上 | ⏳ 同上 |

解包链：`unzip` → `system.new.dat.br` 用系统 `libbrotlidec`（`~/miui/brotli_dec.py`）
→ `~/miui/sdat2img.py` → `debugfs -R "dump ..."`。
