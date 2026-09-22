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
#
# Google Play burns a versionCode permanently the moment an artefact
# carrying it is uploaded to ANY track, including internal testing —
# it can never be reused, and a later upload must be strictly greater.
# So this mapping has to be both monotonic in semver order and free of
# collisions between a release candidate and the release it becomes.
#
# The trailing two digits are the prerelease band. A final release is
# always a multiple of 100; `X.Y.Z-rc.N` sits at (that value - 100 + N),
# i.e. in the gap between the previous patch's release and its own:
#
#     0.0.51        ->  5100
#     0.0.52-rc.1   ->  5101
#     0.0.52-rc.3   ->  5103
#     0.0.52        ->  5200
#
# which is the ordering Play needs to accept an RC on internal testing
# and then promote the final build over it. (An explicit
# -DBSFCHAT_VERSION_CODE=N override wins over all of this, for the
# hotfix case where a packaging-only rebuild has to outrank an already
# uploaded artefact without consuming a patch number.)
if(NOT DEFINED BSFCHAT_VERSION_CODE OR BSFCHAT_VERSION_CODE STREQUAL "")
    string(REPLACE "." ";" _ver_parts "${BSFCHAT_VERSION_DOTS}")
    list(GET _ver_parts 0 _v_major)
    list(GET _ver_parts 1 _v_minor)
    list(GET _ver_parts 2 _v_patch)
    math(EXPR BSFCHAT_VERSION_CODE
         "${_v_major} * 1000000 + ${_v_minor} * 10000 + ${_v_patch} * 100")

    # Prerelease suffix, if any: everything after the first '-'.
    set(_ver_suffix "")
    if(BSFCHAT_VERSION MATCHES "^[^-]+-(.+)$")
        set(_ver_suffix "${CMAKE_MATCH_1}")
    endif()

    if(_ver_suffix MATCHES "^rc\\.?([0-9]+)$")
        set(_rc_num "${CMAKE_MATCH_1}")
        if(_rc_num LESS 1 OR _rc_num GREATER 99)
            message(FATAL_ERROR
                "Release candidate number ${_rc_num} in BSFCHAT_VERSION="
                "'${BSFCHAT_VERSION}' is outside the 1..99 prerelease band. "
                "Pass -DBSFCHAT_VERSION_CODE explicitly if you really need "
                "more than 99 RCs for one patch.")
        endif()
        math(EXPR BSFCHAT_VERSION_CODE
             "${BSFCHAT_VERSION_CODE} - 100 + ${_rc_num}")
    elseif(NOT _ver_suffix STREQUAL "")
        # -dev.<sha> and friends. These are CI/branch artefacts that are
        # never uploaded, so they simply share the final release's code
        # rather than getting an ordering they cannot have. Uploading one
        # WOULD burn the real release's versionCode, hence the warning.
        # STATUS, not WARNING: every branch and PR build on all three
        # desktop platforms carries a -dev.<sha> version, and none of
        # them can reach Play — CI only produces an .aab on tag pushes,
        # and a tag always resolves to a clean version. Warning here
        # would put a CMake warning in every build in the project for a
        # hazard that only exists if someone uploads by hand.
        message(STATUS
            "BSFCHAT_VERSION='${BSFCHAT_VERSION}' has a non-rc prerelease "
            "suffix, so its Android versionCode (${BSFCHAT_VERSION_CODE}) is "
            "the same as the eventual ${BSFCHAT_VERSION_DOTS} release. Fine "
            "for a sideload or a CI artefact; do NOT upload this build to "
            "Google Play or that versionCode is burned for good.")
    endif()
endif()

# Play rejects versionCode < 1, and the platform caps it at
# 2_100_000_000 (just under INT32_MAX, which is aapt's limit).
if(BSFCHAT_VERSION_CODE LESS 1)
    set(BSFCHAT_VERSION_CODE 1)
endif()
if(BSFCHAT_VERSION_CODE GREATER 2100000000)
    message(FATAL_ERROR
        "Android versionCode ${BSFCHAT_VERSION_CODE} exceeds Play's "
        "2100000000 ceiling.")
endif()

message(STATUS "BSFChat version: ${BSFCHAT_VERSION} "
               "(strict=${BSFCHAT_VERSION_DOTS} "
               "nsis=${BSFCHAT_VERSION_NSIS} "
               "android-code=${BSFCHAT_VERSION_CODE})")
