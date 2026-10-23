# Biko3 Android 移植

在 Android 上原生运行《尾行 3》的移植。复用 `ios/` 已有的 guest 静态改写、nxnative 宿主内核与
Vulkan 桥，只替换平台层（窗口、音频、输入、文件、生命周期）。

## 当前状态（2026-10-23）

| 项目 | 状态 | 证据 |
| --- | --- | --- |
| APK 构建 | ✅ | `tools/build_android.py` 直接调用 aapt2/javac/d8/apksigner，无需 Gradle |
| 应用启动 + JNI + 日志 | ✅ | x86_64 API 36 模拟器，`logcat -s Biko3` |
| guest TLS slot | ✅ | 每线程克隆 TLS 块，模拟器/真机自检均通过 |
| guest 镜像校验 | ✅ | `image: 16678912 bytes, NRO0 header, build id 023e…` |
| 加载 / RELR 重定位 / 106 个 hook | ✅ | `[dm] main: reloc … relr=12952`、`[biko3] bound 106 hooks` |
| Vulkan 直连（instance/device/surface/swapchain/提交/呈现） | ✅ | 自检 `vulkan: 8 frames presented through the bridge`；游戏 `Renderer ready: 1280x720, 3 images` |
| 音频（AAudio） | ✅ 启动 | `native audio AAudio 48000 Hz stereo signed16`、`audio stream started` |
| 应用内资源导入 | ✅ | API 30 模拟器：6 卷校验通过、169 个文件展开（2.7 GB）、自动进入游戏 |
| 自动冒烟 `tools/android_smoke.py` | ✅ | `self_test_passed`、`guest_boot_started`、`renderer_ready`、`game_running` 均为 true |
| 触摸 | ✅ | 触摸点移动游戏内光标（坐标限制在 16:9 画面矩形内，黑边不误触） |
| 横屏 + 大号手柄 | ✅ | arm64 真机横屏，面键 60 dp、方向键 52 dp，触摸+A 可进入角色选择 |
| 手柄 | ✅ | 虚拟手柄/实体键到达 guest HID（`hid fill buttons 0x1`），A 确认菜单、场景切换 |
| **guest 启动到渲染器就绪** | ✅（模拟器） | `libnx init completed` → `entering original main` → `Renderer ready: 1280x720, 3 images` |
| **进入标题画面并持续渲染** | ✅（模拟器） | `Original menu scene loaded`，`PERF 60.0fps ... draws=10.0` 持续输出，截图 `work/android/shot-final.png` |
| **标题菜单进入游戏** | ✅（模拟器） | 触摸选到「スタート」后按 A：`Scene state shown=01` → `50` → `38`，进入角色选择画面，截图 `work/android/shot-opening.png` |
| 虚拟手柄 / 数据导入 UI / 真机验证 | ✅ | 虚拟手柄（dp 布局）、应用内导入、arm64 真机运行均已验证 |

> x86_64 模拟器没有 ARM 硬件，Android 自带的 `libndk_translation`（berberis）会把本 APK 的
> arm64 原生库翻译后执行；真实 arm64 手机上不需要这层翻译。第一次启动的 `Data` 需要另行导入，
> 见[资源分卷](resource-packs.md)。

## 目录

```
android/
  app/                       Java Activity + JNI 声明 + Manifest
  port/
    include/                 Darwin API 垫片、dm_tsd.h、ucontext 适配
    src/
      android_shims.c        mach_* / CommonCrypto / getsectiondata / sysctl 等
      dm_tsd.c               guest 线程槽（TP 相对布局）
      android_stubs.c        未移植的零售 SDK 模块空实现
      biko3_android.c        front.h、JNI、窗口/生命周期/日志
      biko3_selftest.c       TLS / 镜像 / 文件 / Vulkan 自检
      Biko3AudioAndroid.c    audout → AAudio
      entry_stub_x86_64.c    非 AArch64 构建的入口桩（guest 无法执行）
tools/
  build_android.py           APK 构建（NDK + build-tools，无 Gradle）
  stage_android.py           宿主复制/打补丁 + 生成头 + 资源
  patch_android_host.py      loader/crash/hle_thread/hle 的 Android 补丁
  patch_android_guest_image.py  guest 镜像 tpidrro → tpidr 寻址改写
  extract_ios_guest.py       从公开 IPA 还原 main.patched.bin 与 TSV 元数据
```

## 构建与运行

```sh
source ~/.local/share/firered-mobile/android-env.sh     # JDK / SDK / NDK
python3 tools/build_android.py --abi arm64-v8a          # 真机
python3 tools/build_android.py --abi x86_64 --install --run   # x86_64 模拟器（ARM 翻译）
```

