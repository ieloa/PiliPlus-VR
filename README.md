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

## 5. 开源协议与声明

- **开源协议**：本项目沿用与上游 PiliPlus 一致的 **[GNU General Public License v3.0 (GPL-3.0)](LICENSE)** 协议开源。你可在遵守 GPL-3.0 条款的前提下自由使用、修改与分发本源码；任何基于本项目的修改或衍生版本均需保留版权声明、注明修改记录并以 GPL-3.0 开源。本软件按“现状”提供，不附带任何明示或默示担保。
- **上游致敬与修改说明**：本项目（PiliPlus VR）基于 [bggRGjQaUbCoE/PiliPlus](https://github.com/bggRGjQaUbCoE/PiliPlus) 修改开发（于 2026 年 11 月新增 Meta Quest 3 OpenXR VR 渲染层、AI 3D 实时深度、OpenXR 弹幕同步及手势追踪等功能），同时致敬上游项目 [orz12/PiliPalaX](https://github.com/orz12/PiliPalaX) 与原作者 [guozhigq/pilipala](https://github.com/guozhigq/pilipala)。
- **免责声明**：本项目仅用于个人兴趣、学习与技术研究，不提供任何破解或侵权内容，与 Bilibili 官方及原版 PiliPlus 作者无直接隶属关系；请遵守相关平台服务条款与法律法规。
