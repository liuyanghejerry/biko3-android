# 将 Biko3 从 iOS 移植到 Android 的可行性调研

> 调研对象：本仓库 `biko3-ios`（iOS 26 / arm64）当前实现。
> 调研方式：静态阅读本仓库与上游 `seedds/nxnative`（pin `677345b0`）源码、在本机用 Linux/gcc 做可移植性实测、核对 Android/AOSP 平台事实。
> 未做的事：没有解密 `ci-inputs/guest.enc`（缺少 `BIKO3_GUEST_KEY`），因此没有跑通 guest；没有 Android 设备/NDK，因此没有在真机上验证。凡涉及"运行时是否可行"的结论，均标注为"需实测"。

---

## 1. 结论摘要

**技术可行，工作量明显小于从零重写，也小于当初的 iOS 移植（反向工程、改写链、hook 发现都已完成）；但"能不能跑"的关键风险在 Android GPU 驱动，而不在移植代码量。**

| 维度 | 结论 | 说明 |
| --- | --- | --- |
| CPU / 指令集 | ✅ 无问题 | guest 是 AArch64 NRO，Android `arm64-v8a` 可直接执行；仅放弃 x86_64 设备 |
| guest 加载 / 重定位 | ✅ 更容易 | Android 允许匿名可执行内存，不需要 iOS 那套"签名段 + `vm_remap`"方案，直接 `mmap`+`mprotect(RX)` 即可 |
| 静态改写 / hook | ✅ 完全复用 | `prepare_nro → rewrite_nro → patch_guest_hooks` 产出的 `main.patched.bin` 与 hook 表与平台无关，一行都不用重做 |
| 图形 | ✅ 更简单 | 游戏用的是标准 Vulkan + 自带 SPIR-V；iOS 需要 MoltenVK 翻译，Android 直接 `libvulkan.so`，零翻译 |
| 音频 | ✅ 易替换 | 目前只有 178 行 `AudioQueue` 传输层，换成 AAudio 即可 |
| 输入 | 🟡 需重写前端 | 虚拟手柄 429 行 ObjC + GameController → Kotlin + `MotionEvent/InputDevice`；host 侧 `dm_input_set`/`dm_touch_set` 接口不变 |
| 文件 / 存档 / 数据导入 | ✅ 基本可复用 | devoptab 层是纯 POSIX（已实测在 Linux 上零改动编译通过）；`.b3data` 解包逻辑改成 Kotlin/C |
| 生命周期 | ✅ 有对应物 | `onPause/onResume` → 现成的 `biko3_vk_suspend` / `biko3_audio_suspend` / `guest_clock_freeze_for` |
| 线程 / 优先级模拟 | 🟡 中等风险 | host 用 Apple QoS 模拟 Horizon 优先级；Android 无 QoS、无 `SCHED_FIFO`，只能用 nice 近似 |
| GPU 驱动兼容 | 🔴 最大风险 | Adreno / Mali / Xclipse 驱动差异、Vulkan 版本与扩展、Surface 生命周期，都是 iOS 上没有的变量 |
| 构建 / 分发 | ✅ 更便宜 | Linux + NDK 即可构建，无需 Mac/Xcode/开发者账号，APK 侧载无 7 天签名问题 |
| 工作量 | 约 **1.5–3 人月** 达成"能跑能玩"，其后是持续的设备兼容 | 见第 7 节阶段估算 |

一句话建议：**先做两周的"能启动"探索性验证（第 8 节），把 GPU 驱动和 Surface 两个风险点证伪或证实，再决定是否投入完整移植。**

---

## 2. 现有 iOS 实现是什么

这不是"把游戏源码重编译"，而是一套**原生宿主 + 静态改写的 guest**：

```
┌──────────────────────────────── 用户可见 ────────────────────────────────┐
│  UIKit 界面：资源导入页 / 16:9 CAMetalLayer / 虚拟手柄(DmGamepad.m)        │
├──────────────────────────────── iOS 适配层（ios/ 目录，~1500 行）────────┤
│  Biko3Vulkan.c  Vulkan→MoltenVK 桥（89 个 Vulkan 钩子）                  │
│  Biko3Libnx.c   libnx 启动 + 平台 poll/create/time（17 个 libnx 钩子）    │
│  Biko3Audio.c   audout → AudioQueue（48000Hz 立体声 s16，480 帧/块）      │
│  Biko3FS.c      devoptab 新实现（romfs:/save:/sdmc: → Documents 路径）    │
│  Biko3Hooks.c   启动时把 hook 表写进 guest 的 16KiB 数据页（不 JIT）      │
├────────────────────── nxnative 宿主核心（上游 C，~7700 行）──────────────┤
│  loader.c  读 NRO/NSO 镜像、RELA/RELR 重定位、段权限                   │
│  hle_svc.c / hle_thread.c / hle_os.c / hle_hid.c / hle_touch.c ...      │
│  即"把 Horizon 内核 + 平台服务用宿主 C 重新实现一遍"                     │
├────────────────────── 静态改写的 guest（离线生成）───────────────────────┤
│  Biko3 的 AArch64 NRO（sha256 31320a8c…）：                              │
│   svc #n、mrs/msr tpidr*、x18 访问 → 静态分支到 16KiB hook 表            │
│   106 个 hook（89 Vulkan + 17 libnx），13639 个 FDE，12952 个 RELR 目标  │
├──────────────────────────────────────────────────────────────────────────┤
│  MoltenVK 1.4.2（Vulkan→Metal）→ Metal → A18 GPU                         │
└──────────────────────────────────────────────────────────────────────────┘
```

