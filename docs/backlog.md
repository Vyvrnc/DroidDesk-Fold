# Backlog

Agreed items not yet scheduled for a release. Windows apps have their own plan:
`docs/windows-apps-plan.md`.

## Stability (from the Fold 7 crashes on 2026-10-20)

1. **PulseAudio watchdog.** The desktop start script starts PulseAudio once
   (`LinuxRuntime.kt`, before `exec xfce4-session`); nothing restarts it. When it dies, Debian apps
   lose sound (`127.0.0.1:4713` refused) until the desktop is restarted. DroidDeskService should
   check it every ~20 s like the X server (`pactl list short sinks` has `AAudio_sink`, with
   `timeout 2`) and restart it natively with the same arguments. It cannot be started from Debian:
   proot runs as uid 0 and PulseAudio refuses root.

2. **Keep `mapping.txt` from every build.** Two crashes on 2026-10-20 (12:27, 12:55, fold.63) were
   an uncaught `java.lang.InterruptedException` from `Thread.sleep` on a background thread, R8 frame
   `f0.k.f(SourceFile:117)` via `K0.a.run`. No `Thread.sleep` at line 117 exists in our sources, so
   it is probably a library. `tools/build-apk.sh` should copy
   `build/app/outputs/mapping/release/mapping.txt` next to the APK
   (`~/DroidDesk-build/mapping-<version>.txt`), then retrace this stack and fix the caller.

3. **Warn when the phantom process limit is on.** On the Fold 7
   `persist.sys.fflag.override.settings_enable_monitor_phantom_procs` came back as `true` although
   the developer option had been set, and Android trimmed the session's child processes every
   5 minutes ("Trimming phantom processes": XFCE, Wine, proot, Claude). At app start, read the flag
   and `max_phantom_processes`; if the limit applies, show a notice with the fix (developer option,
   or the adb commands below). Fix applied by adb on 2026-10-20, survives reboots:
   ```
   settings put global settings_enable_monitor_phantom_procs false
   device_config set_sync_disabled_for_tests persistent
   device_config put activity_manager max_phantom_processes 2147483647
   ```
   Rollback: `settings delete global settings_enable_monitor_phantom_procs`,
   `device_config delete activity_manager max_phantom_processes`,
   `device_config set_sync_disabled_for_tests none`.

4. **Why the app was cached during a build.** `dumpsys activity exit-info` shows two kills at
   11:26 and 11:51 with reason `EXCESSIVE RESOURCE USAGE / EXCESSIVE CPU USAGE`, "Caused by child
   process: excessive cpu … limit=25", importance 400 (cached). With DroidDeskService in the
   foreground the app should not be cached. Check which service type and state it had at that time
   (`dumpsys activity services com.orailnoor.droiddesk`, `dataSync` 6 h limit on Android 15+) and
   keep the app out of the cached state while a session runs.
