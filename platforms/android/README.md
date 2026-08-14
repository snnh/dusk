# Android Shell

This directory contains Dusklight's Android shell built on top of Borealis.

## Prerequisites

- Android SDK installed (`ANDROID_HOME`)
- Android NDK version used by CMake presets (`ANDROID_NDK_VERSION`)
- JDK 17+

Example:

```bash
export ANDROID_HOME="$HOME/Android/Sdk"
export ANDROID_NDK_VERSION="29.0.14206865"
export JAVA_HOME="/usr/lib/jvm/java-17-openjdk"
```

## Build Native Libraries

```bash
cmake --preset android-arm64
cmake --build --preset android-arm64
```

This build produces `build/android-arm64/libmain.so`

## Build APK

```bash
cd platforms/android
./gradlew :app:assembleDebug
```

Output APK:

- `app/build/outputs/apk/debug/app-arm64-v8a-debug.apk`

Aurora needs a hardware-backed graphics adapter. If an AVD has GPU
acceleration disabled, launch it with `-gpu host`.

## Sign Release APKs

Release builds are signed automatically. If no signing variables are present,
Gradle uses the default Android debug key so the APK can be installed for user
testing.

To sign with a custom keystore, provide:

```bash
export DUSK_ANDROID_KEYSTORE_FILE="/path/to/release.keystore"
export DUSK_ANDROID_KEYSTORE_PASSWORD="..."
export DUSK_ANDROID_KEY_ALIAS="..."
export DUSK_ANDROID_KEY_PASSWORD="..."
./gradlew :app:assembleRelease
```

CI can optionally use `DUSK_ANDROID_KEYSTORE_BASE64` instead of
`DUSK_ANDROID_KEYSTORE_FILE`. Store these as GitHub Actions secrets when custom
signing is needed:

- `DUSK_ANDROID_KEYSTORE_BASE64`
- `DUSK_ANDROID_KEYSTORE_PASSWORD`
- `DUSK_ANDROID_KEY_ALIAS`
- `DUSK_ANDROID_KEY_PASSWORD`

## Launch With Runtime Args (adb)

You can pass command-line args through the activity intent:

```bash
adb shell am start -n dev.twilitrealm.dusk/.DuskActivity \
  --es borealis_args "--backend vulkan"
```

Supported extras:

- `borealis_args`: single shell-like argument string
- `borealis_argv`: string-array argv

The legacy `dusk_args` and `dusk_argv` names remain accepted during the shell
transition.
