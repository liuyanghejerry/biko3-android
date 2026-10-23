# Biko3 Android

> **This is the Android port.** It is a fork of
> **[hyc1743/biko3-ios](https://github.com/hyc1743/biko3-ios)** — the iOS port of Biko3 by
> [hyc1743](https://github.com/hyc1743). The offline guest rewriter, the native host kernel
> (nxnative), the 106-hook table, the Vulkan bridge and the resource-pack tooling all come from
> that project. This fork keeps them intact and replaces only the platform layer with an Android
> implementation (window and presentation, audio, input, files, lifecycle, thread slots).
> The iOS app is unaffected and continues upstream.

## What this is

Biko3 is a Nintendo Switch game shipped as an AArch64 NRO. The iOS port runs it natively: the NRO
is statically rewritten offline, embedded in the app as a guest image, and driven by a native host
that emulates the libnx and Vulkan surface the game expects. This fork does the same on Android:

- the guest image is reconstructed from the public IPA and patched for Android's thread-pointer
  conventions;
- the host kernel, hooks and Vulkan bridge are shared with the iOS port, with Android branches for
  `VK_KHR_android_surface`, AAudio, `ANativeWindow` and the Android lifecycle;
- the picture is presented through a `TextureView` in a centred 16:9 box, so display rotation and
  letterboxing stay consistent with the on-screen controls;
- the six `.b3data` volumes are verified and unpacked by the app itself (SHA-256 for the volumes
  and for every extracted file).

## Status

| Area | State | Notes |
| --- | --- | --- |
| Build (arm64-v8a, x86_64) | ✅ | no Gradle — aapt2 / javac / d8 / zipalign / apksigner |
| Self-test | ✅ | guest image identity, file I/O, thread slots, Vulkan device + swapchain, audio |
| Rendering | ✅ | direct Vulkan (`libvulkan.so`), swapchain presentation, 60 fps cap |
| Audio | ✅ | AAudio, 48 kHz stereo signed 16-bit |
| Guest boot | ✅ | libnx init → original `main` → title screen → character select |
| Input | ✅ | touch, virtual pad, physical gamepad (Npad mapping) |
| In-app data import | ✅ | six volumes verified, 169 files unpacked (≈2.7 GB) |
| Verified on | ✅ | x86_64 emulator (API 30/36, ARM translation) and an arm64 Android 16 device |

Builds are tested with NDK r30 and SDK 36. The app targets Android 8.0 (API 28) and newer.

## Requirements

- An arm64 Android device (or an x86_64 emulator with ARM translation for development).
- A Vulkan-capable GPU; the game creates a graphics + compute queue and a 1280×720 swapchain.
- About 3 GB for the six `.b3data` volumes plus about 2.7 GB for the unpacked game data.

## Install (release build)

```sh
python3 tools/build_android.py --abi arm64-v8a --release
adb install -r work/android/Biko3-android-release.apk
```

The release APK is signed with a local key generated on first use at
`~/.android/biko3-release.keystore` (alias `biko3`). Keep that keystore: updates must be signed
with the same key. The APK is not debuggable and the development status overlay is hidden.

The application id stays `io.github.hyc1743.biko3` so that existing installs keep upgrading; the
launcher label is **Biko3 Android**.

## Game data

The six `Biko3Data.partNNN.b3data` volumes (≈2.98 GB total) are distributed separately from the
app. Put them where the app can read them and let the app import them:

```sh
adb push Biko3Data.part00*.b3data \
  /storage/emulated/0/Android/data/io.github.hyc1743.biko3/files/ResourcePacks/
```

Launch the app. It verifies all six volumes, restores the 169 original files under
`files/game/Data` (about 2.7 GB), and starts the game when the data is complete. The import is
resumable: files that were already extracted and verified are skipped.

Alternatively, if the data has already been unpacked on the host:

```sh
adb push <unpacked>/Data \
  /storage/emulated/0/Android/data/io.github.hyc1743.biko3/files/game/
```

The app reads logs and saves from the same external directory (`files/log.txt`, `files/saves/`).

## Build from source

Set the SDK and NDK environment first (for example `source ~/.local/share/firered-mobile/android-env.sh`
if you use the same setup), then:

```sh
# development build (debuggable, both ABIs by default)
python3 tools/build_android.py --abi arm64-v8a --install --run

# release build (signed, not debuggable)
python3 tools/build_android.py --abi arm64-v8a --release

# package only, reusing the staged sources
python3 tools/build_android.py --abi arm64-v8a --no-stage --package-only
```

`tools/stage_android.py` prepares everything the build needs: the upstream host sources are copied
and patched (`tools/patch_android_host.py`), the hook table and game profile are generated, the
guest image is patched (`tools/patch_android_guest_image.py`) and the assets/manifest are staged
under `work/android/staged`.

To rebuild the guest image from the public IPA, use `tools/extract_ios_guest.py`.

## Controls

- **Touch**: the picture area is a touchscreen; touches outside the 16:9 box are ignored.
- **Virtual pad**: d-pad (52 dp arms), A/B/X/Y (60 dp), ZL/ZR, and +/−, laid out in dp so the
  controls stay finger-sized on high-density screens. A is on the right and B on the bottom,
  matching the Nintendo face-button layout.
- **Physical gamepad / keyboard**: standard Android key events are mapped to the same Npad mask
  (A/B/X/Y, L1/R1 as ZL/ZR, dpad, start/select as +/−).

The Switch build's own help text documents the controls it uses: left stick (cursor movement),
ZL (slow movement), d-pad (selection), A (confirm), B (back), + (confirm in some screens), −
(toggle the FPS display), and touch. The PC version's Z/X/C/F5 keys are not part of the Switch
guest's input protocol.

## How it works

- **Guest**: the rewritten NRO image, its fixups/gates metadata and the hook table are installed
  into the app's private storage on first launch; `tools/patch_android_guest_image.py` retargets
  the thread-pointer reads to `tpidr_el0` and rewrites two scalar FP conversion instructions that
  the x86 emulator's ARM translator rejects.
- **Thread slots**: the rewritten guest reads its per-thread data as `[TP + offset]`. Because the
  distance between the thread pointer and dynamic TLS blocks is not stable (and is arbitrary under
  the ARM translator), each guest thread gets a private clone of its ARM TLS block, the slots are
  placed a fixed distance below the clone, and the thread pointer is moved into the clone. Host
  code keeps working because the cloned TCB still points at the real DTV, `bionic_tls` and pthread
  structures.
- **Presentation**: a `TextureView` composites through hwui, so the game picture follows the same
  display rotation as the on-screen controls (a `SurfaceView` does not on a rotated display). A
  `FrameLayout` subclass measures the largest centred 16:9 box on the first layout pass, so the
  surface is created at its final size.
- **Audio**: libnx `audout` calls are translated to an AAudio stream (48 kHz, stereo, 16-bit) with
  a small queue and a silent-sink fallback.
- **Pacing**: presents are paced after `vkQueuePresentKHR` to the display rate (default 60 fps,
  `BIKO3_FPS=0` disables the cap), because the Android present path does not block on vblank.

## Repository layout

| Path | Contents |
| --- | --- |
| `android/app/` | manifest, Activity, virtual gamepad, resource importer, JNI declarations |
| `android/port/` | Android platform layer: Darwin shims, TLS slots, selftest, front end, AAudio |
| `ios/runtime/` | shared runtime (Vulkan bridge, libnx boot, input, hooks) with Android branches |
| `tools/` | staging, build, guest-image patching, IPA extraction, smoke test |
| `docs/android-port.md` | Android port notes, designs, limits and commands |
| `docs/android-port-feasibility.md` | feasibility study and risk register |

## Verification and logs

```sh
python3 tools/android_smoke.py --serial <device>
```

The smoke test builds (unless `--no-build`), installs, launches the app and reports whether the
self-test passed, the guest booted, the renderer came up and the game is running. Guest output is
also written to `Android/data/io.github.hyc1743.biko3/files/log.txt`.

## Known limitations

- On the x86_64 emulator the ARM translator needs the two FP-conversion fixups above; a real arm64
  device executes the original instructions natively.
- The activity is locked to landscape and the picture is letterboxed to 16:9; the app does not
  switch between portrait and landscape at runtime.
- Saves from the iOS port are not imported automatically; the save directory layout differs.
- The release build uses a locally generated signing key; it is not distributed through an app
  store.

## Relationship to the iOS project

This repository is a **fork** of [hyc1743/biko3-ios](https://github.com/hyc1743/biko3-ios) and
would not exist without it:

- the offline static rewriter, the guest hook manifests and the nxnative host kernel are the iOS
  project's work;
- the Vulkan bridge, libnx adapters, input mapping and resource-pack format come from there;
- this fork adds the Android platform layer, the Android build pipeline, the in-app importer and
  the Android-specific fixes described above, and keeps the iOS sources for the shared runtime.

The iOS app remains maintained upstream; Android-specific changes live in this fork.

## Credits

- [hyc1743/biko3-ios](https://github.com/hyc1743/biko3-ios) — the iOS port this project is forked
  from: guest rewriting, native host, hooks, Vulkan bridge and resource tooling.
- [seedds/nxnative](https://github.com/seedds/nxnative) — the native host, rewriter and platform
  services the ports build on.
- [KhronosGroup/MoltenVK](https://github.com/KhronosGroup/MoltenVK) — Vulkan on Metal, used by the
  iOS side of the shared runtime.
- [switchbrew/libnx](https://github.com/switchbrew/libnx) — platform interface references.
- The Chinese patch authors and the resource-pack contributors whose work ships in the data
  volumes; their original credits are preserved.
- [jhaizhou-ops/d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile) and
  [LNPANDY/nxnative-romfs-archive](https://github.com/LNPANDY/nxnative-romfs-archive) — references
  for the iOS adaptation and for splitting large resource sets into volumes.

## License

The adaptation code and tools in this repository are released under the MIT license; see
[LICENSE](LICENSE) and [THIRD_PARTY.md](THIRD_PARTY.md). The game program, images and other assets
remain the property of their respective rights holders.