关键事实（对移植判断影响最大）：

1. **guest 与平台解耦得非常干净。** 平台差异全部收敛在 106 个 hook 上：89 个 Vulkan 入口重定向到 MoltenVK，17 个 libnx 入口重定向到 `Biko3Libnx.c`/`Biko3Audio.c`。游戏本体一行不改。
2. **游戏用的是标准 Vulkan + 自带 SPIR-V 1.0。** [`ios/profile/shader_map.tsv`](../ios/profile/shader_map.tsv) 明确写着"NVN shader map is unused: original SPIR-V is passed to MoltenVK"，NVN/Metal 着色器翻译链对本作是死代码。这意味着 Android 上**没有任何着色器翻译工作**，原生 Vulkan 直接吃同一批 SPIR-V。
3. **Vulkan API 面很窄、很标准。** [`ios/runtime/vulkan-hooks.json`](../ios/runtime/vulkan-hooks.json) 里游戏**静态直接调用**的入口只有 core + `VK_KHR_surface`/`VK_KHR_swapchain` + `VK_NN_vi_surface` + `vkGetPhysicalDeviceProperties2`，没有 dynamic rendering、没有 timeline semaphore、没有 descriptor indexing、没有 mesh shader；用的是传统 render pass + `vkCmdPipelineBarrier`，SPIR-V 也是 1.0。注意：`vkCreateDevice` 里请求的**设备扩展与特性**无法从 hook 清单推出（动态查询走真实驱动），这一项列为风险 R6。
4. **不依赖 JIT。** 一切改写都在离线完成，运行时只写数据页。这让 Android 侧最麻烦的可执行内存问题直接消失。
5. **游戏数据与 IPA 分离**：169 个文件 / 2,982,621,688 字节，打成 6 个 `.b3data` 分卷，首次启动解包（见 [`docs/resource-packs.md`](resource-packs.md)）。
6. **当前 iOS 版本身的设备可玩性还没有闭环证据**：[`docs/port-status.md`](port-status.md) 明确写"不是已验证可玩的发布版"，只有 macOS arm64 的空资源启动 smoke。这一点直接影响 Android 的验证策略（见 7.3）。

---

## 3. 平台耦合点盘点（附实测数据）

### 3.1 实测方法

用本机 gcc（Linux/glibc）对上游 `dm/host/*.c` 逐个 `-fsyntax-only`，用本仓库 `ios/profile/game.tsv` 合成 `game_profile.h`。这不是 Android/bionic 的真实编译，但能**精确定位"哪一行依赖 Apple"**。

结果：**39 个 .c 文件，20 个原样通过，19 个失败。**

同时对本仓库 `ios/runtime/*.c` 做同样测试：`Biko3FS.c`、`Biko3Libnx.c`、`Biko3Input.c` **零改动通过**（`Biko3Audio.c` 预期失败，`Biko3Hooks.c`/`Biko3Vulkan.c` 依赖生成头/ Vulkan 头）。

### 3.2 失败文件分类

| 类别 | 文件（行数） | 是否 Biko3 路径需要 | Android 处理 |
| --- | --- | --- | --- |
| **必须改（8 个文件，2,368 行）** | `hle_thread.c` (957)、`loader.c` (460)、`hle_svc.c` (311)、`crash.c` (215)、`hle_os.c` (161)、`main.c` (135)、`hle.c` (102)、`hle_lifecycle.c` (27) | 是 | 见 3.3，Apple 调用点很少 |
| **不必移植（11 个文件，3,051 行）** | `hle_fs.c` (763)、`hle_movie.c` (758)、`hle_audio.c` (431)、`probe.c` (326)、`hle_sdk16.c` (261)、`hle_movie_avc.c` (143)、`hle_time.c` (134)、`hle_uemalloc.c` (75)、`hle_fpslock.c` (66)、`hle_count.c` (56)、`hle_fdstat.c` (38) | 否（零售 SDK 服务 / Apple 媒体 / 诊断） | 从 Android 构建中剔除；少量符号需补桩（见下） |
| iOS 前端 | `front_mac.m`、`memstat.m`（334 行） | 否 | 用 Android 前端替代 |
| 图形（可选） | `gfx/nvn/*.c`（`hle_nvn.c` 等） | 只有 `dm_gfx_suspend` 一个符号被前端调用 | 保留 `hle_nvn.c` 并改掉 1 处 `mach_time`，或直接提供空实现 |

