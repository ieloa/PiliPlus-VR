# PiliPlus VR

**PiliPlus VR**（Meta Quest 3 / `arm64-v8a`）的开源 VR 层与应用源码。

- 作者主页：【Ao_Lei的个人空间-哔哩哔哩】 https://b23.tv/buRNwq8

## 1. 仓库目录结构

```text
java/                      VR 层 Java 源码（com.example.piliplus.vr）
native/                    C/C++ OpenXR 渲染器源码（libp5m-vr.so）
flutter/                   Flutter / Dart 应用源码
rebuild_from_source.ps1    完整重编译脚本（需自行准备解包后的 apktool/、deps/、tools/、cert/）
rebuild.bat                快速重新打包与签名脚本
```

> **说明**：本仓库仅包含纯源码，不包含 APK 安装包、预编译二进制、`apktool/` 反编译资源、`deps/`、`tools/` 及 `cert/` 签名证书。如需自行重编译可安装 APK，请自行准备原版 APK（使用 `apktool` 解包至 `apktool/`）、编译依赖（`deps/`）、`tools/apktool.jar` 以及签名密钥库（`cert/debug.keystore`）。

## 2. 本版本核心特性

* **AI 3D 实时深度（ZipDepth / DepthView）**：LiteRT ZipDepth 双线程推理、EMA 与死区时序稳定、三次牛顿深度扭曲（cubic Newton depth warp）、RGSS 抗锯齿、AI 边缘增强，以及 Turbo 深度热力图 `uShowDepth` 开关（VR 设置第 3 页 / 长按 **Y** 键）。
* **OpenXR 弹幕稳定性与帧同步**：与 `xrBeginFrame` 同步的帧首唤醒、带帧标记的双槽队列（`g_dm_slots`）、局部高度位图清理与 `glTexSubImage2D` 上传、`danmaku_swapchain_acquired_` / `XR_TIMEOUT_EXPIRED` 状态机保护（防止 `XR_ERROR_CALL_ORDER_INVALID` 卡死）、单目 `2048x1152` 上限，以及所有 3D 环绕模式下强制 `25%` 弹幕密度上限。
* **竖屏与 8K 体验优化**：竖屏视频（`videoW < videoH`）默认关闭 `danmakuVOut`（屏幕上下外溢弹幕）；VR 模式下自动过滤 8K 画质选项（`MAX_RENDER_LONG_EDGE = 4096`），进入 VR 时自动切换至 4K。
* **手势追踪与微手势**：收紧捏合拖拽释放阈值；基于 OpenXR 26 关节骨骼追踪（`XR_EXT_hand_tracking`）新增握拳拇指轻点微手势：
  * **左手握拳 + 拇指轻点**：循环切换 AI 3D 强度（`cycleSynth3dStrength()`）
  * **右手握拳 + 拇指轻点**：播放 / 暂停（`togglePlayPause()`）
  * 已禁用拇指滑动快进，避免误触。

## 3. 重新编译

### 方式一：从 C++ 与 Java 源码完整重编译（`rebuild_from_source.ps1`）
环境要求：Windows、JDK 11+、Android SDK（NDK + CMake + Ninja + build-tools 36.0.0 + android-34 平台）。

```powershell
powershell -ExecutionPolicy Bypass -File .\rebuild_from_source.ps1
```

该脚本会将 `native/*.cpp` 编译为 `libp5m-vr.so`，将 `java/**/*.java` 编译为 `smali`，通过 `apktool` 重建 APK，使用 `zipalign` 对齐，并通过 `cert/debug.keystore` 签名输出 `PiliPlus_VR_rebuilt.apk`。

### 方式二：快速重新打包（`rebuild.bat`）
环境要求：JDK 11+、Android SDK build-tools 36.0.0（需配置 `ANDROID_HOME` 或 `ANDROID_SDK_ROOT`）。

```cmd
rebuild.bat
```

## 4. 安装

```cmd
adb install -r --no-incremental PiliPlus_VR_rebuilt.apk
```
