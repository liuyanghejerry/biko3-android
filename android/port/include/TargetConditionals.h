/* Android port: Darwin platform headers are not available on bionic. These
 * shim headers provide the small subset of the Apple API surface the nxnative
 * host uses, so untouched upstream sources keep compiling. The behaviour is
 * implemented in android/port/src/android_shims.c. */
#pragma once
#define TARGET_OS_MAC 0
#define TARGET_OS_IPHONE 0
#define TARGET_OS_OSX 0
#define TARGET_OS_SIMULATOR 0
