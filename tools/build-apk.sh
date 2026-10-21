#!/usr/bin/env bash
# Builds the release APK from the committed HEAD in a Flutter Docker image and
# checks its signature. Run from the repo root in Git Bash (Windows) or bash.
#
#   WORK_DIR   build scratch + output (default: ~/DroidDesk-build, keep it out of synced folders)
#   KEY_DIR    directory holding debug.keystore used for signing (default: $WORK_DIR/android-key)
#
# The keystore is the update identity of every installed copy: lose it and the
# next release can only be installed after uninstalling, which deletes Linux.
set -euo pipefail

WORK_DIR="${WORK_DIR:-$HOME/DroidDesk-build}"
KEY_DIR="${KEY_DIR:-$WORK_DIR/android-key}"
IMAGE=ghcr.io/cirruslabs/flutter:stable
[ -f "$KEY_DIR/debug.keystore" ] || { echo "missing $KEY_DIR/debug.keystore" >&2; exit 1; }
mkdir -p "$WORK_DIR"

# autocrlf=false: with core.autocrlf=true (Git for Windows) git archive writes CRLF, and the
# scripts the app copies into Linux then fail ("set: pipefail: invalid option name").
git -c core.autocrlf=false archive --format=tar HEAD app > "$WORK_DIR/src.tar"
cat > "$WORK_DIR/build.sh" <<'EOF'
set -e
mkdir -p /work && cd /work && tar xf /out/src.tar
cd /work/app
crlf=$(grep -rlI $'\r' android/app/src/main/assets/droiddesk || true)
[ -z "$crlf" ] || { echo "CRLF in APK assets: $crlf" >&2; exit 1; }
yes | sdkmanager --licenses >/dev/null 2>&1 || true
sdkmanager "cmake;3.22.1" >/dev/null
flutter pub get
flutter build apk --release
cp build/app/outputs/flutter-apk/app-release.apk /out/DroidDesk-fold-dex.apk
B=$(ls -d /opt/android-sdk-linux/build-tools/*/ | tail -1)
${B}apksigner verify --print-certs /out/DroidDesk-fold-dex.apk | grep SHA-256
${B}aapt2 dump badging /out/DroidDesk-fold-dex.apk | head -1
sha256sum /out/DroidDesk-fold-dex.apk
EOF

# cygpath keeps Docker Desktop happy with Windows paths; plain paths elsewhere.
to_host() { command -v cygpath >/dev/null && cygpath -m "$1" || echo "$1"; }
MSYS_NO_PATHCONV=1 docker run --rm \
  -v "$(to_host "$WORK_DIR"):/out" \
  -v "$(to_host "$KEY_DIR"):/root/.android" \
  -v droiddesk-gradle:/root/.gradle -v droiddesk-pub:/root/.pub-cache \
  "$IMAGE" bash /out/build.sh
echo "APK: $WORK_DIR/DroidDesk-fold-dex.apk"