产物：`work/android/Biko3-android-dev.apk`。日志：
`/storage/emulated/0/Android/data/io.github.hyc1743.biko3/files/log.txt`。

游戏数据（首次启动前，任选其一）：

```sh
# A. 应用内导入：把六个分卷推进 ResourcePacks，打开应用后自动校验并展开
adb push Biko3Data.part00*.b3data \
    /storage/emulated/0/Android/data/io.github.hyc1743.biko3/files/ResourcePacks/

# B. 电脑侧展开后直接推送 Data 目录
python3 tools/pack_resources.py restore --parts <六个 .b3data 的目录> \
    --manifest ios/resources/Biko3Data.manifest.json --out work/android/data
adb push work/android/data/Data \
    /storage/emulated/0/Android/data/io.github.hyc1743.biko3/files/game/
```

应用内导入复用 IPA 内置清单：先校验六卷的 SHA-256，再按 segment 还原 169 个文件，
每个文件写到 `.partial` 后校验自身摘要再改名。已在 API 30 模拟器上实测：6 卷校验通过、
169 个文件展开（2.7 GB）后自动进入游戏。

### 发布版（真机安装）

```sh
python3 tools/build_android.py --abi arm64-v8a --release
adb install -r work/android/Biko3-android-release.apk
```

发布版与开发版的差别：

* 清单去掉 `android:debuggable`，`run-as` 不可用，状态叠加层自动隐藏；
* 用独立的发布密钥签名（首次构建自动生成 `~/.android/biko3-release.keystore`，
  alias `biko3`，口令 `biko3release`）；**这个密钥要保留**，否则后续升级会因签名不同而装不上；
* 原生库用 `llvm-strip --strip-debug` 去掉调试段，APK 约 4.0 MB。

真机首次启动：把六个分卷推进 `Android/data/io.github.hyc1743.biko3/files/ResourcePacks/`，
应用检测到分卷后自动校验并展开（约 2.7 GB），完成即进入游戏。

### 自动冒烟

```sh
python3 tools/android_smoke.py --serial emulator-5592
```

构建、安装、启动，并从应用的状态文件（`run-as` 可读）与宿主日志判定
`self-test: passed` / `booting the original guest` / `Renderer ready`，输出 JSON。

## 关键设计

### 1. guest 镜像：不再嵌入签名段

iOS 必须把 guest 放进 `__TEXT` 再用 `vm_remap` 拼回原布局。Android 允许应用对匿名页
`mprotect(PROT_EXEC)`，因此 loader 直接走自己的 POSIX 分支：读 `main.patched.bin` → `mmap` →
`mprotect`，`ios/` 的 embed 机制完全不需要。

`tools/extract_ios_guest.py` 从公开发布的 IPA 里还原出同一份 `main.patched.bin` 与
`main.fixups.tsv`/`main.gates.tsv`（IPA 的 `Payload/Biko3.app/Biko3` 里有 `__dmmainx0`、
`__DMmainW,__img`、`__dmmainx1` 三个段）。

### 2. guest 线程槽：每线程克隆 TLS 块

改写器把 guest 的 `x18`、`tpidr_el0`、`tpidrro_el0`、gate 入口都变成
“读 key page 偏移，再 `load [tpidrro_el0 + 偏移]`”。iOS 上宿主把 `tpidrro_el0` 指向自己的块；
Android 上：

* bionic 的 TSD 不是 TP 相对布局，且 x86 模拟器的 ARM 翻译直接拒绝 `tpidrro_el0` 指令；
* `tools/patch_android_guest_image.py` 把镜像里全部 5324 条 `mrs xN, tpidrro_el0`
  改成同宽度的 `mrs xN, tpidr_el0`（布局、patch 区、gates/fixups 元数据都不变）；
* 偏移必须对每个线程都成立，而 bionic 的动态 TLS 块位置逐线程不同（x86 模拟器的
  berberis 更极端，偏移差到 MB 级），因此 `dm_tsd_attach()` 给每个线程**克隆一份 ARM TLS 块**
  （TCB、可执行文件 TLS 段与其上若干字），把四个槽位放在克隆区下方固定距离 `K = 256 KiB` 处，
  再 `msr tpidr_el0` 把线程指针指进克隆区；克隆窗口只取 TP 两侧的有限范围
  （真机上 bionic 把 TLS 放在线程映射的**顶部**、栈在其下方，不能按"映射起点到 TP"整个复制）；
