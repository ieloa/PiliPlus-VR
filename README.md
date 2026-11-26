# PiliPlus VR

Open-source VR layer and application source code for **PiliPlus VR** (Meta Quest 3 / `arm64-v8a`).

## 1. Repository Structure

```
java/                      Java sources of the VR layer (com.example.piliplus.vr)
native/                    C/C++ OpenXR renderer sources (libp5m-vr.so)
flutter/                   Flutter/Dart app sources
rebuild_from_source.ps1    Full rebuild script (requires base APK unpacked into apktool/, deps/, tools/, cert/)
rebuild.bat                Quick repackage + sign script
```

> **Note**: Prebuilt binaries, APKs, `apktool/` decompiled assets, `deps/`, `tools/`, and `cert/` keystores are excluded from this repository. To rebuild an installable APK, prepare your own base APK (unpacked with `apktool` into `apktool/`), dependencies (`deps/`), `tools/apktool.jar`, and signing keystore (`cert/debug.keystore`).

## 2. Key Highlights in This Build

* **AI 3D Depth (ZipDepth / DepthView)**: LiteRT ZipDepth dual-thread inference, EMA & deadzone temporal stabilization, cubic Newton depth warp, RGSS AA, AI edge enhance, plus Turbo heatmap `uShowDepth` toggle (VR Settings page 3 / long-press **Y**).
* **OpenXR Danmaku Stability & Sync**: Start-of-frame wakeup synced to `xrBeginFrame`, frame-tagged 2-slot queue (`g_dm_slots`), partial-height bitmap clear & `glTexSubImage2D` upload, `danmaku_swapchain_acquired_` / `XR_TIMEOUT_EXPIRED` state-machine guard preventing `XR_ERROR_CALL_ORDER_INVALID` freezes, mono `2048x1152` cap, and mandatory `25%` danmaku density cap in all 3D surround modes.
* **Portrait & 8K UX Caps**: Portrait videos (`videoW < videoH`) default `danmakuVOut` (top/bottom out-of-screen danmaku) to OFF; 8K quality options are filtered out in VR (`MAX_RENDER_LONG_EDGE = 4096`) and auto-switch to 4K on VR entry.
* **Hand Tracking & Microgestures**: Tightened pinch-drag release thresholds; added fist thumb-tap microgesture via OpenXR 26-joint skeletal tracking (`XR_EXT_hand_tracking`):
  * **Left hand fist + thumb tap**: Cycle AI 3D strength (`cycleSynth3dStrength()`)
  * **Right hand fist + thumb tap**: Play / Pause (`togglePlayPause()`)
  * Thumb swipe seek disabled to avoid accidental triggers.

## 3. Rebuild

### Option A: Full Rebuild from C++ & Java Source (`Source/rebuild_from_source.ps1`)
Requires Windows, JDK 11+, Android SDK (NDK + CMake + Ninja + build-tools 36.0.0 + android-34 platform).

```powershell
cd Source
powershell -ExecutionPolicy Bypass -File .\rebuild_from_source.ps1
```

This compiles `Source/native/*.cpp` into `libp5m-vr.so`, compiles `Source/java/**/*.java` into `smali`, rebuilds the APK via `apktool`, aligns with `zipalign`, and signs with `Source/cert/debug.keystore`.

### Option B: Quick Repackage (`Source/rebuild.bat`)
Requires JDK 11+, Android SDK build-tools 36.0.0 (`ANDROID_HOME` or `ANDROID_SDK_ROOT` set).

```cmd
cd Source
rebuild.bat
```

## 4. Install

```cmd
adb install -r --no-incremental PiliPlus_VR_rebuilt.apk
```