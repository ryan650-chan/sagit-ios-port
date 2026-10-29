# sagit-ios-port

在小米 6（sagit / Snapdragon 835）上运行 iPadOS 18.7.10 用户态的研究性移植。
*Porting the iPadOS 18.7.10 userland onto a Xiaomi Mi 6 by running XNU on the bare metal.*

> 研究项目，不是可日常使用的系统。全程只通过 `fastboot boot` 在内存中运行，**不写入任何分区**，
> 断电重启即回到 Android。安全机制（AMFI、沙盒、数据保护）基本处于关闭状态，
> **不要在里面存放任何私人数据**。

---

## 当前状态（2026-10-29）

| 阶段 | 状态 |
|---|---|
| XNU 启动 | ✅ 16K 页、6 核 A55、GICv3、UART + 帧缓冲控制台 |
| 存储 | ✅ 自建 APFS 数据卷（挂在 `/private/var`，可读写）+ UFS 驱动 |
| 用户态启动 | ✅ launchd → 各守护进程 |
| 显示 | ✅ 1080×1920 DSI 面板，软件 CoreAnimation，**已确认有帧提交** |
| SpringBoard | ✅ 运行并绘制锁屏 |
| 解锁 | ❌ 停在 “Press Home to open”，缺 Home 键输入 |
| 触摸 | ❌ 未启用（I²C 总线栈未编入，见「已知问题」） |
| Wi-Fi / 蓝牙 / 蜂窝 / 音频 / 摄像头 / GPU | ❌ 未实现 |

启动后会停在锁屏界面。屏幕能显示、能出帧，但进不了桌面。

---

## 它是怎么跑起来的

```
fastboot boot  →  XnuBridge.efi (UEFI)  →  XNU  →  launchd  →  SpringBoard
```

关键点：

1. **设备树伪造**：用一份仿 iPhone 的设备树喂给 XNU，让内核以为自己在苹果硬件上启动。
2. **数据卷**：在 userdata 尾部切出空间，自己建 APFS 容器作 `/private/var`；
   镜像由 `scripts/build-apfs.py` 生成，卷 UUID 固定在 `SAGIT_DATA_UUID`。
3. **系统卷**：不刷分区，用 `imageboot-attach` 把 `SystemOS.dmg`（来自官方 IPSW）作为
   cryptex 附加上去，挂在 `/System/Cryptexes`。
4. **cryptex 框架补齐**：iOS 18 把大量框架挪进了 cryptex 卷。内核启动后用 bindfs
   做 union，把 `/System/Cryptexes/usr/lib`、`.../PrivateFrameworks`、`.../Frameworks`
   叠加到系统卷对应路径上。**不做这一步，`dlopen` 会成片失败、`pkd` 会因为
   `PlugInKitDaemon.framework` 找不到而每 10 秒崩一次，SpringBoard 卡死不出帧。**
5. **AppleKeyStore 桩**：`AppleKeyStore` 是闭源且依赖 SEP 的，本项目提供自研实现
   （无密码、系统钥匙包永远已加载、设备状态恒为已解锁），否则 `libaks` 全线失败。
6. **内核日志**：`sagit-log=1` 让内核把 console 环形缓冲持续写进 userdata 最后 32 MiB，
   之后用 TWRP 的 `dd` 直接读出来——这台机器没有可用的串口。

---

## 目录结构

```
drivers/            我们自己写的 kext（显示、输入、存储、总线、电源、安全……）
boot/bridge/        XnuBridge.efi 的源码：UEFI 引导、设备树生成、cryptex 交接
patches/xnu/        打给 XNU 的补丁 + series（应用顺序）
patches/<其它>/     按目标分组：llvm、go-apfs、edk2、linux、apfsprogs
scripts/            构建与打包工具（APFS 镜像、kernelcache 打包、bridge、启动镜像）
tools/              显示 / 触摸 / 存储的交接配置、日志抓取器
device_trees/       设备树源文件
docs/               每个子系统的调研与验证记录（触摸、显示、UFS、电源、GPU……）
AGENTS.md           改动约定：红线、构建陷阱、改动落点、取证方式
```

