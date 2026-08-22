<img src="https://github.com/iAmErmac/IronRift/blob/android/Projects/Android/src/main/res/drawable-nodpi/ironwail_icon.png" width="256" height="256" alt="IronRift">

<a href="https://github.com/iAmErmac/IronRift/releases/latest">![GitHub Release](https://img.shields.io/github/v/release/iAmErmac/IronRift?display_name=release&style=for-the-badge&label=Download)</a>

# What's this?

IronRift is the Android host application for [Ironwail](https://github.com/iAmErmac/ironwail), a high-performance Quake engine derived from [QuakeSpasm](https://sourceforge.net/projects/quakespasm/). This project provides the flat-screen Android Activity, native GLES context, surface and lifecycle handling, touch controls, audio integration, and APK packaging. The Ironwail engine and renderer are pinned from the `openxr` branch as a submodule under `Projects/Android/jni/ironwail`.

The renderer targets OpenGL ES 3.1 as its baseline and uses ordinary GLES VBOs, EBOs, VAOs, UBOs, and direct indexed draws.

> [!IMPORTANT]
> This project uses AI assistance during development. We care most about the results and how well the software works, rather than where each line of code came from. If AI-generated code is a philosophical deal-breaker for you, this project probably is not a good fit, and we respectfully suggest looking elsewhere.

## Does performance still matter on a phone?

It does when a map has a lot going on. Modern Quake releases and community maps can push far more geometry, models, particles, and translucent surfaces than the original levels. Android devices also vary widely, so the renderer keeps the base path conservative and measures the work that matters on real hardware.

The desktop renderer can stream model data through SSBOs and GPU-generated indirect draw lists. That approach is not a good fit for Adreno’s tiled/binning architecture: streamed SSBO vertex data can increase synchronization and binning pressure even when it reduces CPU draw setup. The Android build therefore keeps persistent world and model meshes in static VAOs, streams transient GUI and particle data through rotating VBO/EBO buffers, and uses aligned UBO ranges for per-object data. Opaque world surfaces can be combined into bounded indexed batches where ordering and material state allow it.

## Features

- native OpenGL ES rendering on Android
- touch controls for movement, looking, attack, jump, use, menus, and console access
- keyboard and gamepad input alongside touch controls when the device supports them
- a *Mods* menu for installed add-ons
- configurable weapon bindings
- alternative HUD styles based on the Q64 layout
- real-time palettization with optional dithering
- classic underwater warp effect
- lightmapped liquid surfaces
- smoothly interpolated lightstyles
- reduced heap usage and loading time for large maps
- higher color/depth precision to reduce banding and z-fighting artifacts

## Getting started

Clone IronRift with all submodules so the matching Ironwail engine is fetched with it:

```text
git clone --recurse-submodules https://github.com/iAmErmac/IronRift.git
git -C IronRift submodule update --init --recursive
```

If the repository was cloned without submodules, run the second command before building. `Projects/Android/jni/ironwail` is pinned to the `openxr` branch.

## Building the APK

The repository includes build helpers that build the native library and package the debug APK. From the IronRift repository root:

```text
scripts/build.ps1
scripts/build.ps1 Install
```

On a shell environment:

```text
./scripts/build.sh
./scripts/build.sh Install
```

The direct Gradle form is also available from `Projects/Android`:

```text
./gradlew assembleDebug
```

The build uses the Android SDK, NDK 27.2.12479018, Gradle 8.2.1, and produces an arm64-v8a APK under `Projects/Android/build/outputs/apk/debug/`. For a clean rebuild, use `./gradlew clean assembleDebug` from that directory.

With a USB-debugging-enabled device or emulator connected:

```text
adb devices
adb install -r Projects/Android/build/outputs/apk/debug/ironrift-debug.apk
adb shell appops set com.ermac.ironwail MANAGE_EXTERNAL_STORAGE allow
adb shell am start -n com.ermac.ironwail/.GLES3JNIActivity
```

The AppOps command is optional when the permission was already granted through Android settings. It requests the app’s “All files access” capability, which is needed for the shared Quake working directory.

## Quake data and storage permission

The app uses `/sdcard/ironwail` as its working directory. Grant IronRift full file-system access (“All files access”) in Android settings, or use the `adb shell appops` command above. Without this permission the app may start but will not find the game data correctly.

Copy the Quake data directories into `/sdcard/ironwail` itself:

```text
/sdcard/ironwail/id1
/sdcard/ironwail/hipnotic
/sdcard/ironwail/rogue
/sdcard/ironwail/dopa
/sdcard/ironwail/mg1
/sdcard/ironwail/mg3
```

Keep each directory’s original Quake layout and do not add an extra nesting level. For a development launch, `/sdcard/ironwail/commandline.txt` can contain arguments such as:

```text
ironwail +game id1 +map start
```

You need a legally obtained Quake installation or data set; IronRift does not provide the game data.

## System requirements

| | Minimum | Recommended |
|:--|:--|:--|
|Android|Android 10 / API 29, arm64-v8a, OpenGL ES 3.1|A recent Snapdragon/Adreno device with ample memory and sustained performance|
|Storage|Full file-system access and Quake data under `/sdcard/ironwail`|Fast internal storage for shorter map loads|
|Controls|Touchscreen|Touchscreen plus keyboard or gamepad|

Notes:
1. Android API 29 is the current APK minimum. OpenGL ES 3.1 is the renderer compatibility baseline; GLES 3.2 and other extensions are optional runtime tiers.
2. Android performance depends heavily on resolution, thermal state, driver version, and the map being played. Emulator timings are useful for regression checks, not as a substitute for a physical phone.