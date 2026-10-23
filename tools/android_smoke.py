#!/usr/bin/env python3
"""Build, install and launch the Android port on a connected emulator/device,
then report how far it gets. Evidence comes from the app's own status file
(readable with `run-as`, so the app must be the debuggable development build)
and, when the host allows it, from the native log.txt.

Exit status: 0 when the self-test passed and the guest reached the renderer,
1 otherwise. The JSON report separates what was observed from what was not.
"""
from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = "io.github.hyc1743.biko3"
ACTIVITY = PACKAGE + "/.Biko3Activity"


def adb(serial, *arguments, check=False):
    command = [str(Path(shutil.which("adb") or "adb"))]
    if serial:
        command += ["-s", serial]
    command += [str(argument) for argument in arguments]
    result = subprocess.run(command, capture_output=True, text=True)
    if check and result.returncode:
        raise SystemExit("adb failed: " + result.stderr.strip())
    return result.stdout


def run_as(serial, *arguments):
    return adb(serial, "shell", "run-as", PACKAGE, *arguments)


def wait_for(serial, marker, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        output = run_as(serial, "cat", "files/status.txt")
        if marker in output:
            return True
        time.sleep(1)
    return False


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", default=None)
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args(argv)

    report = {"serial": args.serial, "installed": False, "launched": False,
              "self_test_passed": False, "guest_boot_started": False,
              "renderer_ready": False, "tls_per_thread_warning": False}
    if not args.no_build:
        subprocess.run([sys.executable, str(ROOT / "tools/build_android.py"),
                        "--abi", "arm64-v8a"], check=True)
    apk = ROOT / "work/android/Biko3-android-dev.apk"
    adb(args.serial, "install", "-r", apk, check=True)
    report["installed"] = True
    adb(args.serial, "shell", "am", "force-stop", PACKAGE, check=True)
    adb(args.serial, "shell", "am", "start", "-n", ACTIVITY, check=True)
    report["launched"] = True
    report["self_test_passed"] = wait_for(args.serial, "self-test: passed", args.timeout)
    report["guest_boot_started"] = wait_for(args.serial, "booting the original guest", 10)
    status = run_as(args.serial, "cat", "files/status.txt")
    report["tls_per_thread_warning"] = "TP offset differs" in status
    # Renderer readiness is a host-side line in the native log; try to read it
    # directly (shell may not have access on every platform) and via root.
    for attempt in (lambda: adb(args.serial, "shell", "cat",
                                f"/storage/emulated/0/Android/data/{PACKAGE}/files/log.txt"),
                    lambda: adb(args.serial, "root") and adb(args.serial, "shell", "cat",
                                f"/storage/emulated/0/Android/data/{PACKAGE}/files/log.txt")):
        try:
            log = attempt() or ""
        except SystemExit:
            continue
        if "Renderer ready" in log:
            report["renderer_ready"] = True
            break
        time.sleep(2)
    report["game_running"] = False
    for attempt in (lambda: adb(args.serial, "shell", "cat",
                                f"/storage/emulated/0/Android/data/{PACKAGE}/files/log.txt"),
                    lambda: adb(args.serial, "root") and adb(args.serial, "shell", "cat",
                                f"/storage/emulated/0/Android/data/{PACKAGE}/files/log.txt")):
        try:
            log = attempt() or ""
        except SystemExit:
            continue
        if "PERF " in log and "Scene state" in log:
            report["game_running"] = True
            report["scene_tail"] = [line for line in log.splitlines() if line.startswith("Scene state")][-1:]
            break
        time.sleep(2)
    report["status_tail"] = status.strip().splitlines()[-6:]
    print(json.dumps(report, indent=2, ensure_ascii=False))
    ok = report["self_test_passed"] and report["renderer_ready"] and report["game_running"]
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
