# BSFChat client

Qt 6 / QML desktop + mobile client. C++20, CMake + Ninja.

## Dependencies

Most third-party code is pulled by CMake via `FetchContent` at configure
time (libdatachannel, opus, libaom, googletest). The ones that are *not* —
OpenSSL, openh264 and the LiveKit SDK — are built once into `deps/`, which
is gitignored:

```sh
scripts/build-openh264.sh          # H.264 software codec (macOS/Linux)
scripts/build-openssl-android.sh   # Android only
scripts/build-openssl-ios.sh       # iOS only
scripts/fetch-livekit-sdk.sh       # only with -DBSFCHAT_ENABLE_LIVEKIT=ON
```

Qt itself comes from Homebrew locally (`/opt/homebrew/lib/cmake/Qt6`) and
from `jurplel/install-qt-action` in CI. Also needed: `cmake`, `ninja`,
`ccache`, `nasm` (libaom's assembly).

> Working in a git worktree? `deps/` is ignored, so it is not copied in.
> Symlink it: `ln -s /path/to/client/deps deps`.

## Build

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DQt6_DIR=/opt/homebrew/lib/cmake/Qt6 -Wno-dev
cmake --build build -j3
```

Or, equivalently, `cmake --preset macos-dev && cmake --build --preset
macos-dev` (see `CMakePresets.json`, which also carries a `macos-asan`
preset matching the CI sanitizer job).

`CMAKE_CXX_COMPILER_LAUNCHER` is optional: `cmake/Ccache.cmake` defaults it
to `ccache` when ccache is on `PATH`. Pass it explicitly to use something
else, or `-DBSFCHAT_USE_CCACHE=OFF` to opt out. The first build is cold and
takes tens of minutes (libaom and libdatachannel dominate); later ones are
minutes.

## Tests

```sh
ctest --test-dir build --output-on-failure -j4
```

Tests must be parallel-safe — CI runs `ctest` with `-j`.

## Running two clients at once

The client namespaces everything it persists off its application name, so
by default two instances fight over one settings file, one cache, one log
and one single-instance lock. `--profile <name>` (or `$BSFCHAT_PROFILE`)
suffixes all of them at once:

```sh
# terminal 1 — server from the repo root
./server/build/bsfchat-server --config server.dev.toml

# terminal 2 / 3 — two independent clients
open -n build/BSFChat.app --args --profile alice
open -n build/BSFChat.app --args --profile bob
```

With a profile, one instance uses:

| | default | `--profile alice` |
|---|---|---|
| application name | `BSFChat` | `BSFChat-alice` |
| settings | `~/Library/Preferences/com.bsfchat.BSFChat.plist` | `~/Library/Preferences/com.bsfchat.BSFChat-alice.plist` |
| cache (`LocalCache`) | `~/Library/Application Support/BSFChat/BSFChat/cache` | `~/Library/Application Support/BSFChat/BSFChat-alice/cache` |
| log | `~/Library/Logs/BSFChat/bsfchat.log` | `~/Library/Logs/BSFChat-alice/bsfchat.log` |
| single-instance / `bsfchat://` socket | `bsfchat-url-ipc-<user>` | `bsfchat-url-ipc-<user>-alice` |

No flag means no suffix anywhere, so existing installs keep their settings,
cache and logins untouched. The resolution logic lives in
`src/core/AppProfile.cpp` and is covered by `tests/test_profile.cpp`
(`ctest -R test_profile`) — including the "default is unchanged" guarantee.

Profile names are sanitized: anything outside `[A-Za-z0-9._-]` becomes
`_`, leading/trailing separators are trimmed, and the result is capped at
32 characters, so a profile cannot escape its directory.

## Logs

macOS: `~/Library/Logs/BSFChat/bsfchat.log` (rotating, 5 MB × 3
generations; `~/Library/Logs/BSFChat-<profile>/` with a profile).
Elsewhere: `<AppDataLocation>/logs`. Settings → Advanced shows the path,
and the file logger is installed before anything else runs, so a
Finder-launched app still leaves a trace.

For voice/video debugging, turn on Settings → Advanced → verbose voice
logging (equivalently `QT_LOGGING_RULES="bsfchat.*=true"`).

## Do not launch the GUI from an agent session

Running `bsfchat-app` / `BSFChat.app` on this Mac sets off macOS TCC
permission dialogs (microphone, camera, screen recording, local network),
and a rebuilt binary re-triggers them because the signature changed. Build
and test freely; leave launching to a human, and warn before a post-rebuild
launch. Everything in this repo is verifiable with `ctest` — the
`--profile` path logic included.