剔除这些文件时要补的桩很少，但必须补：`front_save_failed()`（原在 `hle_fs.c` 里的 weak 实现）、`dm_audio_suspend()`/`dm_audio_reset()`（`hle_oe.c` 的 env 脚本路径会调）、`dm_gfx_suspend()`。都是十几行的空实现或转发到 `biko3_*`。

**一个需要留意的例外：游戏数据里有 1 个 78.4 MiB 的 `.mpg`**（另有 23 个 `.pp` 共 2.6 GiB、29 个 `.atr` 共 118 MiB）。如果片头动画由游戏自带的软件解码器播放，则与平台无关；如果它走的是 Switch 的视频服务（MVI/avi），那么 `hle_movie*` 就不能简单剔除，Android 侧还要接 `MediaCodec`——这是额外的大工程。iOS 版把 `hle_movie.c` 编进去了，但那是零售 SDK 的 `nn::movie`，与 libnx 的 MVI 不是一回事，**这一项必须实测**（播放片头时看日志与画面）。

**结论：真正需要动的是 8 个文件、2,368 行里的"平台相关一小部分"**（每个文件里 `mach_*`/`sysctl`/`dispatch` 调用通常是 1–15 处）。不是"7700 行 C 都要重写"。

### 3.3 逐项耦合点与 Android 对应物

| iOS 依赖 | 位置 | Android 方案 | 难度 |
| --- | --- | --- | --- |
| `vm_remap` 把签名段拼回原布局 | `loader.c` 的 `#if TARGET_OS_IPHONE` 分支 | 直接走 `#else` 分支：读文件 → `mmap(RW, ANON)` → 拷贝 → `mprotect(RX)` | 低（该分支已是纯 POSIX） |
| `getsectiondata` / `__DATA,swhle` / `__DATA,swforce` / `__TEXT,__dmmainx0` | `hle.c`、`loader.c`、`hle.h`、`hle_table.c`、`hle_bigstack.c` | ELF section + `__start_/__stop_`，或对 Biko3 直接留空表（本作不用 HLE_FORCE） | 低–中 |
| `entry.S` 的 `_dm_gate` / `_unimpl_stubs`、`.private_extern`、`@PAGE/@PAGEOFF` | `dm/host/entry.S`、`hle.h` | ELF 符号命名、`adrp/add` 标准重定位 | 低 |
| `pthread_setspecific` 当 guest TLS 用（Apple TSD = `tpidrro_el0 + 8*key`） | `loader.c install_key_page`、`thread.c`、`hle_thread.c` | **见 3.4**，改为自建 guest TP 块 + 复用 `tpidrro_el0` | 中 |
| `pthread_set_qos_class_self_np`（Horizon 优先级模拟） | `hle_thread.c` | `setpriority`/`nice` 近似；不能提优先级 | 中 |
| `thread_info` / `thread_get_state` / `thread_suspend` / `vm_read_overwrite`（CPU 时间、线程状态、崩溃时抓别的线程） | `hle_thread.c`、`crash.c`、`probe.c`、`hle_fs.c`、`hle_fpslock.c` | `clock_gettime(CLOCK_THREAD_CPUTIME_ID)`、`/proc/self/task/*`；跨线程栈 dump 放弃 | 中 |
| `pthread_get_stackaddr_np` | `hle_svc.c`（QueryMemory 报告主线程栈） | `pthread_getattr_np` + `/proc/self/maps` | 低 |
| `mach_absolute_time` / `clock_gettime_nsec_np` | `hle_os.c`、`hle_time.c`、`nvn_*.c` | `clock_gettime(CLOCK_MONOTONIC)` | 低 |
| `sysctl(KERN_PROC)` 取进程启动时间 | `main.c` | `/proc/self/stat` 第 22 字段 | 低 |
| `dispatch_after` / `dispatch_source`（优雅退出） | `hle_lifecycle.c` | `timerfd`/`poll` 或 `sigaction` + 线程 | 低 |
| `AudioQueue`（48000/2/s16） | `ios/runtime/Biko3Audio.c` | AAudio（API 26+）+ 自建环形缓冲 | 低 |
| `CAMetalLayer` → `VK_EXT_metal_surface` | `Biko3Vulkan.c` + `front_ios.m` | `ANativeWindow` → `VK_KHR_android_surface` | 低–中 |
| `VK_KHR_portability_*` 适配、MoltenVK 静态库 | `Biko3Vulkan.c`、`stage_ios.py` | 全部删除 | 负工作量 |
| `CommonCrypto`（数据校验） | `ios/import/Biko3ResourceInstaller.m` | NDK BoringSSL 或 Kotlin `MessageDigest` | 低 |
| UIKit / GameController / CoreMotion | `front_ios.m`、`DmGamepad.m`、`main.m` | Activity/SurfaceView + `MotionEvent`/`InputDevice`/`SensorManager` | 中（纯 UI 工作量） |

