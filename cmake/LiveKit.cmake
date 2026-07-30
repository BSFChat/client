# LiveKit C++ client SDK — vendored prebuilt, IMPORTED targets, and the
# runtime-library bundling that keeps this from repeating the aom.dll
# incident.
#
# Gated behind BSFCHAT_ENABLE_LIVEKIT, which defaults OFF. With the
# option off this file defines nothing and costs nothing; the normal
# build is byte-identical to a tree without it.
#
# ---------------------------------------------------------------------
# Why this file is careful
# ---------------------------------------------------------------------
# cmake/Dependencies.cmake:19-27 records that BUILD_SHARED_LIBS was
# force-set OFF because non-Qt runtime DLLs are not gathered by
# windeployqt, and aom.dll shipped MISSING from the v0.0.42 installer —
# a released build that had to be recalled. The Windows packaging step
# still copies each non-Qt DLL by name with a `Write-Warning` on failure
# (see .github/workflows/ci.yml, the opus.dll and datachannel*.dll
# blocks), so a missing runtime library produces a *green* CI run and a
# broken installer. That is precisely the failure mode.
#
# LiveKit re-introduces the hazard, and worse than before: the prebuilt
# ships TWO shared libraries, not one.
#
#   liblivekit.dylib / .so / livekit.dll   ~3-6 MB   the C++ API
#   liblivekit_ffi.dylib / .so / .dll      ~18-25 MB the Rust+libwebrtc core
#
# The upstream CMake package is not enough on its own. Its generated
# LiveKitTargets-release.cmake declares exactly one target,
# LiveKit::livekit, and records the ffi library only as the bare string
# `IMPORTED_LINK_DEPENDENT_LIBRARIES_RELEASE "livekit_ffi"` — not a
# target, not a path. CMake's own import-time existence check
# (_cmake_import_check_files_for_LiveKit::livekit) therefore lists only
# liblivekit, and find_package(LiveKit) SUCCEEDS with livekit_ffi absent.
# Linking succeeds too, because liblivekit exports every symbol we call
# and resolves the ffi library lazily at load time. The first sign of
# trouble would be a user launching the app.
#
# So we do not use find_package(). We declare both libraries ourselves,
# assert every file exists at CONFIGURE time, copy both next to the
# executable at BUILD time and assert the copies landed, and add install()
# rules for both. A missing library is a hard error at configure or build,
# never at runtime.
#
# ---------------------------------------------------------------------
# Platform support
# ---------------------------------------------------------------------
# Desktop only: Linux x64/arm64, macOS 12.3+ (arm64 + x64), Windows x64.
# The SDK has no Android or iOS support — no release assets, and
# docs/building.md lists the three desktop platforms only. Mobile voice
# stays on the libdatachannel mesh path.

if(NOT BSFCHAT_ENABLE_LIVEKIT)
    return()
endif()

# Pinned version. Must match scripts/fetch-livekit-sdk.sh. The probe TU
# static_asserts the vendored headers agree, so a half-updated deps/
# directory is a compile error rather than a mystery.
set(BSFCHAT_LIVEKIT_VERSION "1.5.0" CACHE STRING
    "Pinned LiveKit C++ client SDK version")

if(ANDROID OR IOS)
    message(FATAL_ERROR
        "BSFCHAT_ENABLE_LIVEKIT is ON but the target is Android/iOS.\n"
        "livekit/client-sdk-cpp does not support mobile: there are no "
        "mobile release assets and docs/building.md lists Linux, macOS "
        "and Windows only. Mobile voice uses the libdatachannel mesh "
        "path. Configure mobile builds without this option.")
endif()
if(NOT BSFCHAT_ENABLE_VOICE)
    message(FATAL_ERROR
        "BSFCHAT_ENABLE_LIVEKIT requires BSFCHAT_ENABLE_VOICE=ON.")
endif()

