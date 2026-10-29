# 日志记录器重写 — 变更与验证记录

日期：2026-10-26
依据：UniXNU `porting/log-capture` §4–§5 + sheng 参考实现（`package/BootLog.c`）

---

## 1. 旧实现为什么必须换掉

旧 `sagit_autolog_start()`（`devfs_vfsops.c`）：

```c
if (rootdevice[0] == 0) return;                 /* rootdevice = IOFindBSDRoot() 的根*卷*名 */
snprintf(path, "/dev/%s", rootdevice);
vnode_open(path, ...);                          /* 依赖 devfs → 依赖根卷已挂载 */
bytes = bsize * bcount;                         /* 这是卷大小，不是分区大小 */
off   = (bytes - 32 MiB) & ~0xfff;
vn_rdwr(UIO_WRITE, vp, ..., off, ...);          /* 写进 APFS 容器内部 */
```

后果有两层：

1. **主机读不到**：host 读的是 userdata **分区**尾部 32 MiB，writer 写的是**卷**尾部 → 窗口恒为全零（实测 33,554,432 字节，非零字节 0）。
2. **可能破坏容器**：每 2 秒把 1 MiB 覆盖写到容器内部固定位置。日志中 `free 98→94→88→86`、`ENOSPC` 刷屏、`Too many corpses being created` 与这个行为时间上吻合（吻合，非证明）。

---

## 2. 新实现

### 2.1 文件

| 文件 | 作用 |
|---|---|
| `kernel/bsd/miscfs/devfs/sagit_bootlog.c` | 新写入器（格式 v2、头最后写、boot id、序列、状态、CRC） |
| `kernel/bsd/miscfs/devfs/sagit_iokit.cpp` | 新增 `sagit_userdata_bsdname()`：选 **Whole=false 且 ≥ 64 GiB** 的 media，即 userdata 分区 |
| `kernel/bsd/conf/files` | 新增 `bsd/miscfs/devfs/sagit_bootlog.c standard` |
| `kernel/bsd/miscfs/devfs/devfs_vfsops.c` | 删除旧 autolog 全部代码；钩子改调 `sagit_bootlog_start()`；dump 增加 `sagit_bootlog_report()` |

### 2.2 对齐文档的四条要求

| 文档要求 | 实现 |
|---|---|
| 预留区域边界唯一，构建器/写入器/读取器一致 | 窗口 = 分区最后 32 MiB，块对齐；几何写进 header，host 端比对 `window_offset` 不一致直接报 MISMATCH |
| 内核写入器接入存储驱动限定范围读写 | 通过分区块设备节点 `vn_rdwr`，仍然经过 QcomUFS；范围由 `64 GiB` 下限 + 窗口边界双重约束 |
| header 最后写，读者不信任未计数内容 | 每轮先写 body（blocks 1..32），成功后才写 header（block 0） |
| boot id / 快照 / 写状态 | header 内含 `boot_id`、`seq`、`writes`/`errors`、`status`、body CRC32 |

### 2.3 窗口布局（v2）

```
block 0         header  "SAGITLG2"
blocks 1..32    kernel msgbuf，最旧字节在前，零填充
```

### 2.4 启动参数

```
sagit-log=1            # 启用（替代旧的 sagit-autolog=1）
sagit-log-period=2     # 每轮秒数，最小 1
```

### 2.5 放弃的方案（记录原因）

先按 sheng 的做法把窗口放进 QcomUFS kext（`sagit_ufs_log_open/write/sync` 五个导出符号），内核侧 `weak_import` 调用。**内核链接失败**：

```
"_sagit_ufs_log_open", referenced from: _sagit_bootlog_thread
ld: symbol(s) not found for architecture arm64
```

原因：本项目内核用 `-Wl,-static` 链接，undefined 符号一律报错，`weak_import` 不生效。kext 侧符号确实导出了（`llvm-nm` 可见 5 个 `T _sagit_ufs_log_*`），问题在链接期不是运行时。故改为核内解析目标 + 块设备写入。

### 2.6 实机第一轮：EBUSY(16)

v2 镜像上机后 dump 打印 `sagit-port BOOTLOG: failed 16`（`EBUSY`）。原因不是选错设备，而是**打开方式**：

