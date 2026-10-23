#!/usr/bin/env python3
"""Build the Android APK for the Biko3 port without Gradle.

The APK is assembled with the SDK build-tools directly (aapt2, javac, d8,
zipalign, apksigner), which keeps the build identical on Linux and in CI and
avoids a Gradle/AGP dependency for a single-activity app.

Environment: ANDROID_HOME (or ANDROID_SDK_ROOT) and ANDROID_NDK_HOME, as set by
the SDK command-line tools installation. Example:

  source ~/.local/share/firered-mobile/android-env.sh
  python3 tools/build_android.py --abi arm64-v8a --abi x86_64 --install --run
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STAGED = ROOT / "work/android/staged"
BUILD = ROOT / "work/android"
ABI_TRIPLES = {"arm64-v8a": "aarch64-linux-android28", "x86_64": "x86_64-linux-android28"}

HOST_SOURCES = [
    "main.c", "loader.c", "thread.c", "crash.c", "hle.c", "hle_svc.c", "hle_thread.c",
    "hle_os.c", "hle_table.c", "hle_hid.c", "hle_touch.c", "hle_oe.c", "hle_guestthread.c",
    "hle_misc.c", "Biko3FS.c", "Biko3Libnx.c", "Biko3Input.c", "Biko3Vulkan.c", "Biko3Hooks.c",
    "Biko3GameProbe.c",
]
PORT_SOURCES = [
    "android_shims.c", "dm_tsd.c", "android_stubs.c", "biko3_selftest.c",
    "biko3_android.c", "Biko3AudioAndroid.c",
]
ENTRY_RENAMES = [
    "_unimpl_stubs=unimpl_stubs", "_unimpl_entry=unimpl_entry", "_dm_gate=dm_gate",
    "_unimpl_c=unimpl_c", "_gate_dispatch=gate_dispatch",
    "_dm_thread_enter=dm_thread_enter", "_dm_thread_returned=dm_thread_returned",
]


def env_path(name: str) -> Path:
    value = os.environ.get(name)
    if not value:
        raise SystemExit(f"{name} is not set; source the Android SDK environment first")
    return Path(value)


def run(command, log=None):
    command = [str(part) for part in command]
    print("+", " ".join(command))
    result = subprocess.run(command, capture_output=True, text=True)
    if log:
        Path(log).parent.mkdir(parents=True, exist_ok=True)
        Path(log).write_text(result.stdout + result.stderr, encoding="utf8")
    if result.returncode:
        sys.stderr.write(result.stdout + result.stderr)
        raise SystemExit(f"command failed: {command[0]}")
    return result.stdout


def native(ndk: Path, abi: str, stage: bool) -> Path:
    triple = ABI_TRIPLES[abi]
    compiler = ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin" / f"{triple}-clang"
    if not compiler.is_file():
        raise SystemExit(f"missing NDK compiler {compiler}")
    upstream = BUILD / "upstream/nxnative"
    objects = BUILD / f"obj/{abi}"
    if objects.exists():
        shutil.rmtree(objects)
    objects.mkdir(parents=True)
    out = BUILD / f"lib/{abi}"
    out.mkdir(parents=True, exist_ok=True)
    common = [
        "-std=gnu11", "-O2", "-g", "-fPIC", "-DANDROID=1", "-Wno-unused-parameter",
        f"-I{STAGED/'gen'}", f"-I{ROOT/'android/port/include'}", f"-I{STAGED/'host'}",
        f"-I{upstream/'dm/gfx/nvn/include'}", f"-I{upstream/'dm/third_party/vulkan-headers/include'}",
        f"-I{upstream/'dm/third_party/stb_vorbis'}", "-include", "port_prefix.h",
    ]
    # Native TLS on arm64 (bionic resolves __tls_get_addr); emulated TLS on the
    # x86_64 emulator build, which never executes the guest anyway.
    tls_flags = ["-fno-emulated-tls"] if abi == "arm64-v8a" else ["-femulated-tls"]
    common += tls_flags
    compiled = []
    for name in HOST_SOURCES:
        extra = ["-Dmain=dm_main"] if name == "main.c" else []
        target = objects / (Path(name).stem + ".o")
        run([compiler, *common, *extra, "-c", STAGED / "host" / name, "-o", target],
            log=objects / (Path(name).stem + ".log"))
        compiled.append(target)
    for name in PORT_SOURCES:
        target = objects / (Path(name).stem + ".o")
        run([compiler, *common, "-c", ROOT / "android/port/src" / name, "-o", target],
            log=objects / (Path(name).stem + ".log"))
        compiled.append(target)
    entry = objects / "entry.o"
    if abi == "arm64-v8a":
        renames = [f"-D{rename}" for rename in ENTRY_RENAMES]
        run([compiler, "-std=gnu11", "-O1", "-fPIC", "-DANDROID=1", *renames,
             "-c", STAGED / "host/entry.S", "-o", entry], log=objects / "entry.log")
    else:
        run([compiler, *common, "-c", ROOT / "android/port/src/entry_stub_x86_64.c", "-o", entry],
            log=objects / "entry.log")
    compiled.append(entry)
    library = out / "libbiko3.so"
    run([compiler, "-shared", "-o", library, *compiled, "-Wl,--no-undefined",
         "-Wl,-z,max-page-size=16384", "-lvulkan", "-laaudio", "-landroid", "-lnativewindow", "-llog", "-lm", "-ldl"],
        log=objects / "link.log")
    return library


def package(sdk: Path, abis: list[str], libraries: dict[str, Path],
           release: bool = False, ndk: Path | None = None) -> Path:
    java = env_path("JAVA_HOME") / "bin/java"
    if not java.is_file():
        java = Path(shutil.which("java") or "")
    if not java.is_file():
        raise SystemExit("java not found; set JAVA_HOME")
    build_tools = max((sdk / "build-tools").iterdir(), key=lambda path: path.name)
    platform = sdk / "platforms" / "android-36" / "android.jar"
    if not platform.is_file():
        platforms = sorted((sdk / "platforms").iterdir())
        platform = platforms[-1] / "android.jar"
    work = BUILD / "apk"
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    assets = work / "assets/guest"
    assets.mkdir(parents=True)
    for source in (STAGED / "assets/guest").iterdir():
        shutil.copyfile(source, assets / source.name)
    manifest = ROOT / "android/app/AndroidManifest.xml"
    if release:
        # Release builds are not debuggable: drop the attribute the development
        # manifest carries for `run-as` based smoke tests.
        text = manifest.read_text(encoding="utf8")
        text = text.replace('\n        android:debuggable="true"', "")
        manifest = work / "AndroidManifest.release.xml"
        manifest.write_text(text, encoding="utf8")
    if release and ndk:
        strip = ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"
        for abi, library in list(libraries.items()):
            stripped = work / f"stripped-{abi}.so"
            run([strip, "--strip-debug", library, "-o", stripped])
            libraries[abi] = stripped
    unsigned = work / "unsigned.apk"
    run([build_tools / "aapt2", "link", "-o", unsigned,
         "--manifest", manifest,
         "-I", platform, "-A", work / "assets",
         "--min-sdk-version", "28", "--target-sdk-version", "36", "--version-code", "1",
         "--version-name", "0.1.0"], log=work / "aapt2.log")
    classes = work / "classes"
    classes.mkdir()
    sources = sorted((ROOT / "android/app/java").rglob("*.java"))
    run([env_path("JAVA_HOME") / "bin/javac" if (env_path("JAVA_HOME") / "bin/javac").is_file() else "javac",
         "-source", "17", "-target", "17", "-Xlint:-options", "-classpath", platform,
         "-d", classes, *sources], log=work / "javac.log")
    dex = work / "dex"
    dex.mkdir()
    run([build_tools / "d8", "--min-api", "28", "--lib", platform,
         "--output", dex, *sorted(classes.rglob("*.class"))], log=work / "d8.log")
    stage_dir = work / "package"
    stage_dir.mkdir()
    shutil.copyfile(dex / "classes.dex", stage_dir / "classes.dex")
    for abi, library in libraries.items():
        target = stage_dir / "lib" / abi / "libbiko3.so"
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(library, target)
    import zipfile
    merged = work / "merged.apk"
    with zipfile.ZipFile(unsigned) as source, zipfile.ZipFile(merged, "a", zipfile.ZIP_DEFLATED) as target:
        for item in source.infolist():
            target.writestr(item, source.read(item.filename))
        for path in sorted(stage_dir.rglob("*")):
            if path.is_file():
                target.write(path, path.relative_to(stage_dir).as_posix())
    aligned = work / "aligned.apk"
    run([build_tools / "zipalign", "-f", "4", merged, aligned], log=work / "zipalign.log")
    if release:
        keystore = Path.home() / ".android/biko3-release.keystore"
        alias, store_pass = "biko3", "biko3release"
        if not keystore.is_file():
            run([env_path("JAVA_HOME") / "bin/keytool", "-genkeypair", "-keystore", keystore,
                 "-alias", alias, "-storepass", store_pass, "-keypass", store_pass,
                 "-keyalg", "RSA", "-keysize", "2048", "-validity", "10000",
                 "-dname", "CN=Biko3 Android,O=Biko3,C=CN"], log=work / "keytool.log")
        signed = BUILD / "Biko3-android-release.apk"
        run([build_tools / "apksigner", "sign", "--ks", keystore, "--ks-pass", f"pass:{store_pass}",
             "--key-pass", f"pass:{store_pass}", "--ks-key-alias", alias,
             "--out", signed, aligned], log=work / "apksigner.log")
    else:
        keystore = Path.home() / ".android/debug.keystore"
        if not keystore.is_file():
            run([env_path("JAVA_HOME") / "bin/keytool", "-genkeypair", "-keystore", keystore,
                 "-alias", "androiddebugkey", "-storepass", "android", "-keypass", "android",
                 "-keyalg", "RSA", "-keysize", "2048", "-validity", "10000",
                 "-dname", "CN=Android Debug,O=Android,C=US"], log=work / "keytool.log")
        signed = BUILD / "Biko3-android-dev.apk"
        run([build_tools / "apksigner", "sign", "--ks", keystore, "--ks-pass", "pass:android",
             "--key-pass", "pass:android", "--ks-key-alias", "androiddebugkey",
             "--out", signed, aligned], log=work / "apksigner.log")
    run([build_tools / "apksigner", "verify", "--print-certs", signed], log=work / "verify.log")
    print(f"APK: {signed} ({signed.stat().st_size} bytes)")
    return signed


def adb(sdk: Path, *arguments: str, check: bool = False) -> str:
    result = subprocess.run([str(sdk / "platform-tools/adb"), *map(str, arguments)],
                            capture_output=True, text=True)
    if check and result.returncode:
        raise SystemExit("adb " + " ".join(arguments) + " failed: " + result.stderr)
    return result.stdout


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--abi", action="append", choices=sorted(ABI_TRIPLES), default=None)
    parser.add_argument("--no-stage", action="store_true")
    parser.add_argument("--install", action="store_true")
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--serial", default=None)
    parser.add_argument("--package-only", action="store_true")
    parser.add_argument("--release", action="store_true",
                        help="not debuggable, signed with the Biko3 release key")
    args = parser.parse_args(argv)
    sdk = env_path("ANDROID_HOME")
    ndk = env_path("ANDROID_NDK_HOME")
    abis = args.abi or ["arm64-v8a", "x86_64"]
    if not args.no_stage and not args.package_only:
        run([sys.executable, ROOT / "tools/stage_android.py"])
    libraries = {}
    if not args.package_only:
        for abi in abis:
            libraries[abi] = native(ndk, abi, True)
    else:
        for abi in abis:
            libraries[abi] = BUILD / f"lib/{abi}/libbiko3.so"
    apk = package(sdk, abis, libraries, release=args.release, ndk=ndk)
    if args.install or args.run:
        serial = ["-s", args.serial] if args.serial else []
        adb(sdk, *serial, "install", "-r", apk, check=True)
        if args.run:
            adb(sdk, *serial, "shell", "am", "force-stop", "io.github.hyc1743.biko3", check=True)
            adb(sdk, *serial, "shell", "am", "start", "-n",
                "io.github.hyc1743.biko3/.Biko3Activity", check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