补丁顺序由 `patches/xnu/series` 逐行给出（文件名排序，`z` 开头的必须最后）。
`patches/` 之外的目标在同一套规则下各自成组。

## 准备

仓库**不含任何 Apple 二进制**。需要自备：

- **XNU 源码树**：`patches/xnu/series` 里的补丁按该顺序应用到对应版本的 XNU。
- **iPadOS 18.7.10（22H374，iPad7,11 / iPad7,12）固件**：从 Apple CDN 下载 IPSW，
  取出 `kernelcache.release.ipad7c`、`SystemOS.dmg`、`Cryptex1,AppOS`、根文件系统等，
  放到 `build/apfs/` 下（路径见 `scripts/build-port.sh`）。
- **主机构建环境**：Linux + LLVM/Clang 工具链、`apfs` 相关脚本依赖的 Python 3。

## 构建

```sh
./scripts/build-port.sh
```

分四步：编译 XNU → 打包 kernelcache（含本仓库的 kext）→ AMFI 处理 → 生成
`XnuBridge.efi` 与 `build/boot/boot-sagit.img`。

注意：**只有第一步真正编译 C 源码**，后两步只是重新打包。构建脚本会检查编译日志里
是否有 `error:`，并比对内核哈希，避免静默发一份旧内核。

## 运行

```sh
./scripts/run-fastboot.sh              # 只启动
./scripts/run-fastboot.sh --pull-log   # 启动 → 等自复位 → 用 TWRP 读日志
```

启动参数里的 `sagit-reboot-after=<秒>` 会让内核到点自己重启回 recovery，
迭代时很方便，默认关闭。

## 读日志

没有串口可用，所以日志走一条别的路：

1. 内核把 msgbuf 环形缓冲写进 userdata 最后 32 MiB 的固定窗口；
2. 进 TWRP 后，`tools/sagit-log-pull.py` 用 `dd` 把窗口读出来并解码。

```sh
python3 tools/sagit-log-pull.py --adb adb --tail 3
```

窗口里还能塞别的东西——`sagit_dump_file()` 会把任意文件搬到窗口上方 8 MiB 处，
于是连只读系统卷里的二进制都能掏出来分析（`usermanagerd` 就是这么逆出来的）。

## 已知问题 / 下一步

- **触摸**：`QcomI2C` + `GenericI2C` 未编入内核清单，`SynapticsTouch` 没有 provider，
  从来没有启动过。加回去需要先解决 QUP5 的时钟交接：不加 `--touch-prepare` 会挂死，
  加了 `--touch-prepare` 会卡在 EFI 阶段。下一步是给那条路径加打印定位。
- **解锁**：iPhone/iPad 的锁屏要 Home 键。试过往 `QcomButtons` 的 HID 描述符里注入
  `AC Home (0x223)` 并在启动后定时脉冲，SpringBoard 没有响应。代码树里还有
  `FingerprintHID`（把前置指纹当 Home 键）但未编入，是更正确的方向。
- **watchdogd** 仍在崩溃循环（SIGTRAP），未影响出帧。
- 分辨率相关的取舍：我们是 16:9（1080×1920），如果改成 iPhone X 那种全面屏机型，
  iOS 会按 1125×2436 排版，底部 516 像素（含 Home 指示条）会被切掉，
  所以目前保留 Home 键机型的身份。

## 许可与声明

- 本仓库只包含本项目编写的代码与文档。Apple 的 XNU 源码按 APSL 发布，kext 二进制
  来自你自己的 IPSW，均不随仓库分发。
- 各源文件顶部保留原始许可头（GPL-2.0 / BSD-2-Clause / APSL）。
- 与 Apple 无任何关联，未获其授权。仅供内核与驱动移植研究，请勿用于任何依赖
  安全性的场景。

## 关于仓库布局

这是一个独立的研究仓库，**没有**采用 UniXNU 的设备仓库布局（不带 `board.toml` /
`porting.toml` / `feeds.toml`，驱动也没有拆成独立组件仓库）。补丁目录和
`AGENTS.md` 的约定沿用了他们的习惯，方便将来迁移；现阶段先把东西跑通比先对齐目录更有意义。