# ---------------------------------------------------------------------
# Resolve the vendored triple
# ---------------------------------------------------------------------
if(APPLE)
    if(CMAKE_OSX_ARCHITECTURES)
        set(_lk_arch_probe "${CMAKE_OSX_ARCHITECTURES}")
    else()
        set(_lk_arch_probe "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    if(_lk_arch_probe MATCHES "arm64|aarch64")
        set(_lk_triple "macos-arm64")
    elseif(_lk_arch_probe MATCHES "x86_64|AMD64")
        set(_lk_triple "macos-x64")
    else()
        message(FATAL_ERROR
            "Cannot map macOS architecture '${_lk_arch_probe}' to a LiveKit "
            "release triple. Upstream publishes macos-arm64 and macos-x64 "
            "only; a universal (fat) build is not available as a prebuilt.")
    endif()
    # Upstream states macOS 12.3+. A lower deployment target would link
    # but could fail on symbols the SDK resolves from newer frameworks
    # (it links ScreenCaptureKit, which is 12.3+).
    if(CMAKE_OSX_DEPLOYMENT_TARGET AND CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS "12.3")
        message(FATAL_ERROR
            "LiveKit requires macOS 12.3+ but CMAKE_OSX_DEPLOYMENT_TARGET is "
            "${CMAKE_OSX_DEPLOYMENT_TARGET}.")
    endif()
elseif(WIN32)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_lk_triple "windows-x64")
    else()
        message(FATAL_ERROR
            "LiveKit publishes a windows-x64 prebuilt only; this is a "
            "32-bit configuration.")
    endif()