* 宿主代码继续可用：克隆的 TCB 里 DTV、`bionic_tls`、`pthread_internal_t` 指针都指向原对象，
  `pthread_getspecific`/`__tls_get_addr`/stack guard 全部照常；
* guest 读 `[TP − K + slot*8]` 命中克隆区的槽位，key page 偏移对所有线程、所有平台相同；
* 加载时 `tsd_check()` 仍验证 `[TP + key_offset] == 0x1234`。

已排除的方案（都实测过）：`tpidrro_el0` 写入（模拟器 SIGILL）、`x18` 当基址（翻译器自己占用
x18）、initial-exec TLS（bionic 拒绝 dlopen 库）、在 `TP−K` 处 mmap 固定页（其他线程的
TP−K 已被占用）、把槽位镜像到 guest 固定地址（落进 `/system/framework/framework-res.apk`
只读映射，`mprotect` 也改不了）。

### 3. 图形：libvulkan 直连

`ios/runtime/Biko3Vulkan.c` 用 `#if defined(__ANDROID__)` 切到 `VK_KHR_android_surface`：
`ANativeWindow`（来自 `SurfaceView`）→ `vkCreateAndroidSurfaceKHR`。MoltenVK、
`VK_KHR_portability_subset`、`VK_EXT_metal_surface` 全部不参与。

Android 侧的两个实际差异（已在桥里处理）：

* 驱动通常只报告 `VK_COMPOSITE_ALPHA_INHERIT`，而游戏要求 `OPAQUE`：查询时补上 OPAQUE，
  创建 swapchain 时映射回 INHERIT（RGBX 窗口本来就是不透明的）。
* 游戏的交换链按 1280×720 创建：`ANativeWindow_setBuffersGeometry(1280, 720, RGBX)`，由合成器放大。

### 4. 音频

`audout` 的 9 个 hook 指向 `Biko3AudioAndroid.c`：48000 Hz / 立体声 / s16，每块 480 帧。
AAudio 低延迟 + 128 块队列，消费完才把 guest 缓冲标记为 released；打不开时退化为静音 sink。

### 5. 前端

`biko3_android.c` 实现 `front.h` 与 JNI：

* `nativeStart(assets, filesDir, dataDir, Surface, w, h)`：把 assets 里的 guest 文件复制到
  `files/guest`（按大小跳过已有的），设置 `BIKO3_GAME_ROOT`/`BIKO3_SAVE_ROOT` 等环境变量，
  在 64 MiB 栈的线程里跑自检，然后 `dm_main`。
* `onPause/onResume` → `biko3_vk_suspend` / `biko3_audio_suspend` / `guest_clock_freeze_for`。
* `status()` 返回叠加层显示的进度文本；宿主日志写到 `files/log.txt`。

## 已知限制

1. **模拟器**：x86_64 镜像靠 `libndk_translation`（berberis）翻译 arm64，性能仍足够（标题画面
   稳定 60 fps）。除 TLS 布局外，它还会把少数合法指令判为非法，当前镜像补了两类：
   `fcvtzu/fcvtzs dD,dN`（标量双精度→整数）改写成等价的向量形式。
   新补丁集中在 `tools/patch_android_guest_image.py` 的 `TRANSLATOR_CLASSES`，带原始指令校验。
   另：本机 x86_64 的 Android Emulator 拒绝启动 arm64 系统镜像（launcher 架构检查 +
   官方 QEMU 的 arm64 机型缺 PCI 设备），所以无法用原生 arm64 模拟器绕开翻译层。
2. **数据**：APK 不含游戏数据；首次启动需导入约 3 GB。
3. **输入**：目前触摸直接进游戏触摸屏，虚拟手柄与实体手柄尚未接上（`dm_input_set` 接口已就绪）。
4. **Surface 生命周期**：切后台时 Surface 会被销毁，尚未实现“假 surface 句柄 + swapchain 失效”
   恢复（见 `docs/android-port-feasibility.md` R2）。
5. **仅 arm64-v8a**：x86_64 构建只用于模拟器自检，不能执行 guest。

## 下一步

1. arm64 真机验证：装 APK、推分卷，应用内导入后跑通游戏主循环并截图。
2. 虚拟手柄（DmGamepad 的 Kotlin 版）+ `InputDevice` 实体手柄。
3. 资源导入器（分卷 → `game/Data`）与进度 UI，复用 `Biko3Data.manifest.json`。
4. Surface 丢失/恢复与 `onTrimMemory`。
5. 真机（arm64）验证：去掉 TP 改写带来的模拟器折衷不影响真机路径；记录帧率、温度、
   AAudio xRun。
