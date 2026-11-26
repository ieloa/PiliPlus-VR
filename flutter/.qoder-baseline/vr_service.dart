import 'dart:io' show Platform;
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_smart_dialog/flutter_smart_dialog.dart';
import 'package:get/get.dart';
import 'package:PiliPlus/pages/video/controller.dart';
import 'package:PiliPlus/plugin/pl_player/controller.dart';
import 'package:PiliPlus/models/common/video/video_type.dart';
import 'package:PiliPlus/utils/accounts.dart';

enum VrProjectionMode {
  cinema('cinema', '巨幕影院 (2D Cinema)', '在虚拟影院巨幕中观看普通视频', Icons.movie_filter_outlined),
  sphere360('sphere_360', '360° 全景 (Sphere)', '球形360度全景，随头部转动', Icons.panorama_photosphere_outlined),
  dome180('dome_180', '180° VR (Dome)', '正面180度半球沉浸视角', Icons.screen_rotation_outlined),
  sbs3d('sbs_3d', '3D 左右格式 (SBS)', '左右半宽/全宽双目3D立体渲染', Icons.view_in_ar_outlined),
  ou3d('ou_3d', '3D 上下格式 (Top-Bottom)', '上下分屏双目3D立体渲染', Icons.vertical_split_outlined);

  final String key;
  final String title;
  final String subtitle;
  final IconData icon;

  const VrProjectionMode(this.key, this.title, this.subtitle, this.icon);
}

class VrService {
  static const MethodChannel _channel = MethodChannel('com.example.piliplus/vr_bridge');
  static bool? _isQuest;

  /// Check if the device is a Meta Quest VR headset
  static Future<bool> isQuestDevice() async {
    if (!Platform.isAndroid) return false;
    if (_isQuest != null) return _isQuest!;
    try {
      final res = await _channel.invokeMethod<bool>('isQuestDevice');
      _isQuest = res ?? false;
    } catch (_) {
      _isQuest = false;
    }
    return _isQuest!;
  }

  /// Launch the native Quest/Android VR player Activity
  static Future<Map<String, dynamic>?> openVrPlayer({
    required String videoUrl,
    String? audioUrl,
    required String title,
    String? bvid,
    int? aid,
    int? cid,
    String? desc,
    String? ownerName,
    String? ownerFace,
    String? viewCount,
    String? likeCount,
    String? cookie,
    int positionMs = 0,
    String vrMode = 'cinema',
  }) async {
    if (!Platform.isAndroid) {
      SmartDialog.showToast('VR 模式目前仅支持 Android / Meta Quest 设备');
      return null;
    }

    try {
      final result = await _channel.invokeMapMethod<String, dynamic>('openVrPlayer', {
        'videoUrl': videoUrl,
        'audioUrl': audioUrl,
        'title': title,
        'bvid': bvid,
        'aid': aid,
        'cid': cid,
        'desc': desc,
        'ownerName': ownerName,
        'ownerFace': ownerFace,
        'viewCount': viewCount,
        'likeCount': likeCount,
        'cookie': cookie,
        'positionMs': positionMs,
        'vrMode': vrMode,
      });

      return result;
    } catch (e) {
      SmartDialog.showToast('启动 VR 播放器失败: $e');
      return null;
    }
  }