elseif(UNIX)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(_lk_triple "linux-arm64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        set(_lk_triple "linux-x64")
    else()
        message(FATAL_ERROR
            "LiveKit publishes linux-x64 and linux-arm64 prebuilts only; "
            "this host is '${CMAKE_SYSTEM_PROCESSOR}'.")
    endif()
else()
    message(FATAL_ERROR "Unsupported platform for BSFCHAT_ENABLE_LIVEKIT.")
endif()

set(BSFCHAT_LIVEKIT_TRIPLE "${_lk_triple}")
set(BSFCHAT_LIVEKIT_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/deps/livekit-${_lk_triple}-${BSFCHAT_LIVEKIT_VERSION}")

if(NOT EXISTS "${BSFCHAT_LIVEKIT_ROOT}")
    message(FATAL_ERROR
        "Vendored LiveKit SDK not found at:\n  ${BSFCHAT_LIVEKIT_ROOT}\n"
        "Run:\n  scripts/fetch-livekit-sdk.sh ${_lk_triple}\n"
        "(downloads the pinned v${BSFCHAT_LIVEKIT_VERSION} release asset and "
        "verifies its SHA-256), or configure with "
        "-DBSFCHAT_ENABLE_LIVEKIT=OFF.")
endif()

# ---------------------------------------------------------------------
# Enumerate every file we require, then assert all of them at once
# ---------------------------------------------------------------------
# Deliberately checked as a set with a single combined error: finding out
# about three missing files one configure run at a time is how the
# original DLL bug survived as long as it did.
set(_lk_include_dir "${BSFCHAT_LIVEKIT_ROOT}/include")

if(WIN32)
    # Windows splits import library (link time) from DLL (run time).
    set(_lk_implib      "${BSFCHAT_LIVEKIT_ROOT}/lib/livekit.lib")
    set(_lk_ffi_implib  "${BSFCHAT_LIVEKIT_ROOT}/lib/livekit_ffi.dll.lib")
    set(BSFCHAT_LIVEKIT_RUNTIME     "${BSFCHAT_LIVEKIT_ROOT}/bin/livekit.dll")
    set(BSFCHAT_LIVEKIT_FFI_RUNTIME "${BSFCHAT_LIVEKIT_ROOT}/bin/livekit_ffi.dll")
    set(_lk_required "${_lk_implib}" "${_lk_ffi_implib}"
                     "${BSFCHAT_LIVEKIT_RUNTIME}" "${BSFCHAT_LIVEKIT_FFI_RUNTIME}")
elseif(APPLE)
    set(BSFCHAT_LIVEKIT_RUNTIME     "${BSFCHAT_LIVEKIT_ROOT}/lib/liblivekit.dylib")
    set(BSFCHAT_LIVEKIT_FFI_RUNTIME "${BSFCHAT_LIVEKIT_ROOT}/lib/liblivekit_ffi.dylib")
    set(_lk_required "${BSFCHAT_LIVEKIT_RUNTIME}" "${BSFCHAT_LIVEKIT_FFI_RUNTIME}")
else()
    set(BSFCHAT_LIVEKIT_RUNTIME     "${BSFCHAT_LIVEKIT_ROOT}/lib/liblivekit.so")
    set(BSFCHAT_LIVEKIT_FFI_RUNTIME "${BSFCHAT_LIVEKIT_ROOT}/lib/liblivekit_ffi.so")
    set(_lk_required "${BSFCHAT_LIVEKIT_RUNTIME}" "${BSFCHAT_LIVEKIT_FFI_RUNTIME}")
endif()
list(APPEND _lk_required "${_lk_include_dir}/livekit/room.h"
                         "${_lk_include_dir}/livekit/e2ee.h"
                         "${_lk_include_dir}/livekit/platform_audio.h"
                         "${_lk_include_dir}/livekit/build.h")

set(_lk_missing "")
foreach(_f IN LISTS _lk_required)
    if(NOT EXISTS "${_f}")
        list(APPEND _lk_missing "${_f}")
    endif()
endforeach()
if(_lk_missing)
    string(REPLACE ";" "\n  " _lk_missing_text "${_lk_missing}")
    message(FATAL_ERROR
        "The vendored LiveKit SDK at ${BSFCHAT_LIVEKIT_ROOT} is incomplete.\n"
        "Missing:\n  ${_lk_missing_text}\n"
        "Re-run scripts/fetch-livekit-sdk.sh ${_lk_triple} — it wipes and "
        "re-extracts the whole tree.\n"
        "NOTE: this check exists because upstream's own CMake package does "
        "NOT verify livekit_ffi. Do not weaken it.")
endif()

# ---------------------------------------------------------------------
# IMPORTED targets — openh264 precedent (Dependencies.cmake:103-129),
# adapted for SHARED
# ---------------------------------------------------------------------
# Two targets, both real, so the ffi library is a first-class build
# object rather than an undeclared file that happens to be next door.
add_library(livekit::ffi SHARED IMPORTED)
set_target_properties(livekit::ffi PROPERTIES
    IMPORTED_LOCATION "${BSFCHAT_LIVEKIT_FFI_RUNTIME}")
if(WIN32)
    set_target_properties(livekit::ffi PROPERTIES
        IMPORTED_IMPLIB "${_lk_ffi_implib}")
endif()

add_library(livekit::livekit SHARED IMPORTED)
set_target_properties(livekit::livekit PROPERTIES
    IMPORTED_LOCATION "${BSFCHAT_LIVEKIT_RUNTIME}"
    INTERFACE_INCLUDE_DIRECTORIES "${_lk_include_dir}"
    # Not INTERFACE_LINK_LIBRARIES: the ffi library is loaded through
    # liblivekit's own recorded dependency (macOS install name
    # @rpath/liblivekit_ffi.dylib with LC_RPATH @loader_path; Linux
    # NEEDED liblivekit_ffi.so with RUNPATH $ORIGIN). Putting it on the
    # link line would add an unused direct dependency the linker may
    # strip. We need it PRESENT, which is the bundling function's job.
    IMPORTED_LINK_DEPENDENT_LIBRARIES "livekit::ffi")
if(WIN32)
    set_target_properties(livekit::livekit PROPERTIES
        IMPORTED_IMPLIB "${_lk_implib}")
endif()

set(BSFCHAT_HAVE_LIVEKIT ON)
message(STATUS "LiveKit SDK v${BSFCHAT_LIVEKIT_VERSION} (${_lk_triple}): ${BSFCHAT_LIVEKIT_ROOT}")

# The generated verify script used by the POST_BUILD assertion below.
# Written once at configure time; takes the paths to check as arguments
# so one script serves every target and platform.
set(BSFCHAT_LIVEKIT_VERIFY_SCRIPT
    "${CMAKE_CURRENT_BINARY_DIR}/verify-livekit-runtime.cmake")
file(WRITE "${BSFCHAT_LIVEKIT_VERIFY_SCRIPT}" [==[
# Generated by cmake/LiveKit.cmake. Asserts that every LiveKit runtime
# library actually landed beside the executable / inside the bundle.
#
# This is the aom.dll tripwire. Without it, a POST_BUILD copy that
# silently no-ops (wrong generator expression, renamed upstream asset,
# a stale deps/ tree) produces a build that links, tests, packages and
# ships — and dies on the user's first voice join.
set(_missing "")
foreach(_i RANGE 3 ${CMAKE_ARGC})
    if(DEFINED CMAKE_ARGV${_i})
        set(_p "${CMAKE_ARGV${_i}}")
        if(_p AND NOT EXISTS "${_p}")
            list(APPEND _missing "${_p}")
        endif()
    endif()
endforeach()
if(_missing)
    string(REPLACE ";" "\n  " _text "${_missing}")
    message(FATAL_ERROR
        "LiveKit runtime library missing after build:\n  ${_text}\n"
        "The app would link and package successfully and then fail at "
        "the user's first voice join. Failing the build instead.")
endif()
]==])

# ---------------------------------------------------------------------
# bsfchat_livekit_bundle_runtime(<target>)
# ---------------------------------------------------------------------
# Puts both shared libraries where the loader will find them, verifies
# they arrived, and adds install() rules.
#
#   macOS   -> <target>.app/Contents/Frameworks/, app RPATH
#              @executable_path/../Frameworks. liblivekit.dylib already
#              carries LC_RPATH @loader_path and asks for
#              @rpath/liblivekit_ffi.dylib, so co-locating the two is
#              sufficient — no install_name_tool fix-up needed.
#              Call this BEFORE the codesign POST_BUILD step in
#              CMakeLists.txt so the copied dylibs get signed with the
#              rest of the bundle (they arrive adhoc/linker-signed).
#   Windows -> next to the .exe. The NSIS installer does
#              `File /r "*.*"` from dist\, so anything the CI staging
#              step places beside bsfchat-app.exe is installed.
#   Linux   -> next to the binary, app RUNPATH $ORIGIN. liblivekit.so
#              already has RUNPATH $ORIGIN and NEEDED liblivekit_ffi.so.
#
function(bsfchat_livekit_bundle_runtime target)
    if(NOT BSFCHAT_HAVE_LIVEKIT)
        return()
    endif()

    # macOS has two shapes: the .app bundle (libraries belong in
    # Contents/Frameworks) and a plain executable such as a test binary
    # (libraries sit beside it). Asking for TARGET_BUNDLE_CONTENT_DIR on a
    # non-bundle target expands to nothing and would silently copy to the
    # wrong place, so branch on the actual property.
    get_target_property(_lk_is_bundle ${target} MACOSX_BUNDLE)
    if(APPLE AND _lk_is_bundle)
        set(_dest "$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Frameworks")
        set_property(TARGET ${target} APPEND PROPERTY
            BUILD_RPATH "@executable_path/../Frameworks")
        set_property(TARGET ${target} APPEND PROPERTY
            INSTALL_RPATH "@executable_path/../Frameworks")
    elseif(APPLE)
        set(_dest "$<TARGET_FILE_DIR:${target}>")
        set_property(TARGET ${target} APPEND PROPERTY
            BUILD_RPATH "@executable_path")
        set_property(TARGET ${target} APPEND PROPERTY
            INSTALL_RPATH "@executable_path")
    else()
        set(_dest "$<TARGET_FILE_DIR:${target}>")
        if(NOT WIN32)
            set_property(TARGET ${target} APPEND PROPERTY
                BUILD_RPATH "$ORIGIN")
            set_property(TARGET ${target} APPEND PROPERTY
                INSTALL_RPATH "$ORIGIN")
        endif()
    endif()

    cmake_path(GET BSFCHAT_LIVEKIT_RUNTIME FILENAME _lk_runtime_name)
    cmake_path(GET BSFCHAT_LIVEKIT_FFI_RUNTIME FILENAME _lk_ffi_runtime_name)

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_dest}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${BSFCHAT_LIVEKIT_RUNTIME}" "${BSFCHAT_LIVEKIT_FFI_RUNTIME}" "${_dest}"
        # Assert, do not assume. See the note in the verify script.
        COMMAND ${CMAKE_COMMAND} -P "${BSFCHAT_LIVEKIT_VERIFY_SCRIPT}"
                "${_dest}/${_lk_runtime_name}"
                "${_dest}/${_lk_ffi_runtime_name}"
        COMMENT "Bundling LiveKit runtime libraries into ${_dest}"
        VERBATIM)

    # install() rules so any packaging path that uses them (rather than
    # the current hand-rolled CI copy steps) gets both libraries.
    if(APPLE AND _lk_is_bundle)
        install(FILES "${BSFCHAT_LIVEKIT_RUNTIME}" "${BSFCHAT_LIVEKIT_FFI_RUNTIME}"
                DESTINATION "$<TARGET_FILE_BASE_NAME:${target}>.app/Contents/Frameworks")
    elseif(WIN32)
        install(FILES "${BSFCHAT_LIVEKIT_RUNTIME}" "${BSFCHAT_LIVEKIT_FFI_RUNTIME}" DESTINATION bin)
    else()
        install(FILES "${BSFCHAT_LIVEKIT_RUNTIME}" "${BSFCHAT_LIVEKIT_FFI_RUNTIME}" DESTINATION bin)
    endif()

    # Machine-readable manifest of the runtime files that MUST ship.
    # The Windows installer staging step and the Linux tarball step in
    # .github/workflows/ci.yml currently name each non-Qt runtime
    # library by hand and only Write-Warning when one is absent — which
    # is how aom.dll shipped missing. A packaging step can read this
    # file and hard-fail instead of guessing filenames.
    file(WRITE "${CMAKE_BINARY_DIR}/livekit-runtime-manifest.txt"
        "${_lk_runtime_name}\n${_lk_ffi_runtime_name}\n")
endfunction()
