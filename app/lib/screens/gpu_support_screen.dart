import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:provider/provider.dart';
import 'package:droiddesk/theme/droid_theme.dart';
import 'package:droiddesk/state/app_state.dart';

/// What the GPU does on this device, as far as we tested it. The rows come from
/// docs/VERIFICATION.md (GPU kit runs, docs/gpu-test-kit.md); update both together.
enum GpuStatus { works, caveat, broken, software, untested }

class GpuCheck {
  final String feature;
  final GpuStatus status;
  final String detail;
  final String evidence;

  const GpuCheck(this.feature, this.status, this.detail, this.evidence);
}

class GpuFamily {
  final String name;
  final String path;
  final String defaultState;
  final List<GpuCheck> checks;

  const GpuFamily({
    required this.name,
    required this.path,
    required this.defaultState,
    required this.checks,
  });
}

const _fold = 'Fold 7 (Adreno 830), fold.106, 2026-11-01';
const _tab = 'Tab S10 FE (Xclipse 540), fold.106, 2026-11-01';

const _adreno = GpuFamily(
  name: 'Adreno (Snapdragon)',
  path: 'Turnip (Vulkan) + Zink (OpenGL)',
  defaultState: 'On by default. Tested on the Adreno 830; older Adreno models may fall back '
      'to software rendering (the self-test decides).',
  checks: [
    GpuCheck('Desktop and Linux apps (OpenGL)', GpuStatus.works,
        'OpenGL 4.6 on Zink, in Termux and in Debian (Mesa 26.2.6)', _fold),
    GpuCheck('Vulkan', GpuStatus.works, 'Turnip, Vulkan 1.4', _fold),
    GpuCheck('WebGL in Firefox', GpuStatus.works, 'WebGL 2, about 115 FPS on the test page', _fold),
    GpuCheck('WebGL in Chromium', GpuStatus.caveat,
        'WebGL 2 works; Zink presents without kopper so it does not flicker (about 60 FPS)', _fold),
    GpuCheck('3D apps', GpuStatus.works, 'OrcaSlicer: model loads and rotates on OpenGL 4.6', _fold),
    GpuCheck('Electron apps (VS Code, Obsidian)', GpuStatus.untested, 'No result yet', ''),
    GpuCheck('Video decoding', GpuStatus.software,
        'Videos play, decoded by the CPU (no hardware decoding in Mesa)', ''),
  ],
);

const _xclipse = GpuFamily(
  name: 'Xclipse (Exynos)',
  path: 'virgl over ANGLE/Vulkan (experimental, opt-in)',
  defaultState: 'Off by default: software rendering (llvmpipe). Turn on with '
      '"droiddesk-native-gpu on" in the terminal and restart the desktop.',
  checks: [
    GpuCheck('Desktop and Termux apps (OpenGL)', GpuStatus.caveat,
        'With the opt-in: OpenGL 2.1 / GLES 3.1 on virgl; without it: software, OpenGL 4.6', _tab),
    GpuCheck('WebGL in Firefox', GpuStatus.works,
        'With the opt-in: about 57 FPS on the test page (2 FPS in software)', _tab),
    GpuCheck('WebGL in Chromium', GpuStatus.caveat,
        'Worked in an older test after a virgl fix; not retested on fold.106',
        'Tab S10 FE, 2026-10-28'),
    GpuCheck('Debian apps (OpenGL)', GpuStatus.broken,
        'With "droiddesk-gpu on" in Debian, OpenGL fails to start (X error); keep it off', _tab),
    GpuCheck('Vulkan', GpuStatus.software, 'Software only (lavapipe)', _tab),
    GpuCheck('Electron apps (VS Code, Obsidian)', GpuStatus.software,
        'Run with the GPU off (it crashed on virgl)', 'Tab S10 FE, 2026-10-26'),
    GpuCheck('Video decoding', GpuStatus.software, 'Videos play, decoded by the CPU', ''),
  ],
);

const _other = GpuFamily(
  name: 'Other GPU (Mali, PowerVR, ...)',
  path: 'Software rendering (llvmpipe); virgl opt-in untested',
  defaultState: 'Software rendering. Everything works, but 3D and WebGL are slow.',
  checks: [
    GpuCheck('Desktop and Linux apps (OpenGL)', GpuStatus.software, 'Software, OpenGL 4.x', ''),
    GpuCheck('Hardware OpenGL (virgl opt-in)', GpuStatus.untested,
        'Same path as Xclipse, never tested on this GPU', ''),
    GpuCheck('WebGL in browsers', GpuStatus.software, 'Works, slowly', ''),
    GpuCheck('Video decoding', GpuStatus.software, 'Videos play, decoded by the CPU', ''),
  ],
);

GpuFamily gpuFamilyFor(String gpuName) {
  final name = gpuName.toLowerCase();
  if (name.contains('adreno')) return _adreno;
  if (name.contains('xclipse')) return _xclipse;
  return _other;
}

class GpuSupportScreen extends StatelessWidget {
  const GpuSupportScreen({super.key});

  static const _issues = 'https://github.com/Vyvrnc/DroidDesk-Fold/issues';