  /// Show YouTube VR style Mode Selector Dialog
  static void showVrModeSheet({
    required BuildContext context,
    required VideoDetailController videoDetailController,
    required PlPlayerController plPlayerController,
  }) {
    final videoUrl = videoDetailController.videoUrl;
    if (videoUrl == null || videoUrl.isEmpty) {
      SmartDialog.showToast('当前视频流尚未准备就绪');
      return;
    }

    final audioUrl = videoDetailController.audioUrl;
    String? title;
    String? desc;
    String? ownerName;
    String? ownerFace;
    String? viewCount;
    String? likeCount;
    try {
      final d = videoDetailController.videoType == VideoType.ugc
          ? videoDetailController.ugcIntroCtr.videoDetail.value
          : videoDetailController.pgcIntroCtr.videoDetail.value;
      title = d.title;
      desc = d.desc;
      ownerName = d.owner?.name;
      ownerFace = d.owner?.face;
      viewCount = d.stat?.view == null ? null : d.stat!.view.toString();
      likeCount = d.stat?.like == null ? null : d.stat!.like.toString();
    } catch (_) {}
    final String safeTitle = title ?? 'Bilibili 视频';
    final currentPosMs = plPlayerController.position.value * 1000;

    String cookie = '';
    try {
      cookie = Accounts.main.headers['cookie'] ?? Accounts.main.headers['Cookie'] ?? '';
    } catch (_) {}

    showModalBottomSheet(
      context: context,
      backgroundColor: Colors.transparent,
      builder: (ctx) {
        final theme = Theme.of(ctx);
        final isDark = theme.brightness == Brightness.dark;

        return Container(
          decoration: BoxDecoration(
            color: isDark ? const Color(0xFF1E1E24) : Colors.white,
            borderRadius: const BorderRadius.vertical(top: Radius.circular(20)),
          ),
          padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 16),
          child: SafeArea(
            child: Column(
              mainAxisSize: MainAxisSize.min,
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Row(
                  children: [
                    const Icon(Icons.view_in_ar, size: 24, color: Color(0xFFFB7299)),
                    const SizedBox(width: 10),
                    Text(
                      '选择 VR 沉浸播放模式',
                      style: theme.textTheme.titleMedium?.copyWith(
                        fontWeight: FontWeight.bold,
                      ),
                    ),
                    const Spacer(),
                    IconButton(
                      icon: const Icon(Icons.close, size: 20),
                      onPressed: () => Navigator.of(ctx).pop(),
                    ),
                  ],
                ),
                const SizedBox(height: 8),
                Text(
                  '支持 Meta Quest 3 头部 6DoF 追踪与 YouTube VR 级空间双翼交互',
                  style: theme.textTheme.bodySmall?.copyWith(
                    color: theme.textTheme.bodySmall?.color?.withValues(alpha: 0.7),
                  ),
                ),
                const SizedBox(height: 12),
                ...VrProjectionMode.values.map((mode) {
                  return ListTile(
                    contentPadding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
                    leading: Container(
                      padding: const EdgeInsets.all(8),
                      decoration: BoxDecoration(
                        color: const Color(0xFFFB7299).withValues(alpha: 0.15),
                        borderRadius: BorderRadius.circular(10),
                      ),
                      child: Icon(mode.icon, color: const Color(0xFFFB7299)),
                    ),
                    title: Text(
                      mode.title,
                      style: const TextStyle(fontWeight: FontWeight.w600, fontSize: 15),
                    ),
                    subtitle: Text(
                      mode.subtitle,
                      style: TextStyle(
                        fontSize: 12,
                        color: theme.textTheme.bodySmall?.color?.withValues(alpha: 0.7),
                      ),
                    ),
                    trailing: const Icon(Icons.arrow_forward_ios, size: 14),
                    onTap: () async {
                      Navigator.of(ctx).pop();

                      // Pause 2D player
                      plPlayerController.pause();

                      // Open native VR Activity
                      final res = await openVrPlayer(
                        videoUrl: videoUrl,
                        audioUrl: audioUrl,
                        title: safeTitle,
                        bvid: videoDetailController.bvid,
                        aid: videoDetailController.aid,
                        cid: videoDetailController.cid.value,
                        desc: desc,
                        ownerName: ownerName,
                        ownerFace: ownerFace,
                        viewCount: viewCount,
                        likeCount: likeCount,
                        cookie: cookie,
                        positionMs: currentPosMs,
                        vrMode: mode.key,
                      );

                      if (res != null) {
                        final lastBvid = res['lastBvid'] as String?;
                        final lastPos = (res['lastPosition'] as num?)?.toInt() ?? 0;

                        if (lastBvid != null && lastBvid.isNotEmpty && lastBvid != videoDetailController.bvid) {
                          // Switched to related video inside VR
                          Get.toNamed('/videoV', arguments: {'bvid': lastBvid});
                        } else if (lastPos > 0) {
                          plPlayerController
                            ..seekTo(
                              Duration(milliseconds: lastPos),
                              isSeek: false,
                            )
                            ..play();
                        }
                      }
                    },
                  );
                }),
                const SizedBox(height: 8),
              ],
            ),
          ),
        );
      },
    );
  }
}
