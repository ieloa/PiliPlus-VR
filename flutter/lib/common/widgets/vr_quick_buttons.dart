import 'package:flutter/material.dart';
import 'package:shared_preferences/shared_preferences.dart';

/// VR entry buttons placed in the same Row as the "关注" button.
///
/// These are real Flutter widgets, not native overlay views. They use the exact
/// same TextButton.styleFrom as `followButton()` in the UGC intro page, so they
/// line up with it and disappear together with the page UI in fullscreen.
///
/// The native side (VrQuickChannel) cannot use a MethodChannel here: this APK
/// is R8-shrunk and no longer contains io.flutter.plugin.common.MethodChannel
/// or the original vr_bridge handler. Instead we share Android's
/// FlutterSharedPreferences file, which the shared_preferences plugin uses.
/// Keys are written with the plugin's default "flutter." prefix and observed
/// by a native OnSharedPreferenceChangeListener.
class VrQuickButtons extends StatefulWidget {
  const VrQuickButtons({super.key});

  @override
  State<VrQuickButtons> createState() => _VrQuickButtonsState();
}

class _VrQuickButtonsState extends State<VrQuickButtons>
    with WidgetsBindingObserver {
  static const String _keyAuto = 'vr_auto_enter';
  static const String _keyEnter = 'vr_quick_enter';

  bool _autoEnter = true;
  SharedPreferences? _prefs;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    _load();
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    super.dispose();
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    // The VR settings card can change the same setting; refresh on return.
    if (state == AppLifecycleState.resumed) _load();
  }

  Future<void> _load() async {
    for (int i = 0; i < 5; i++) {
      try {
        final prefs = await SharedPreferences.getInstance();
        await prefs.reload();
        _prefs = prefs;
        final value = prefs.getBool(_keyAuto);
        if (mounted && value != null) {
          setState(() => _autoEnter = value);
        }
        return;
      } catch (_) {
        // Native bridge may not have published the initial value yet.
      }
      await Future<void>.delayed(const Duration(milliseconds: 250));
    }
  }

  Future<void> _enterVr() async {
    try {
      final prefs = _prefs ?? await SharedPreferences.getInstance();
      // A fresh value every time guarantees the native listener fires.
      await prefs.setInt(_keyEnter, DateTime.now().microsecondsSinceEpoch);
    } catch (_) {}
  }

  Future<void> _toggleAutoEnter() async {
    final bool next = !_autoEnter;
    setState(() => _autoEnter = next);
    try {
      final prefs = _prefs ?? await SharedPreferences.getInstance();
      await prefs.setBool(_keyAuto, next);
    } catch (_) {}
  }

  ButtonStyle _style(ColorScheme scheme) => TextButton.styleFrom(
        tapTargetSize: MaterialTapTargetSize.shrinkWrap,
        visualDensity: const VisualDensity(vertical: -2.8),
        foregroundColor: scheme.onSecondaryContainer,
        backgroundColor: scheme.secondaryContainer,
      );

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    final style = _style(scheme);
    const textStyle = TextStyle(fontSize: 13);
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        TextButton(
          onPressed: _enterVr,
          style: style,
          child: const Text(' 进入VR ', style: textStyle),
        ),
        const SizedBox(width: 6),
        TextButton(
          onPressed: _toggleAutoEnter,
          style: style,
          child: Text(
            _autoEnter ? ' 自动:开 ' : ' 自动:关 ',
            style: textStyle,
          ),
        ),
        const SizedBox(width: 6),
      ],
    );
  }
}
