# 2026-10-31：桌面打通 + GPU 第一步

一晚上的实测记录。结论都来自真机日志与从显示驱动扫描缓冲导出的帧。

## 结果

主屏幕起来了。导出帧（720×1280，最近邻缩自 1080×1920 扫描缓冲）里能看到
Settings 图标、Search 胶囊和 Dock。背景纯黑，因为还没有壁纸服务。

```
首帧     ~93 s（8 核；4 核时 ~128 s）
buddy    未启动（设置向导被跳过）
GPU      QcomGPU: native IOGPUScheduler + IOGPUWorkQueue enabled
```

## 设置向导为什么一直跳不过

之前几版的思路是"写偏好"：往 `com.apple.purplebuddy.plist` 里写
`SetupDone=true` 等键。这条路在 iOS 18 上不成立，原因有三层，逐层排掉才通：

1. **文件属主**。内核用 root 写、权限 0600，cfprefsd 以 mobile 跑，打不开——
   整个域读起来是空的。改成 501:501 / 0644 后探测输出能证明可读。
2. **文件被覆盖**。可读之后仍然无效：实测文件从 340 字节变成 180 字节，
   属主 501——cfprefsd 把它自己的内存态整体刷回去了。内核抢不过它。
3. **真正的门槛是激活**。过掉前两层后向导依旧，界面只有 "Connect to Wi-Fi" 和
   "Connect to Mac or PC"，没有跳过项。未激活的设备就是这么要求联网的。
   桥接加 `--development-hactivation`（强制要求 `--development-legacy-data`）
   写 `/chosen` 的四元组后，向导彻底不再出现。

**教训**：先确认"设备是激活的"，再谈偏好。偏好写得再对，激活没过也没用。

## EventTiming profile

过了向导后 UIKit 弹 "Misconfigured Device"，缺
`/System/Library/EventTimingProfiles/SDM835.DEFAULT.plist`（文件名按设备树的
`target-type` 拼）。系统卷只读，所以改成运行时方案：开机把空字典 plist 写进
Data 卷，再用现成的 bindfs union 叠到系统目录上。弹窗消失。

上游用的是离线 APFS COW 补丁，效果一样；运行时叠加的好处是不用改系统卷。

## 身份字符串

- 序列号：`IOPlatformSerialNumber`（设备树根节点）。**不要**用 `serial-number`——
  内核那个解析器按 Apple 的二进制布局读第 13~20 字节，短字符串会读越界。
- 型号：`regulatory-model-number`。
- 名称：`product-name`。

## GPU：第一个里程碑

设备树里 `gpu@5000000` 与 `gpucc` 本来就有，只是 `disabled`。打开节点并把
`QcomGPU` / `QcomGPUCC` / `QcomSCM` / `QcomRPM` 编进内核后，真机日志确认：

```
QcomGPU: native IOGPUScheduler + IOGPUWorkQueue enabled
QcomSCM / QcomRPM / QcomGPUCC / QcomGPU  <class ...>
```

四者都加载、IOGPU 服务发布，没有崩溃。**这一步不碰 GPU 寄存器**：没有交接
记录时驱动只发布诊断服务。

### 硬件提交还缺什么

1. **固件**：`a530_pm4.fw`、`a530_pfp.fw`（公开，linux-firmware 的 `qcom/` 下就有），
   以及 `a540_gpmu.fw2` 与签名 ZAP `a540_zap.mdt`（linux-firmware 没有，
   必须从 Android 的 vendor/system 镜像取）。这台机器的 system 分区里已经不含
   firmware 目录，所以要走 MIUI ROM。
2. **策略 JSON**：`--gpu-handoff-profile` 需要 RPM 域/时钟/限值/interconnect、
   IOVA 窗口、平均/峰值带宽、`firmware_power` 频率电压、`gfx_regulator`。
   上游用完删掉了，格式在 `docs/gpu/hardware-submission.md`。
3. 之后按顺序：`--gpu-zap-firmware` → `--gpu-firmware` → `--gpu-smmu-init`
   → `--gpu-cold-start`。

## CPU 调频的坑（已回退）

`QcomOSM` 接上、桥接加 `--cpu-osm-handoff` 之后，整机慢了 7 倍：两簇停在
300 MHz 辅助时钟（上游记录里也遇到过，根因是 SAW 板级初始化不完整）。
不要在没有补齐 SAW 初始化的情况下开 OSM。

## 构建环境

GPU kext 需要主机 **LLVM 23**（`tools/gpu-compiler/AIR.cpp` 读 AIR 位码，
代码里写死主版本号 23）以及一份 Mesa 24.3.4 源码树编 IR3。官方 LLVM 发布包是
静态库构建，`llvm-config --libs` 缺 `target`/`passes`，需要一个包装器补上
组件与 `--start-group`。
