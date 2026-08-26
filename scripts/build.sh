#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ANDROID="$ROOT/Projects/Android"
export GRADLE_USER_HOME="${GRADLE_USER_HOME:-$ROOT/.gradle-mobile}"
GRADLE_BIN="${GRADLE_BIN:-gradle}"
"$GRADLE_BIN" --no-daemon -p "$ANDROID" assembleDebug
APK="$(find "$ANDROID/build/outputs/apk/debug" -name '*.apk' -type f | sort | tail -n 1)"
test -n "$APK"
ARCHIVE_DIR="$ROOT/build"
mkdir -p "$ARCHIVE_DIR"
ARCHIVE_APK="$ARCHIVE_DIR/ironrift-vr-arm64-debug.apk"
cp "$APK" "$ARCHIVE_APK"
printf 'APK: %s\n' "$APK"
printf 'Archive: %s\n' "$ARCHIVE_APK"
if [[ "${1:-}" == "Install" ]]; then adb install -r "$APK"; fi