`vnode_open("/dev/diskXsY", FREAD|FWRITE)` → `spec_open` → 设备 d_open，
而根卷正挂在该分区上，IOStorageFamily 以 EBUSY 拒绝第二次独占式打开。

改为 **`bdevvp(dev_t)` + `vn_rdwr`**：

* `dev_t` 从 IOMedia 的 `BSD Major` / `BSD Minor` 属性取（IOKit 已发布，无需 devfs）
* `bdevvp` 按 dev_t 直接构造 spec vnode，**跳过 `spec_open` 的挂载占用检查**
* 传输仍由 QcomUFS 完成；窗口位于 APFS 容器之外，不触碰文件系统元数据

这样既不需要 kext 符号（避开 `-static` 链接问题），也不受挂载占用阻挡。


---

## 3. 验证结果

| 步骤 | 结果 |
|---|---|
| `make build TARGET=SDM835 JOBS=3` | ✅ `no undefined symbols`，16,180,792 B |
| `build-drivers --driver QcomUFS` | ✅ 恢复原状后编译通过 |
| `make kernelcache`（含 patcher 单测） | ✅ 42 Kexts，SHA256 `736a4e2c…` |
| AMFI PMGR | ✅ `a44dd9a7…` |
| EDK2 (`scripts/build-sagit.sh`) | ✅ `build/edk2-sagit/boot-sagit.img` 11,245,568 B |
| `var.apfs` 复原 | ✅ 从旧 boot.img 的 LZMA payload 抽出，SHA256 `7a3f6d21…` 与文档一致，卷 UUID `6F1C0F5E-…` 正确 |
| `build-bridge.py` | ❌ `--development-no-sep requires --firmware-report` |

### 3.1 唯一阻塞

`build-bridge.py:1561`：

```python
if args.development_no_sep:
    if not filesystem_policy:            # filesystem_policy 来自 --firmware-report
        parser.error('--development-no-sep requires --firmware-report')
```

`prepared-firmware.json` 不在交接包、也不在后续补的六个文件里。缺它就无法生成 no-SEP 模式的桥（即无法产出可启动镜像）。

**需要的文件**：原环境 `~/sagit/ios_sagit/build/apfs/prepared-firmware.json`（JSON，通常几十 KB～几 MB）。

拿到后剩余步骤约 2 分钟（bridge + boot，EDK2/kernelcache 已就绪）。

---

## 4. 主机侧

`tools/sagit-log-pull.py` 已升级：同时识别 `SAGITLG1`/`SAGITLG2`，v2 会打印 writer 与 reader 各自的几何并做比对，还会校验 body CRC32。`DEFAULT_PARTITION` 已修正为 `120410058752`。

后续修正：
- `decode_v2()` 少传 `partition_size` 导致 `NameError`（已修）。
- CRC 不一致重新定性为**撕裂读**（writer 先写 body 后写 header，抓在半途），
  读取器改为自动重读（`--retries`，默认 3），不再误报。
- 抓取完成后自动在**桌面**写 `<YYYY-MM-DD_HH-MM-SS>.txt`：内容是几何头 + **完整 ring**
  （不是控制台那 200 行），一次启动一份文件，便于按时间对比。控制台只打印 tail。
  相关参数：`--save-dir`（改目录）、`--no-save`（只打印）、`--tail`（打印行数）。
  OneDrive 重定向的桌面会自动识别。

---

## 5. 实机第一份日志读出的结论（2026-10-26 01:05）

`status=0x3 writes=75 errors=0 skipped=0`：写入器完全正常，旧的"窗口恒空"问题终结。

日志里两个持续故障：

| 现象 | 判读 |
|---|---|
| `Protobox: watchdogd(N) deny(1) iokit-open-service QualcommWatchdogTimer` → `Corpse allowed 1 of 5` → 10 秒后又一次 | watchdogd 打不开看门狗，被沙箱拒绝 → 退出 → launchd 重拉，**固定循环** |
| `disk1s1 gettin' low on space`，`free_blocks 8..652`（共 8192 块） | 32 MiB 的 RAM `/private/var` 被 logd 写满 |

`build-bridge.py` 已有 `--development-watchdog-policy`（`bridge_watchdog_policy.py`）：
只把 sandbox profile 里 `AppleARMWatchdogTimer` 这个 21 字节字面量换成
`QualcommWatchdogTimer`，守护进程、`IOWatchdogUserClient` 规则和权限检查全部保留。
**此前的重建脚本漏了这个参数**，这就是 denial 的来源。

