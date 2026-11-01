import 'package:flutter/material.dart';
import 'package:droiddesk/theme/droid_theme.dart';
import 'package:droiddesk/services/platform_bridge.dart';

/// Languages offered for the Linux apps (glibc locale -> name in that language).
const linuxLanguages = <String, String>{
  'en_US.UTF-8': 'English',
  'cs_CZ.UTF-8': 'Čeština',
  'sk_SK.UTF-8': 'Slovenčina',
  'de_DE.UTF-8': 'Deutsch',
  'pl_PL.UTF-8': 'Polski',
  'fr_FR.UTF-8': 'Français',
  'es_ES.UTF-8': 'Español',
  'it_IT.UTF-8': 'Italiano',
};

/// Language of the Linux apps, saved as soon as it is picked (the install sets it up in
/// Debian). Android's language is preselected.
class LinuxLanguagePicker extends StatefulWidget {
  const LinuxLanguagePicker({super.key});

  @override
  State<LinuxLanguagePicker> createState() => _LinuxLanguagePickerState();
}

class _LinuxLanguagePickerState extends State<LinuxLanguagePicker> {
  String? _language;

  @override
  void initState() {
    super.initState();
    DroidDeskPlatform.getDesktopLanguage().then((locale) {
      if (!mounted) return;
      final known = linuxLanguages.containsKey(locale) ? locale : 'en_US.UTF-8';
      setState(() => _language = known);
      // Save it even when untouched, so the install and the session agree.
      DroidDeskPlatform.setDesktopLanguage(known);
    });
  }

  @override
  Widget build(BuildContext context) {
    final language = _language;
    if (language == null) return const SizedBox(height: 48);
    return Row(
      mainAxisAlignment: MainAxisAlignment.center,
      children: [
        Icon(Icons.translate_rounded, size: 18, color: DroidTheme.secondary),
        const SizedBox(width: 8),
        Text('Linux apps:', style: DroidTheme.bodySm.copyWith(color: Colors.white70)),
        const SizedBox(width: 12),
        DropdownButton<String>(
          value: language,
          dropdownColor: const Color(0xFF1E293B),
          underline: const SizedBox.shrink(),
          style: DroidTheme.bodyMd.copyWith(color: Colors.white),
          items: [
            for (final entry in linuxLanguages.entries)
              DropdownMenuItem(value: entry.key, child: Text(entry.value)),
          ],
          onChanged: (value) {
            if (value == null) return;
            setState(() => _language = value);
            DroidDeskPlatform.setDesktopLanguage(value);
          },
        ),
      ],
    );
  }
}
