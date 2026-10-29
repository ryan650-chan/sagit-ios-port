# AGENTS.md

给在这个仓库里干活的人（和维护它的 agent）看的操作约定。README 讲这个项目是什么，
这里讲**改动时该守什么**。

## 一句话

把 iPadOS 18.7.10（22H374）的用户态跑在小米 6（sagit / SDM835）裸机上：
XNU 是真内核，SpringBoard 是真的苹果二进制，全程只 `fastboot boot`，不写分区。

## 硬性红线

1. **不提交任何 Apple 二进制**。kernelcache、`*.dmg`、`*.aea`、`*.ipsw`、trust cache、
   dyld 共享缓存一律不进仓库；`.gitignore` 已经挡住常见后缀。需要固定来源时，
   只记 URL 与 SHA-256（参照 `proprietary-files.txt` 的写法）。
2. **不提交构建产物**。`build/` 整个目录都是产物，包括 `build/output/`、
   `build/apfs/`、`build/boot/`。
3. **不要为了"能跑"而放宽验证**。`docs/*/validation*.json` 里的结论都有证据；
   改动如果推翻了某条结论，更新那个文件，而不是绕过去。
4. 这个移植默认关闭 AMFI、沙盒和数据保护。**任何"为了安全"的改动都不要假装它成立**，
   文档里如实写现状。

## 目录

| 路径 | 内容 |
|---|---|
| `drivers/` | 我们写的 kext。每个子目录一个 bundle，`Info.plist` 里是匹配条件 |
| `boot/bridge/` | `XnuBridge.efi`：UEFI 引导、生成给 XNU 的设备树、cryptex 交接 |
| `patches/<set>/` | 补丁集，按目标分组：`xnu/`、`llvm/`、`go-apfs/`、`edk2/`、`linux/`、`apfsprogs/` |
| `patches/xnu/series` | XNU 补丁的应用顺序，逐行一个文件，`#` 注释 |
| `scripts/` | 构建与打包工具；入口是 `build-port.sh` |
| `tools/` | 交接配置（显示/触摸/存储）、日志抓取器、kext 清单 |
| `device_trees/` | 板级 DTS 源 |
| `docs/` | 每个子系统的调研与验证记录 |

## 构建

```sh
./scripts/build-port.sh          # XNU -> kernelcache -> AMFI -> XnuBridge.efi -> boot.img
./scripts/run-fastboot.sh        # 只启动，断电即恢复
```

两个必须知道的陷阱：

- **`make kernelcache` 只打包，不编译。** 只有 `make build` 编 C 源码。编译错误会被
  日志吞掉，流水线照样产出镜像——所以脚本里比对内核哈希，哈希没变就是没重编。
- **`--touch-prepare` 会挂在这台机器上**（卡在 EFI 阶段，连内核日志都出不来）。
  动触摸之前先修那条路径，不要直接把它加进构建参数。

## 改动的落点

- **kext 要不要编进内核**：由 `docs/sdm835/ios18/stages/08-full/kexts.txt` 决定。
  加驱动是往这个文件加 bundle id，不是改构建脚本。
- **启动参数**：在 `scripts/build-port.sh` 的 `ARGS` 里。带 `sagit-` 前缀的都是本项目
  自己加的（日志窗口、数据卷、追踪开关）。
- **内核里的补丁**：走 `patches/xnu/`。文件名前缀决定顺序，`z` 开头的必须最后；
  应用器按文件名排序，只有少数几个有显式依赖（见 `scripts/kernel_patch_stack.py`）。
- **cryptex 缺文件**：iOS 18 把大量框架放在 cryptex 卷里。系统卷上找不到的框架，
  先确认它在 `/System/Cryptexes/...` 下，然后用 bindfs union 叠出来，
  不要去改系统卷。

## 取证方式

没有串口。日志走 userdata 最后 32 MiB 的窗口：

```sh
python3 tools/sagit-log-pull.py --adb adb --tail 3
```

需要看只读系统卷里的某个二进制时，用内核态的 `sagit_dump_file()` 把它搬到同一块
窗口区（窗口上方 8 MiB），再在 TWRP 里 `dd` 出来。`usermanagerd` 就是这么逆向的。

## 提交

前缀：`feat(<area>)`、`fix(<area>)`、`docs`、`build(<area>)`、`chore`。
`<area>` 用子系统名（`display`、`touch`、`usermgr`、`cryptex`、`apfs`…）。
一条提交只做一件事；涉及结论变化时，同一条里更新 `docs/` 下对应的记录。

## 和其它布局的关系

本仓库是独立的研究仓库，**不遵循** UniXNU 的设备仓库布局（没有 `board.toml` /
`porting.toml` / `feeds.toml`，驱动也没拆成独立组件仓库）。将来若要迁过去，
需要先有 `UniXNU-sdm835-common`、对应的 OS profile，并把 `drivers/` 拆成组件。