---

## 6. var 容量的真实上限（重要）

想扩大 RAM `/private/var`，会撞上**三处**限制，不是一个：

| 位置 | 检查 | 原值 |
|---|---|---|
| `scripts/build-bridge.py` | `8 MiB <= len(data) <= 64 MiB` | 64 MiB |
| `scripts/build-bridge.py` | bridge Image `image_size <= 0x4000000` | 64 MiB |
| `boot/bridge/uefi.c` | `driver_bytes <= 64 MiB`，否则 `EFI_LOAD_ERROR` | 64 MiB |

而且 bridge Image **本身就有 29.7 MB**（内核 + DT + 代码），所以 32 MiB 的 var
已经把 Image 顶到 58.9 MiB——离上限只剩 5 MB。**不放开上限就几乎无法扩容。**

本轮已放开后两处（Image→192 MiB，EFI 预留→128 MiB）并重建 EDK2，
据此做出 96 MiB 的 var（`var96.apfs`，卷 UUID 不变，41 文件 `--verify` 通过）。

---

## 7. v4 → v5 → v6 的推进（2026-10-26 午后）

| 版本 | 改动 | 实机结果 |
|---|---|---|
| v3 | 日志写入器改走 `bdevvp`（避开 EBUSY） | 日志首次可读；watchdogd 被沙箱 deny |
| v4 | + `--development-watchdog-policy` | **deny 归零**；watchdogd 能开 timer，但开完 145 ms 就崩（`newUserClient` 只有 super，没实现 `IOWatchdogUserClient` 的 selector） |
| v5 | + var 32→96 MiB | **ENOSPC 51→0，low-space 156→0**；连续跑 10 分钟以上，进程从 5 个变一大批 |
| v6 | + `--full-memory` | 待测 |

### 内存是 v5 之后的新瓶颈

`available_pages: 800`（16 KiB 页 ≈ 12.8 MB），memorystatus 开始杀 logd。

根因：`device_trees/xiaomi-sagit.dts` 的 `memory@a0000000` **size 为 0**，由桥在运行时填充；
桥默认只交 **256 MiB arena**（`full_memory_requested=false`）。内核已导出
`_pmap_boot_memory_contains`，所以 `--full-memory` 可用——它会补
`boot-memory-ranges` / `-count` / `-bootstrap-end` 三个 DT 属性并定义
`BRIDGE_FULL_MEMORY=1`。

v6 = v5 + `--full-memory`，DT 里已核对到 3 处 `boot-memory-*`。

### 仍待解决

* `QualcommWatchdogTimer::newUserClient` 走的是通用 IOUserClient，watchdogd
  打开后第一次调用即崩。需要实现 `IOWatchdogUserClient` 的 external methods
  （setEnable / setTimeout 等）。

---

## 8. v6 结果与 v7（2026-10-26 13:20）

v6（`--full-memory`）实机日志 15–149 s：

* ✅ 内存问题解决：`memorystatus` 从刷屏降到 3 行，无 `System is unhealthy`、
  无 `killing_top_process`。
* ❌ watchdogd **不是自己好的**：

```
[68.182351]: watchdogd[114] Corpse failure, too many 6
[68.182427]: Process[114] crashed: watchdogd. Too many corpses being created.
```

崩溃预算 5 次用完，launchd 放弃它 —— 所以 v5 的 310 s 窗口里"看不到 watchdogd"
不是稳定，而是早已被判死刑。**上一轮判断已更正。**

v7 新增 `drivers/platform/QcomWatchdog/QualcommWatchdogUserClient.{h,cpp}`，
`newUserClient` 对 type==1 改走自己的 client（其它 type 仍走框架）：

* `externalMethod` 打印 selector / 标量出入参数 / 结构体大小 / 调用序号；
* 一律返回 success，并调一次 `resetWatchdog()`（`tick()` 每 5 s 仍独立喂硬件）。

目的是**取证**：拿到 watchdogd 真实的 selector 序列后再按语义实现，
避免猜测。Kext 内已确认 `_ZN26QualcommWatchdogUserClient14externalMethod...` 符号。



