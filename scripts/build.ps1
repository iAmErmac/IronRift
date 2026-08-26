param([ValidateSet('Build','Install')][string]$Action='Build')
$ErrorActionPreference='Stop'
$root = Split-Path -Parent $PSScriptRoot
$android = Join-Path $root 'Projects\Android'
$env:GRADLE_USER_HOME = if ($env:GRADLE_USER_HOME) { $env:GRADLE_USER_HOME } else { Join-Path $root '.gradle-mobile' }
$gradle = Get-Command gradle -ErrorAction SilentlyContinue
if (-not $gradle) { throw 'gradle was not found on PATH; install Gradle or configure Android Studio Gradle.' }
Push-Location $android
try { & $gradle.Source --no-daemon assembleDebug; if ($LASTEXITCODE -ne 0) { throw "Gradle failed: $LASTEXITCODE" } } finally { Pop-Location }
$apk = Get-ChildItem (Join-Path $android 'build\outputs\apk\debug') -Filter *.apk | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $apk) { throw 'Debug APK not found.' }
$archiveDir = Join-Path $root 'build'
New-Item -ItemType Directory -Force -Path $archiveDir | Out-Null
$archiveApk = Join-Path $archiveDir 'ironrift-vr-arm64-debug.apk'
Copy-Item -LiteralPath $apk.FullName -Destination $archiveApk -Force
Write-Host "APK: $($apk.FullName)"
Write-Host "Archive: $archiveApk"
if ($Action -eq 'Install') { $adb = Get-Command adb -ErrorAction Stop; & $adb.Source install -r $apk.FullName; if ($LASTEXITCODE -ne 0) { throw "adb install failed: $LASTEXITCODE" } }