  @override
  Widget build(BuildContext context) {
    final state = context.watch<AppState>();
    final gpu = state.gpuType;
    final family = gpuFamilyFor(gpu);
    final mode = state.deviceInfo['graphicsMode']?.toString() ?? '';

    return Scaffold(
      backgroundColor: DroidTheme.background,
      appBar: AppBar(
        backgroundColor: DroidTheme.background,
        foregroundColor: DroidTheme.textPrimary,
        elevation: 0,
        title: Text('GPU support', style: DroidTheme.headingSm.copyWith(color: DroidTheme.textPrimary)),
      ),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(24, 8, 24, 32),
        children: [
          _card(
            Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(gpu, style: DroidTheme.headingMd.copyWith(color: DroidTheme.textPrimary)),
                const SizedBox(height: 4),
                Text(family.name, style: DroidTheme.bodySm.copyWith(color: DroidTheme.textSecondary)),
                const SizedBox(height: 12),
                _kv('Hardware path', family.path),
                if (mode.isNotEmpty) _kv('Now', mode),
                const SizedBox(height: 8),
                Text(family.defaultState, style: DroidTheme.bodySm.copyWith(color: DroidTheme.textSecondary)),
              ],
            ),
          ),
          const SizedBox(height: 16),
          _card(
            Column(
              children: [
                for (var i = 0; i < family.checks.length; i++) ...[
                  if (i > 0) Divider(height: 1, color: DroidTheme.surfaceBorder.withValues(alpha: 0.5)),
                  _checkRow(family.checks[i]),
                ],
              ],
            ),
          ),
          const SizedBox(height: 12),
          Wrap(
            spacing: 16,
            runSpacing: 6,
            children: [
              for (final s in GpuStatus.values) _legend(s),
            ],
          ),
          const SizedBox(height: 24),
          _card(
            Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text('Test your device',
                    style: DroidTheme.headingSm.copyWith(color: DroidTheme.textPrimary)),
                const SizedBox(height: 8),
                Text(
                  'With the desktop running, run droiddesk-gpu-report in the terminal (or ask Claude '
                  'Code or Codex to run it). It tests OpenGL, Vulkan and WebGL in your browsers and '
                  'writes a report to send us as a GitHub issue. It changes no settings.',
                  style: DroidTheme.bodySm.copyWith(color: DroidTheme.textSecondary),
                ),
                const SizedBox(height: 12),
                Wrap(
                  spacing: 12,
                  runSpacing: 8,
                  children: [
                    OutlinedButton.icon(
                      onPressed: () => _copy(context, 'droiddesk-gpu-report'),
                      icon: const Icon(Icons.copy_rounded, size: 16),
                      label: const Text('Copy command'),
                    ),
                    OutlinedButton.icon(
                      onPressed: () => _copy(context, _issues),
                      icon: const Icon(Icons.link_rounded, size: 16),
                      label: const Text('Copy issues link'),
                    ),
                  ],
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }

  static void _copy(BuildContext context, String text) {
    Clipboard.setData(ClipboardData(text: text));
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('Copied: $text')));
  }

  static Widget _card(Widget child) => Container(
        padding: const EdgeInsets.all(16),
        decoration: BoxDecoration(
          color: DroidTheme.cardBg,
          borderRadius: BorderRadius.circular(DroidTheme.radiusMd),
          border: Border.all(color: DroidTheme.surfaceBorder),
        ),
        child: child,
      );

  static Widget _kv(String key, String value) => Padding(
        padding: const EdgeInsets.symmetric(vertical: 2),
        child: Row(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            SizedBox(width: 120, child: Text(key, style: DroidTheme.bodySm)),
            Expanded(
              child: Text(value, style: DroidTheme.monoSm.copyWith(color: DroidTheme.textSecondary)),
            ),
          ],
        ),
      );

  static (IconData, Color, String) _look(GpuStatus s) => switch (s) {
        GpuStatus.works => (Icons.check_circle_rounded, DroidTheme.success, 'Tested, works'),
        GpuStatus.caveat => (Icons.error_outline_rounded, DroidTheme.warning, 'Works with limits'),
        GpuStatus.broken => (Icons.cancel_rounded, DroidTheme.error, 'Broken'),
        GpuStatus.software => (Icons.memory_rounded, DroidTheme.secondary, 'CPU / software'),
        GpuStatus.untested => (Icons.help_outline_rounded, DroidTheme.textMuted, 'Not tested'),
      };

  static Widget _legend(GpuStatus s) {
    final (icon, color, label) = _look(s);
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        Icon(icon, size: 14, color: color),
        const SizedBox(width: 4),
        Text(label, style: DroidTheme.bodySm.copyWith(color: DroidTheme.textMuted)),
      ],
    );
  }

  static Widget _checkRow(GpuCheck check) {
    final (icon, color, label) = _look(check.status);
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 10),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Tooltip(message: label, child: Icon(icon, size: 20, color: color)),
          const SizedBox(width: 12),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(check.feature, style: DroidTheme.bodyMd.copyWith(color: DroidTheme.textPrimary)),
                const SizedBox(height: 2),
                Text(check.detail, style: DroidTheme.bodySm.copyWith(color: DroidTheme.textSecondary)),
                if (check.evidence.isNotEmpty)
                  Padding(
                    padding: const EdgeInsets.only(top: 2),
                    child: Text(check.evidence,
                        style: DroidTheme.bodySm.copyWith(color: DroidTheme.textMuted, fontSize: 11)),
                  ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}
