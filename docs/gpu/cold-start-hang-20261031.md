# GPU 冷启动挂死记录（2026-10-31 07:20）

## 现象

首次带全套 GPU 冷启动开关上机（v107）：

```
--gpu-handoff-profile tools/gpu-handoff.json
--gpu-zap-firmware  build/gpu/a540_zap.mdt
--gpu-firmware      build/gpu/firmware        # a530_pm4/pfp + a540_gpmu
--gpu-smmu-init
--gpu-cold-start
```

设备没有进 XNU。Windows 侧持续枚举到 `EDK2 Fastboot`（VID_18D1/PID_D00D）且
设备状态为 Error；4 分钟内 adb/fastboot 都不可用；桥接的 15 秒恢复看门狗没有把
设备带回 TWRP。**只能长按电源恢复。**

## 这一版通过了哪些静态检查

- 策略 JSON 通过 `configure_gpu_handoff` 的全部校验：四张 RPM 表身份/宽度/跨表一致性、
  IOVA 窗口、NoC 预算、OPP 与 CX/MX corner 匹配、PM8005 S1 电压窗口；
- 四个开关逐个加进桥接都能构建成功（`--gpu-smmu-init` 依赖 `--gpu-firmware`，
  `--gpu-cold-start` 依赖前者）；
- 固件取自 MIUI V11.0.5.0 恢复包的 `/vendor/firmware`，文件与解析都正常。

所以失败点在**运行时**，不在构建期。

## 构建期已知约束（避免下次踩坑）

- `--full-memory` 与「有 `--gpu-handoff-profile` 但没有 `--gpu-cold-start`」互斥
  （`scripts/build-bridge.py:1446`）。想做"只加策略"的最小变量版本，就必须去掉
  `--full-memory`，而那会改变内存模型——两种都不是纯变量。
- 策略里的 level 是 RPM 调压等级：`48=MIN_SVS`、`64=LOW_SVS`、`128=SVS`、
  `192=SVS_PLUS`、`256=NOM`、`320=NOM_PLUS`、`384=TURBO`（见 `qcom-rpmpd.h`）。
  当前策略四域都用 48，即最低档 floor。

## 下一步的 bisect 计划

手机恢复后按顺序，每步只加一个开关，并且显式 `--watchdog-seconds 5`：

| 版本 | 开关组合 | 目的 |
|---|---|---|
| v109 | 策略 + ZAP + 固件 + 冷启动 + watchdog 5 | 与 v107 同路径，但失败后 5 秒内复位 |
| v110 | 去掉 `--gpu-smmu-init` | 判断是否 SMMU 页表初始化挂住 |
| v111 | 再去掉 ZAP/固件，只留策略（同时去掉 `--full-memory`） | 判断是否策略/RPM 交互挂住 |

判读方式：每次跑完进 TWRP 拉日志，看桥接最后打印到哪一步（RPM ACK / GX 上电 /
时钟 / SMMU / 固件装载 / ZAP 认证 / CP 自检）。若日志根本没有内核启动行，说明
挂在 ExitBootServices 之前，重点看桥接阶段的输出。
