# Single source of truth for the BSFChat version string.
#
# Resolved in this order:
#   1. -DBSFCHAT_VERSION=X.Y.Z  (CI passes this on tag builds; takes
#                                priority so PR builds + tag builds
#                                stay deterministic)
#   2. `git describe --tags --abbrev=0` in the source tree
#   3. Fallback "0.0.0-dev"
#
# Exposes (in the parent scope after include()):
#   BSFCHAT_VERSION       — full user-visible string. May include a
#                           "-suffix" (e.g. "0.1.0-rc1") for dev/CI
#                           builds. This is what the Updater compares
#                           against GitHub Releases' tag_name, so it
#                           MUST be lower than any real published tag
#                           for an unreleased build.
#   BSFCHAT_VERSION_DOTS  — strict MAJOR.MINOR.PATCH (suffix stripped),
#                           used by project(), MACOSX_BUNDLE_*, and
#                           anywhere Apple/Win32 metadata refuses
#                           non-numeric components.
#   BSFCHAT_VERSION_NSIS  — MAJOR.MINOR.PATCH.0 — NSIS's
#                           VIProductVersion requires exactly four
#                           dotted integers.
#   BSFCHAT_VERSION_CODE  — monotonic integer for Android versionCode.
#                           Mapping leaves room for 99 minors and 99
#                           patches per major; if we ever bump major
#                           past 99 we have a different problem.

# Step 1: honour an explicit -D override (CI uses this).
if(NOT DEFINED BSFCHAT_VERSION OR BSFCHAT_VERSION STREQUAL "")
    # Step 2: derive from git tag in the source tree.
    find_package(Git QUIET)
    if(Git_FOUND)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} describe --tags --abbrev=0
            WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/..
            OUTPUT_VARIABLE _git_tag
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE _git_rc)
        if(_git_rc EQUAL 0 AND _git_tag)
            string(REGEX REPLACE "^v" "" BSFCHAT_VERSION "${_git_tag}")
        endif()
    endif()
endif()

# Step 3: dev fallback.
if(NOT DEFINED BSFCHAT_VERSION OR BSFCHAT_VERSION STREQUAL "")
    set(BSFCHAT_VERSION "0.0.0-dev")
    message(WARNING
        "Falling back to BSFCHAT_VERSION=0.0.0-dev — no git tags found "
        "and no -DBSFCHAT_VERSION override. Updater will treat every "
        "published release as newer than this build, which is fine for "
        "local dev but means CI must always pass -DBSFCHAT_VERSION.")
endif()

# Strict M.N.P for things that refuse suffixes (Apple bundle keys,
# project(), NSIS VIProductVersion).
string(REGEX REPLACE "-.*$" "" BSFCHAT_VERSION_DOTS "${BSFCHAT_VERSION}")

if(NOT BSFCHAT_VERSION_DOTS MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR
        "BSFCHAT_VERSION='${BSFCHAT_VERSION}' does not parse as "
        "MAJOR.MINOR.PATCH[-suffix]. Pass -DBSFCHAT_VERSION explicitly "
        "or tag the repo with a vX.Y.Z release.")
endif()

set(BSFCHAT_VERSION_NSIS "${BSFCHAT_VERSION_DOTS}.0")

# Android versionCode: MAJOR * 1_000_000 + MINOR * 10_000 + PATCH * 100
# The trailing two zero-digits are reserved for hotfix bumps (e.g. a
# packaging-only fix that needs to outrank the just-shipped APK
# without consuming a real patch number).
string(REPLACE "." ";" _ver_parts "${BSFCHAT_VERSION_DOTS}")
list(GET _ver_parts 0 _v_major)
list(GET _ver_parts 1 _v_minor)
list(GET _ver_parts 2 _v_patch)
math(EXPR BSFCHAT_VERSION_CODE
     "${_v_major} * 1000000 + ${_v_minor} * 10000 + ${_v_patch} * 100")
# Play Store rejects versionCode < 1. Floor for the dev fallback.
if(BSFCHAT_VERSION_CODE LESS 1)
    set(BSFCHAT_VERSION_CODE 1)
endif()

message(STATUS "BSFChat version: ${BSFCHAT_VERSION} "
               "(strict=${BSFCHAT_VERSION_DOTS} "
               "nsis=${BSFCHAT_VERSION_NSIS} "
               "android-code=${BSFCHAT_VERSION_CODE})")