### 3.4 唯一需要"设计"而不是"翻译"的点：guest TLS slot

现状机制（[`docs/graphics-bridge.md`](graphics-bridge.md) 与上游 `dm/tools/patch_nso.py`）：

- 改写器把 guest 里的 `mrs tpidrro_el0` / `mrs tpidr_el0` / x18 访问，替换成"从 `tpidrro_el0` 加上一个偏移取值"；
- 偏移来自 guest 镜像里一张可写"key page"（改写器预留 16 KiB 段，索引 508–511），**由宿主在加载时写入**；
- iOS 上宿主写的是 `pthread_key_t * 8`，因为 Darwin 的 TSD 恰好就是 `TP + 8*key`。

Android 上这套语义不成立：bionic 的 `pthread_getspecific` 读的是 `__get_bionic_tls().key_data[...]`（[`pthread_key.cpp`](https://raw.githubusercontent.com/aosp-mirror/platform_bionic/main/libc/bionic/pthread_key.cpp)），不是 TP 相对寻址；而且 bionic 的 `__get_tls()`/`__set_tls()` **只读写 `tpidr_el0`**，从不写也不读 `tpidrro_el0`（[`libc/platform/bionic/tls.h`](https://raw.githubusercontent.com/aosp-mirror/platform_bionic/main/libc/platform/bionic/tls.h)，已对照 AOSP main 核对）。

于是有一条很便宜的解法：

1. key page 里写入**固定偏移**（例如 0/8/16/24），不再写宿主 key 值；
2. 宿主为每个 guest 线程分配 64 字节的"guest TP 块"，把 `tpidrro_el0` 指向它（EL0 可写）；
3. 宿主自己那 32 处 `pthread_setspecific/getspecific`（`hle_thread.c` 21、`thread.c` 5、`loader.c` 2、`hle_bigstack.c` 2、`crash.c` 2）改读写这个块。

这样**离线改写产物完全不变**（不需要重跑 hash 绑定的改写链），只在宿主侧改约 50 行。风险是"依赖 bionic 不碰 `tpidrro_el0`"，需要在真机上做一次确认实验（见第 8 节 E2）。

> 这是一个"看起来玄、实际很小"的移植点，但必须最先验证——它是 guest 能不能起来的前提。

---

## 4. Android 侧技术选型对照

| 层 | iOS 现状 | Android 选型 | 说明 |
| --- | --- | --- | --- |
| 图形 API | Vulkan → MoltenVK → Metal | **Vulkan 直连** `libvulkan.so` | 删掉 MoltenVK、`VK_KHR_portability_subset`、`VK_EXT_metal_surface` 别名逻辑 |
| 窗口 | `CAMetalLayer` | `SurfaceView` + `ANativeWindow` + `VK_KHR_android_surface` | 16:9 letterbox：`ANativeWindow_setBuffersGeometry` 或 SurfaceView 定比 |
| 着色器 | 原样 SPIR-V → MoltenVK 翻译 | 原样 SPIR-V → 驱动 | **零工作量**，也是本作最适合 Android 的地方 |
| 音频 | `AudioQueue`（480 帧/块，5 ms） | **AAudio**（`LOW_LATENCY`，48000/2/s16）+ 环形缓冲 | iOS 已踩过"20 ms 在飞 + 21 ms 回调 → 每秒 47 次欠载"的坑，Android 应默认加 50–200 ms 抖动缓冲 |
| 输入（物理） | `GameController.framework` | `InputDevice` + `KeyEvent`/`MotionEvent`（`SOURCE_GAMEPAD`） | host 端 `dm_input_set()` 位图布局不变（上游 `dm/host/dm_input.h`，与 [`Biko3Input.c`](../ios/runtime/Biko3Input.c) 对接） |
| 输入（虚拟手柄） | `DmGamepad.m`（429 行，CAShapeLayer 绘制） | Kotlin 自绘 View / Canvas | 布局常量与触控所有权逻辑可逐条照搬 |
| 触摸 | `dm_touch_set()` | 同接口，归一化坐标 | 已平台无关 |
| 文件 / 存档 | `Documents/` + Files App | app-specific external dir + SAF | 路径策略同 iOS，`BIKO3_GAME_ROOT/SAVE_ROOT` 环境变量机制保留 |
| 数据导入 | `Biko3ResourceInstaller.m`（444 行） | Kotlin（SAF）或 C（逐文件 SHA-256） | 分卷格式与清单完全复用 |
| 生命周期 | `UIScene` 四个回调 | `onResume/onPause/onStop/onDestroy` | 调用同一组 `biko3_vk_suspend`/`biko3_audio_suspend`/`guest_clock_freeze_for` |
| 构建 | xcodegen + xcodebuild（需 macOS） | **Gradle + CMake/NDK**（Linux/Windows 均可） | `stage_ios.py` 只需换 Xcode 段 → CMake 源列表 |
| CI | `macos-26` runner | `ubuntu-latest` + NDK | 成本与排队时间都显著下降 |
| 分发 | 未签名 IPA，侧载（7 天/1 年签名限制） | 侧载 APK（debug 签名即可） | 无证书周期问题 |

**最小 SDK / ABI 建议**：`minSdk 26`（AAudio）、`abiFilters "arm64-v8a"`、`targetSdk 35`、`compileSdk 35`；CMake 用 NDK r28+ 并设 `-Wl,-z,max-page-size=16384`（见第 5 节第 4 条）。

---

## 5. 为什么这一例上 Android 反而更容易

1. **图形从"翻译"变成"直连"。** iOS 需要 MoltenVK：扩展列表要镜像、要 `VK_KHR_portability_subset`、要 `VK_EXT_metal_surface`；这些逻辑在 Android 上整段删除。同时去掉 SPIR-V→MSL 的翻译与管线缓存开销。
2. **可执行内存限制消失。** iOS 不允许运行时生成代码，才有了 [`gen_embed.py`](https://github.com/seedds/nxnative) 那套"把 guest 拆成三段塞进签名二进制的 `__TEXT`，再用 `vm_remap` 拼回原布局"的做法。AOSP `private/app.te` 里有 `allow appdomain self:process execmem;`（[来源](https://android.googlesource.com/platform/system/sepolicy/+/refs/heads/main/private/app.te)），即应用可以对匿名内存 `mprotect(PROT_EXEC)`。Android 上 guest 可以直接 `mmap` 匿名页再 `mprotect`，整套签名段机制不需要。
3. **C ABI 更接近 Horizon。** 上游 `hle.h` 专门写了 Apple ABI 差异（Apple 要求调用方扩展 `<32` 位参数；Apple 的可变参数走栈）以及由此产生的补丁。Android 的 AAPCS64 与 Switch 更接近：可变参数走寄存器、小整数不保证扩展——反而**减少**了 ABI 适配面。
4. **页面粒度天然吻合。** 现有镜像按 16 KiB 对齐（`0x4000`），既能跑 4 KiB 页内核 Android，也正好满足 Android 15+ 的 16 KB 页面设备（[Android 页面大小兼容指南](https://developer.android.com/guide/practices/page-sizes)）。
5. **构建与分发门槛低。** 不需要 Mac、Xcode、开发者账号；CI 用 ubuntu 即可；APK 侧载没有 7 天重签问题。

---

## 6. 风险登记表

| # | 风险 | 严重度 | 可能性 | 依据 / 影响 | 缓解 |
| --- | --- | --- | --- | --- | --- |
| R1 | **GPU 驱动兼容**（Adreno / Mali / Xclipse 的 Vulkan 实现差异、管线/描边/内存类型 bug） | 🔴 高 | 中–高 | 这是 iOS 用 Metal（MoltenVK 已高度收敛）时不存在的一整类问题；Unity 在 Android 上的 Vulkan 路径本来就以"机型碎片化"著称 | 先用 2–3 个 GPU 家族实测；把 `vkCreateDevice` 请求的扩展/特性全部打日志；必要时在桥里过滤不必要扩展 |
| R2 | **Surface 生命周期**：`surfaceDestroyed` 后 `VkSurfaceKHR` 失效，游戏自己的 swapchain 不一定会重建 | 🔴 高 | 中 | iOS 的 `CAMetalLayer` 不会因为切后台被销毁；Android 会。游戏是 Switch 版 Unity 的 Vulkan 后端，是否处理 `VK_ERROR_SURFACE_LOST_KHR` 未知 | 在桥里做"**假 NN surface 句柄**"：对外返回稳定句柄，内部映射到当前真实 `VkSurfaceKHR`；surface 重建后让 `vkAcquireNextImageKHR`/`vkQueuePresentKHR` 返回 `VK_ERROR_OUT_OF_DATE_KHR`，迫使游戏重建 swapchain。约 150–250 行 |
| R3 | **线程优先级模拟**：Android 应用不能 `SCHED_FIFO`，也没有 Apple QoS | 🟡 中 | 中 | 上游 `hle_thread.c` 用 QoS + 同核让让（`dm_yield_to_core_peers`）模拟 Horizon 调度，某些游戏依赖它（注释提到 Metroid Dread） | 用 nice 近似；`dm_yield_to_core_peers` 改成 `/proc/self/task/*/stat` 判定；Biko3 是普通 Unity 线程模型，敏感度大概率低 |
| R4 | **音频欠载/爆音** | 🟡 中 | 中 | 游戏只在飞 4×5 ms；Android 端到端延迟通常比 iOS 高 | 加 50–200 ms 环形缓冲，监控 AAudio xRun（`AAudioStream_getXRunCount`） |
| R5 | **guest TLS slot 方案**（3.4）需要验证 bionic 不碰 `tpidrro_el0` | 🟡 中 | 低 | 若不可行，需退回"改写器生成调用桩"，代价大得多（要重跑 hash 绑定改写链） | 先做 10 行汇编级 spike（第 8 节 E2） |
| R6 | **游戏请求的 Vulkan 版本/扩展/特性未知** | 🟡 中 | 中 | 没有 guest 二进制就无法静态确认 `vkCreateDevice` 的请求内容 | 在桥里完整记录 `VkInstanceCreateInfo` / `VkDeviceCreateInfo`；必要时做"能力协商"适配 |
| R7 | **内存 / 后台被杀 / 散热** | 🟡 中 | 中 | host 会预留 4 GiB `PROT_NONE`（地址空间，不占物理），默认堆 512 MiB（`BIKO3_HEAP_MB`），外加 Vulkan 显存；Android 低内存设备（4–6 GB）风险高 | 目标 8 GB+；堆大小做成可调；`onTrimMemory` 时暂停 |
| R8 | 仅 `arm64-v8a` | 🟢 低 | — | guest 是 AArch64 原生码，x86_64 设备/模拟器无法执行 | 明确声明只支持 arm64；CI 不做 x86 构建 |
| R9 | 数据导入 UX：Android 11+ 限制了 `Android/data` 的文件管理器访问 | 🟡 中 | 高 | 用户"把 6 个分卷拷进 App 目录"这一步在 Android 上不如 iOS 的 Files App 直观 | 三条路：SAF 导入器（推荐）；`adb push` 文档；可选申请 `MANAGE_EXTERNAL_STORAGE` 从 Downloads 直接读 |
| R10 | **iOS 基线本身未被证明可玩** | 🟡 中 | — | [`docs/port-status.md`](port-status.md)：只有 macOS 空资源启动证据；游戏是否真的跑得起来（尤其 Vulkan 管线、着色器）还没有设备证据 | Android 验证应先以"能启动 + 能出首帧"为目标，而不是"复刻已验证行为"；若 iOS 真机可玩，则 Android 的风险大幅下降 |
| R11 | 上游 `nxnative` 无 Android 分支，本项目要自维护一份平台 patch | 🟡 中 | 高 | 上游 pin 在 `677345b0`；将来升级上游会冲突 | 把 Android 改动收敛成"新增文件 + `#ifdef __ANDROID__` 小块"，并保留 pin 校验（沿用现有 `references.lock.json` 思路） |
| R12 | **片头/过场视频路径未知**：数据里有 1 个 78.4 MiB 的 `.mpg` | 🟡 中 | 低–中 | 若游戏自带软件解码器 → 无影响；若走 Horizon 视频服务（MVI/avi）→ 需要在 host 里补服务 HLE，并在 Android 接 `MediaCodec`。另外上游 `hle_movie.c` 自己的注释写着"the host does not play movies yet, opening fails" | 播放片头时抓日志确认；最坏情况先跳过视频（去掉/空实现），不阻塞主流程 |

---

## 7. 工作量估算与阶段计划

### 7.1 代码量估算

| 工作项 | 新增/改动 | 说明 |
| --- | --- | --- |
| host 平台层（loader/svc/thread/os/crash/hle/entry.S/lifecycle） | 改 ~400–700 行 | 以 `#ifdef __ANDROID__` 小块 + 新增 `dm/host/posix_*` 适配文件为主 |
| ELF/POSIX 版 section 与 entry | 新增 ~250 行 | 替代 Mach-O 段机制 |
| Vulkan 桥 Android 化（surface/扩展/swapchain 失效） | 改 ~250 行 | `Biko3Vulkan.c` 约 283 行，改动集中在后半 |
| 音频（AAudio + 环形缓冲） | 新增 ~220 行 | 替换 `Biko3Audio.c` |
| Android App 壳（Activity/SurfaceView/JNI/日志/生命周期） | 新增 ~500–700 行 Kotlin/C | |
| 虚拟手柄 | 新增 ~700–900 行 Kotlin | 对应 429 行 ObjC，逻辑可照搬 |
| 资源导入器 + 首次启动 UI | 新增 ~400–600 行 | 清单/分卷/SHA-256/断点续解 |
| 构建系统（CMake+Gradle+清单） | 新增 ~300 行 | |
| 打包工具 `stage_android.py`（基于 `stage_ios.py` 改造） | 改/新增 ~300 行 | 复用 NRO 准备/改写/hook 全流程 |
| CI（ubuntu + NDK） | 新增 ~120 行 | |
| **合计** | **约 3,500–5,000 行新代码，复用既有逻辑 ~1,500 行** | |

### 7.2 里程碑

| 阶段 | 目标 | 关键验收 | 估时 |
| --- | --- | --- | --- |
| **M0** | Linux/arm64 宿主无窗口 bring-up | guest 打印 `original libnx init completed` + `entering original main`，新 libc 文件回调命中；等价于现有 `mac_guest_smoke.py` | 1.5–2.5 周 |
| **M1** | APK 内加载 guest（无图形） | 同上指标，日志可从 `logcat` 导出；设备上 TLS/SVC/线程 稳定 | 1–1.5 周 |
| **M2** | 出首帧 | `Renderer ready:` + 连续 10 秒稳定帧；`vkCreateSwapchainKHR`/present 正常 | 1.5–2.5 周（含 R1/R2 风险） |
| **M3** | 可操作 | 虚拟手柄 + 实体手柄 + 触摸 + 音频 + 存档读写 | 2–3 周 |
| **M4** | 首次启动体验 | 6 分卷导入、校验、断点续解、错误提示、进度保留 | 1–1.5 周 |
| **M5** | 性能/兼容/发布 | 2–3 个 GPU 家族通过；温度与帧率曲线；崩溃日志可回收；APK 与资源发布流程 | 2–4 周 + 长期 |
| | **合计** | | **约 9–14 周单人** |

### 7.3 与 iOS 基线的依赖关系

- 若 iOS 真机已经能玩：Android 只需复现同一套行为，M2/M3 估时可以取下限。
- 若 iOS 也还没在真机上跑通：Android 移植会变成"两台设备并行 bring-up"，不确定性叠加。**建议先把 iOS 真机的首帧与操作验证做完**（成本最低的一次验证），再启动 Android。

---

## 8. 最小验证实验（建议按顺序做，2 周内出结论）

| # | 实验 | 目的 | 通过标准 |
| --- | --- | --- | --- |
| **E1** | 用 NDK 把 host 的 8 个关键文件（外加 `entry.S`、`hle.h`）按 `aarch64-linux-android26` 编一遍（只 `-c`） | 量化真实报错，验证 3.2 的分类 | 报错全部落在已知 Apple API 上；无新增结构性问题 |
| **E2** | 在 Android 上跑 10 行汇编 spike：设置 `tpidrro_el0` → 调 bionic 函数 → 读回 | 验证 3.4 的 TLS 方案 | `tpidrro_el0` 保持我们写的值，在 bionic 调用（malloc/errno/线程创建）之后仍稳定 |
| **E3** | 用 NDK 把 host 编成 **bionic 链接**的 `aarch64-linux-android26` 命令行程序，`adb push` 到 `/data/local/tmp` 直接跑（shell 域，不需要 Java/APK） | 在真机上验证 loader/relocation/SVC/线程/文件；把"native 移植"和"Android 应用壳"两件事彻底分开 | 出现 `original libnx init completed`（GPU 属 shell 域常见不可用，图形失败可预期） |
| **E4** | 最小 APK：JNI + SurfaceView + 只有桥的 `vkCreateInstance`/`vkCreateViSurfaceNN`/swapchain 清屏 | 验证 R1/R2：驱动能不能创建设备、surface、swapchain 并连续 present | 三个 GPU 家族各跑到 60 秒无 `DEVICE_LOST` |
| **E5** | 在 E4 的 APK 里接上真实 hook 表和 `main.patched.bin` | 验证游戏 Vulkan 路径 | 出首帧，日志无 pipeline 失败 |

E3/E4 的价值在于：**在写任何 Kotlin UI 之前就能判断这条路能不能走通**。

另外这些现有资产可直接复用：

- `tools/prepare_nxnative.py`、`tools/rewrite_nro.py`、`tools/patch_guest_hooks.py`、`tools/patch_libnx_host.py`：与平台无关；
- `tools/game_shader_smoke.py` + `tools/extract_embedded_spirv.py`：可离线反射 SPIR-V 的 capability/扩展，提前判断 R6；
- `tests/native/vulkan_smoke.c`：改成 Android 版即可在设备上做真 GPU compute 验证（含 suspend/resume 语义）；
- `tools/resource_import_smoke.py` 的测试矩阵（跨卷、截断、伪造摘要等）：验证 Android 导入器实现是否等价。

---

## 9. 备选方案对比

| 方案 | 优点 | 缺点 | 建议 |
| --- | --- | --- | --- |
| **A. 原生移植（本文主体）** | 性能最好；复用现有反向工程成果与文件/Vulkan 层逻辑；无模拟开销 | 需要写 Android 前端；驱动兼容要打磨 | ✅ 推荐 |
| B. 用现成 Switch 模拟器（Eden / Sudachi / Ryujinx-Android 等）跑正式版 | 不用写移植代码 | 需要完整 NSP + 密钥，与当前"只发布 NRO + 自打包 Data"的合规路径冲突；CPU 全模拟，性能与耗电差一档；对 homebrew NRO 支持不完整 | ❌ 不推荐 |
| C. 云游戏 / 串流 | 手机端零适配 | 需要服务器与网络；延迟 | 与本目标无关 |

方案 A 还有一个隐性优势：**当前的 guest 镜像与 hook 表是 hash 绑定的**，说明这条链已经过严格验证；Android 只是换一个"宿主后端"，比换模拟器内核风险低得多。

---

## 10. 合规与分发

- 与 iOS 相同：APK 只包含宿主代码 + 加密/嵌入的 guest 可执行体，**不包含游戏数据**；数据仍由用户自备并通过分卷导入。
- Android 侧载不经过 Google Play，因此不受 Play 政策约束；但**不得公开分发游戏本体或 Data**。
- 第三方许可沿用现状：nxnative(MIT)、MoltenVK(Apache-2.0) 在 Android 上不再需要；需要新增的是 NDK/AAudio（平台自带）、以及（若用）BoringSSL（Apache-2.0）。
- `references.lock.json` 的 pin 机制建议保留，Android 的宿主改动以 patch 集形式记录摘要。

---

## 11. 建议的下一步

1. **确认基线**：先在真机确认 iOS 版能出首帧、能操作（这是最便宜的一次风险消除）。
2. **做 E2 + E3（约 2–3 天）**：验证 `tpidrro_el0` 复用方案（E2）与 Android 上原生 host bring-up（E3）。这两项决定"可行性"，而不是"工作量"。
3. **做 E4（约 3–5 天）**：三个 GPU 家族的最小 Vulkan 表面验证，决定 R1/R2 的量级。
4. 三项都通过后，再按 M0–M5 立项；否则优先解决驱动/Surface 方案，而不是先写 UI。

如果只想要一句判断：**这条路的代码工作量是可控的（1.5–3 人月），真正的不确定性集中在 Android GPU 驱动与 Surface 生命周期；先花两周做 E2–E4 就能把这两个不确定性压到可决策的程度。**

---

## 附录 A：本报告引用的关键位置

| 主题 | 位置 |
| --- | --- |
| iOS 移植现状与未验证项 | [`docs/port-status.md`](port-status.md) |
| Vulkan→Metal 桥、hook 覆盖、静态改写 | [`docs/graphics-bridge.md`](graphics-bridge.md) |
| libnx 启动、devoptab、音频、SVC 约定 | [`docs/libnx-startup.md`](libnx-startup.md) |
| NRO 准备与 FDE 恢复 | [`docs/nxnative-preparation.md`](nxnative-preparation.md) |
| 资源分卷格式与解包 | [`docs/resource-packs.md`](resource-packs.md) |
| 89 个 Vulkan hook | [`ios/runtime/vulkan-hooks.json`](../ios/runtime/vulkan-hooks.json) |
| 17 个 libnx hook | [`ios/runtime/libnx-hooks.json`](../ios/runtime/libnx-hooks.json) |
| Vulkan 桥实现 | [`ios/runtime/Biko3Vulkan.c`](../ios/runtime/Biko3Vulkan.c) |
| 音频传输 | [`ios/runtime/Biko3Audio.c`](../ios/runtime/Biko3Audio.c) |
| devoptab 实现 | [`ios/runtime/Biko3FS.c`](../ios/runtime/Biko3FS.c) |
| 资源导入（ObjC） | [`ios/import/Biko3ResourceInstaller.m`](../ios/import/Biko3ResourceInstaller.m) |
| 构建管线 | [`tools/build_ios.py`](../tools/build_ios.py)、[`tools/stage_ios.py`](../tools/stage_ios.py) |
| 上游宿主（Apple 依赖点） | `seedds/nxnative@677345b0`: `dm/host/loader.c`、`hle_thread.c`、`hle_svc.c`、`entry.S`、`dm/ios/Sources/*` |

## 附录 B：外部事实来源

- AOSP SELinux `private/app.te`：`allow appdomain self:process execmem;` — <https://android.googlesource.com/platform/system/sepolicy/+/refs/heads/main/private/app.te>
- bionic `libc/platform/bionic/tls.h`（`__get_tls`/`__set_tls` 只用 `tpidr_el0`）— <https://raw.githubusercontent.com/aosp-mirror/platform_bionic/main/libc/platform/bionic/tls.h>
- bionic `libc/bionic/pthread_key.cpp`（TSD 存在 `bionic_tls.key_data`，非 TP 相对寻址）— <https://raw.githubusercontent.com/aosp-mirror/platform_bionic/main/libc/bionic/pthread_key.cpp>
- Android 16 KB 页面大小兼容性 — <https://developer.android.com/guide/practices/page-sizes>
- Android Vulkan 概览 — <https://developer.android.com/games/develop/vulkan/overview>

## 附录 C：术语

- **guest**：被执行的原始 Switch AArch64 程序（Biko3 NRO）。
- **host**：nxnative 宿主，负责加载、重定位、Horizon 内核/服务 HLE。
- **hook 表**：改写器在 guest 镜像 `0xfac000` 处预留的 16 KiB 可写指针表；槽 0–1023 给 Vulkan，1024+ 给 libnx。
- **key page**：改写器预留的 16 KiB 页，保存 guest TLS slot 的偏移，由宿主加载时写入。
- **gate**：把 `svc #n`、`mrs cntpct_el0` 改写成 `bl` 到宿主 `dm_gate` 的桩。
