# Android

## Dependencies

* [Android Studio](https://developer.android.com/studio)
* [NDK 28.2.13676358 and CMake 3.31.6](https://developer.android.com/studio/projects/install-ndk#default-version) (versions are pinned in `src/android/app/build.gradle.kts`)
* [Git](https://git-scm.com/download)

## WINDOWS ONLY - Additional Dependencies

* **[Visual Studio 2022 Community](https://visualstudio.microsoft.com/downloads/)** - **Make sure to select "Desktop development with C++" support in the installer. Make sure to update to the latest version if already installed.**
* **[Vulkan SDK](https://vulkan.lunarg.com/sdk/home#windows)** - **Make sure to select Latest SDK.**
  * A convenience script to install the latest SDK is provided in `.ci\windows\install-vulkan-sdk.ps1`.

## Cloning Eden with Git

```sh
git clone --recursive https://git.eden-emu.dev/eden-emu/eden.git
```

Eden by default will be cloned into:

* `C:\Users\<user-name>\eden` on Windows
* `~/eden` on Linux and macOS

## Building

1. Start Android Studio, on the startup dialog select `Open`.
2. Navigate to the `eden/src/android` directory and click on `OK`.
3. In `Build > Select Build Variant`, select `release` or `relWithDebInfo` as the "Active build variant".
4. Build the project with `Build > Make Project` or run it on an Android device with `Run > Run 'app'`.

## Building with Terminal

1. Download the SDK and NDK from Android Studio.
2. Navigate to SDK and NDK paths.
3. Then set ANDROID_SDK_ROOT and ANDROID_NDK_ROOT in terminal via
`export ANDROID_SDK_ROOT=path/to/sdk`
`export ANDROID_NDK_ROOT=path/to/ndk`.
4. Navigate to `src/android`.
5. Then build with `./gradlew assembleMainlineRelWithDebInfo`.
6. To build the optimised build use `./gradlew assembleGenshinSpoofRelWithDebInfo`.
7. You can pass extra variables to cmake via `-PYUZU_ANDROID_ARGS="-D..."`

Remember to have a Java SDK installed if not already, on Debian and similar this is done with `sudo apt install openjdk-17-jdk`.

### Script

A convenience script for building is provided in `.ci/android/build.sh`. On Windows, this must be run in Git Bash or MSYS2. This script provides the following options:

```txt
Usage: build.sh [-t|--target FLAVOR] [-b|--build-type BUILD_TYPE]
                [-h|--help] [-r|--release] [-n|--nightly] [extra options]

Build script for Android.
Associated variables can be set outside the script,
and will apply both to this script and the packaging script.
bool values are "true" or "false"

Options:
    -r, --release           Enable update checker. If set, sets the DEVEL bool variable to false.
                            By default, DEVEL is true.
    -t, --target <FLAVOR>   Build flavor (variable: TARGET)
                            Valid values are: legacy, optimized, standard, chromeos
                            Default: standard
    -b, --build-type <TYPE> Build type (variable: TYPE)
                            Valid values are: Release, RelWithDebInfo, Debug
                            Default: Release
    -n, --nightly           Create a nightly build.

Extra arguments are passed to CMake (e.g. -DCMAKE_OPTION_NAME=VALUE)
Set the CCACHE variable to "true" to enable build caching.
The APK and AAB will be output into "artifacts".
```

Examples:

* Build legacy release with update checker:
  * `.ci/android/build.sh -r -t legacy`
* Build standard release with debug info without update checker for phones:
  * `.ci/android/build.sh -b RelWithDebInfo`
* Build optimized release with update checker:
  * `.ci/android/build.sh -r -t optimized`
* Build for ChromeOS (x86_64):
  * `.ci/android/build.sh -t chromeos`

### Signing release builds

Release builds made without a release keystore are silently signed with the
committed debug key. They install and run fine for local testing, but they
**cannot be distributed**: anyone holding the public debug key could forge
updates for your app, and app stores will reject them.

1. Generate a release keystore (once — guard this file with your life; losing
   it means you can never publish updates for the same app ID again):
   ```sh
   keytool -genkeypair -v -keystore suyu-release.jks -keyalg RSA \
     -keysize 4096 -validity 10950 -alias suyu-release
   ```
   The long validity matters: app stores require the key to stay valid well
   past 2033.
2. Never commit `*.jks` / `*.keystore` files (already git-ignored).
3. Build locally with the keystore:
   ```sh
   export ANDROID_KEYSTORE_FILE="$PWD/suyu-release.jks"
   export ANDROID_KEYSTORE_PASS="<keystore password>"
   export ANDROID_KEY_ALIAS="suyu-release"
   ./.ci/android/build.sh -t standard -b Release -r
   ```
   Without these variables the build still succeeds but prints a warning that
   the APK/AAB is debug-signed.
4. For GitHub releases, add three repository secrets and the workflow picks
   them up automatically (the "Verify APK signatures" step flags any APK
   still carrying the debug certificate):
   * `ANDROID_KEYSTORE_B64` — base64 of the keystore:
     `base64 -i suyu-release.jks | pbcopy` (macOS) or
     `base64 -w0 suyu-release.jks` (Linux)
   * `ANDROID_KEY_ALIAS`
   * `ANDROID_KEYSTORE_PASS`
5. Sanity-check any APK before distributing it:
   ```sh
   apksigner verify --print-certs app.apk
   ```
   The SHA-256 digest must **not** be the debug certificate
   (`9D:09:B2:00:…:0D:7A:F1`, the key in `src/android/app/debug.keystore`).

If you use Play App Signing, the key above becomes your *upload* key: keep the
keystore backed up offline even though Google holds the final app signing key.

### Additional Resources

<https://developer.android.com/studio/intro>
