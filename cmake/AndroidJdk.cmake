# Picks the JDK that Gradle will use for the Android build, and hands
# the choice to Gradle so nobody has to set JAVA_HOME by hand.
#
# WHY THIS EXISTS
#
# androiddeployqt shells out to Gradle, and Gradle runs *everything* —
# javac, D8/R8, the AGP plugin itself — inside its daemon JVM. So one
# JDK choice decides whether the APK links or dies at the last step.
# The machine default is the wrong choice more often than not:
#
#   * AGP 7.4.1 (what Qt 6.5's build.gradle template pins, i.e. every
#     local build on this machine) bundles a D8 that cannot parse class
#     files newer than Java 17's. Compile the Qt bindings with a JDK 26
#     javac and `:dexBuilderRelease` fails with
#         D8: java.lang.NullPointerException:
#             Cannot invoke "String.length()" because "<parameter1>" is null
#     on QtLoader$1/$2 and our own ScreenCaptureHelper$1/$2/$4 — i.e. on
#     anonymous inner classes, with nothing in the message naming the
#     JDK. A completely healthy C++/QML build appears to fail at the
#     packaging step for no reason.
#   * Gradle 8.0 (the wrapper Qt 6.5 ships) refuses to run on anything
#     past Java 19 at all.
#   * AGP 8.10.1 (Qt 6.10, which CI uses) needs at least Java 17.
#
# Intersect those and exactly one version is left: 17. Hence the default
# window below is 17..17 rather than a range — a JDK 18 or 19 would get
# past Gradle and then hit the same D8 wall, so widening the ceiling
# only makes sense together with the AGP that can cope with it. CI pins
# 17 too, via actions/setup-java.
#
# HOW THE CHOICE REACHES GRADLE
#
# Not through the environment — the `apk` target is a custom command Qt
# generates, and it inherits whatever shell the build was started from.
# Instead we write `org.gradle.java.home` into gradle.properties, which
# picks the daemon JVM and therefore also the javac and D8 that run
# inside it.
#
# androiddeployqt generates android-build/gradle.properties, so we
# cannot edit it after the fact. But it builds that file in a fixed
# order: the Qt template is copied first, then QT_ANDROID_PACKAGE_SOURCE_DIR
# is copied over the top (forceOverwrite=true), and only then does
# mergeGradleProperties() rewrite the file — preserving every line whose
# key it does not own. So a gradle.properties inside the package source
# dir survives into the build, and keeps surviving across rebuilds.
#
# "Over the top" is per FILE, not per key: our gradle.properties does
# not merge with the Qt template, it REPLACES it. So the staged file
# must be the template PLUS our line, never our line alone — see the
# seeding step below, and what breaks without it.
#
# That dir is `android/`, which is checked in, and the JDK path is
# machine-specific — so we stage a copy of `android/` into the build
# tree and add the generated gradle.properties there. Everything under
# `android/` is registered as a configure dependency, so editing the
# manifest or the Java sources re-runs CMake and refreshes the copy.
#
# EXPOSES (in the parent scope after include()):
#   BSFCHAT_ANDROID_JDK_HOME            — the JDK Gradle will use, or ""
#                                         if no supported one was found
#   BSFCHAT_ANDROID_PACKAGE_SOURCE_DIR  — staged dir to hand to
#                                         QT_ANDROID_PACKAGE_SOURCE_DIR

# The supported window. Both are cache variables because the floor and
# the ceiling come from the toolchain, not from us: raise
# BSFCHAT_ANDROID_JDK_MAX once the local Qt kit moves past 6.5 and
# brings an AGP whose D8 reads newer class files.
set(BSFCHAT_ANDROID_JDK_MIN "17" CACHE STRING
    "Oldest JDK accepted for the Android Gradle build (AGP 8 needs 17)")
set(BSFCHAT_ANDROID_JDK_MAX "17" CACHE STRING
    "Newest JDK accepted for the Android Gradle build (AGP 7.4.1's D8 stops at 17)")
set(BSFCHAT_ANDROID_JDK "" CACHE PATH
    "Explicit JDK home for the Android Gradle build (overrides the search)")

# Major version of the JDK installed at <jdk_home>, or "" if that is not
# a usable JDK. Reads the `release` file every JDK ships rather than
# spawning java(1) for each candidate; falls back to `java -version` for
# the odd layout that has no such file.
function(_bsfchat_jdk_major jdk_home out_var)
    set(${out_var} "" PARENT_SCOPE)
    if(NOT IS_DIRECTORY "${jdk_home}" OR NOT EXISTS "${jdk_home}/bin/javac")
        return()
    endif()

    set(_raw "")
    if(EXISTS "${jdk_home}/release")
        file(STRINGS "${jdk_home}/release" _lines REGEX "^JAVA_VERSION=")
        if(_lines)
            list(GET _lines 0 _raw)
        endif()
    endif()
    if(NOT _raw)
        # java writes its version banner to stderr.
        execute_process(
            COMMAND "${jdk_home}/bin/java" -version
            OUTPUT_QUIET
            ERROR_VARIABLE _raw
            RESULT_VARIABLE _rc)
        if(NOT _rc EQUAL 0)
            return()
        endif()
    endif()

    # Matches both `JAVA_VERSION="17.0.19"` and the banner's
    # `openjdk version "17.0.19"`. A JDK 8 reports 1.8.0_x, whose major
    # parses as 1 — below any floor we would ever set, so it is rejected
    # on the version check rather than needing a special case here.
    string(REGEX MATCH "\"([0-9]+)" _match "${_raw}")
    if(_match)
        set(${out_var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
    endif()
endfunction()

if(BSFCHAT_ANDROID_JDK_MAX LESS BSFCHAT_ANDROID_JDK_MIN)
    message(FATAL_ERROR
        "BSFCHAT_ANDROID_JDK_MAX (${BSFCHAT_ANDROID_JDK_MAX}) is below "
        "BSFCHAT_ANDROID_JDK_MIN (${BSFCHAT_ANDROID_JDK_MIN}); no JDK can "
        "satisfy that.")
endif()

# Ordered list of places a supported JDK might live. JAVA_HOME comes
# first so a deliberate choice (CI's actions/setup-java, direnv, a
# developer's shell) wins whenever it is usable at all; after that we
# probe the well-known per-version install paths oldest-first, so that
# if the window is ever widened the version inside every toolchain's
# range still beats a newer one only some of them accept.
function(_bsfchat_android_jdk_candidates out_var)
    set(_out "")
    if(DEFINED ENV{JAVA_HOME} AND NOT "$ENV{JAVA_HOME}" STREQUAL "")
        list(APPEND _out "$ENV{JAVA_HOME}")
    endif()

    foreach(_v RANGE ${BSFCHAT_ANDROID_JDK_MIN} ${BSFCHAT_ANDROID_JDK_MAX})
        if(APPLE)
            # Only finds JDKs installed as macOS bundles under
            # /Library/Java — Homebrew's are not, hence the paths below.
            execute_process(
                COMMAND /usr/libexec/java_home -v ${_v}
                OUTPUT_VARIABLE _jh
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
                RESULT_VARIABLE _rc)
            if(_rc EQUAL 0 AND _jh)
                list(APPEND _out "${_jh}")
            endif()
        endif()
        list(APPEND _out
            "/opt/homebrew/opt/openjdk@${_v}/libexec/openjdk.jdk/Contents/Home"
            "/usr/local/opt/openjdk@${_v}/libexec/openjdk.jdk/Contents/Home"
            "/Library/Java/JavaVirtualMachines/temurin-${_v}.jdk/Contents/Home")
        # Linux distro layouts, plus whatever a tarball install dropped
        # into the conventional directory.
        file(GLOB _linux_jdks
            "/usr/lib/jvm/java-${_v}-openjdk*"
            "/usr/lib/jvm/java-${_v}-*"
            "/usr/lib/jvm/*-${_v}-jdk*"
            "/usr/lib/jvm/jdk-${_v}*")
        list(APPEND _out ${_linux_jdks})
    endforeach()

    list(REMOVE_DUPLICATES _out)
    set(${out_var} "${_out}" PARENT_SCOPE)
endfunction()

# --- 1. Resolve the JDK -----------------------------------------------

set(BSFCHAT_ANDROID_JDK_HOME "")

if(BSFCHAT_ANDROID_JDK)
    # Explicit -D wins and is trusted, but a typo here would surface as
    # the same unreadable D8 crash, so check it and say so instead.
    _bsfchat_jdk_major("${BSFCHAT_ANDROID_JDK}" _explicit_major)
    if(NOT _explicit_major)
        message(FATAL_ERROR
            "BSFCHAT_ANDROID_JDK=${BSFCHAT_ANDROID_JDK} is not a JDK "
            "(no bin/javac). Point it at a JDK home, or unset it to let "
            "the build find one.")
    endif()
    if(_explicit_major LESS BSFCHAT_ANDROID_JDK_MIN
       OR _explicit_major GREATER BSFCHAT_ANDROID_JDK_MAX)
        message(WARNING
            "BSFCHAT_ANDROID_JDK is Java ${_explicit_major}, outside the "
            "${BSFCHAT_ANDROID_JDK_MIN}-${BSFCHAT_ANDROID_JDK_MAX} range this "
            "Gradle/AGP toolchain supports. Using it as asked; expect Gradle "
            "to refuse to start, or D8 to fail dexing anonymous inner classes.")
    endif()
    set(BSFCHAT_ANDROID_JDK_HOME "${BSFCHAT_ANDROID_JDK}")
    message(STATUS
        "Android: Gradle will use Java ${_explicit_major} at "
        "${BSFCHAT_ANDROID_JDK} (BSFCHAT_ANDROID_JDK)")
else()
    _bsfchat_android_jdk_candidates(_jdk_candidates)
    foreach(_cand IN LISTS _jdk_candidates)
        _bsfchat_jdk_major("${_cand}" _major)
        if(_major
           AND NOT _major LESS BSFCHAT_ANDROID_JDK_MIN
           AND NOT _major GREATER BSFCHAT_ANDROID_JDK_MAX)
            set(BSFCHAT_ANDROID_JDK_HOME "${_cand}")
            message(STATUS "Android: Gradle will use Java ${_major} at ${_cand}")
            break()
        endif()
    endforeach()
endif()

if(NOT BSFCHAT_ANDROID_JDK_HOME)
    if(BSFCHAT_ANDROID_JDK_MIN STREQUAL BSFCHAT_ANDROID_JDK_MAX)
        set(_jdk_wanted "No JDK ${BSFCHAT_ANDROID_JDK_MIN}")
    else()
        set(_jdk_wanted "No JDK between ${BSFCHAT_ANDROID_JDK_MIN} and "
                        "${BSFCHAT_ANDROID_JDK_MAX}")
    endif()
    # Not fatal: the C++/QML half of the build is still worth having,
    # and a JDK outside the window may still work on a newer kit.
    message(WARNING
        "${_jdk_wanted} found; Gradle will fall back to JAVA_HOME / the "
        "default java. If the apk target dies in dexBuilder with a D8 "
        "NullPointerException, that is why — install one (macOS: "
        "`brew install openjdk@${BSFCHAT_ANDROID_JDK_MIN}`) or pass "
        "-DBSFCHAT_ANDROID_JDK=<jdk home>.")
endif()

# --- 2. Stage the package source dir with the JDK baked in ------------

set(_pkg_src "${CMAKE_CURRENT_SOURCE_DIR}/android")
set(_pkg_staged "${CMAKE_CURRENT_BINARY_DIR}/android-package")

# Re-run CMake (and so refresh the copy) when anything in android/
# changes, otherwise an edited manifest -- or a newly added Java source
# -- would silently not reach the APK. Two mechanisms, because they
# catch different things: CONFIGURE_DEPENDS on the glob re-runs the glob
# at build time and reconfigures when a file is ADDED or REMOVED, while
# CMAKE_CONFIGURE_DEPENDS on the resulting list reconfigures when the
# CONTENT of one of them changes.
file(GLOB_RECURSE _pkg_files CONFIGURE_DEPENDS LIST_DIRECTORIES false
    "${_pkg_src}/*")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_pkg_files})

# Full replace, not a merge: a file deleted from android/ must not live
# on in the staged copy and get packaged.
file(REMOVE_RECURSE "${_pkg_staged}")
file(COPY "${_pkg_src}/" DESTINATION "${_pkg_staged}")

if(BSFCHAT_ANDROID_JDK_HOME)
    # SEED FROM QT'S TEMPLATE FIRST. The comment at the top of this file
    # is right that androiddeployqt copies the Qt template, then copies
    # QT_ANDROID_PACKAGE_SOURCE_DIR over the top, then runs
    # mergeGradleProperties() — and right that the merge preserves keys
    # it does not own. What it missed is what "over the top" does at
    # FILE granularity: the package source dir's gradle.properties
    # REPLACES the template's, so by the time the merge runs, the
    # template's keys are already gone and there is nothing to preserve.
    #
    # The template is not decoration. It carries
    #
    #     android.useAndroidX=true   <- Qt 6.9+ pulls androidx.core, and
    #                                   AGP hard-fails :mergeReleaseNativeLibs
    #                                   without this. That is the whole
    #                                   symptom: "Configuration
    #                                   :releaseRuntimeClasspath contains
    #                                   AndroidX dependencies, but the
    #                                   android.useAndroidX property is not
    #                                   enabled."
    #     org.gradle.jvmargs=-Xmx2500m ...  <- the default heap is far
    #                                   smaller and D8 needs this one
    #     org.gradle.parallel=true
    #
    # So: copy the template, then append our line. Appending to a copy of
    # the template is also why this cannot regress silently — if Qt adds
    # a key in a future kit, it comes along.
    set(_qt_gradle_template
        "${QT6_INSTALL_PREFIX}/src/3rdparty/gradle/gradle.properties")
    if(NOT EXISTS "${_qt_gradle_template}")
        # QT6_INSTALL_PREFIX is not set for every generator/toolchain
        # combination; Qt6_DIR always is, and the prefix is three levels
        # above <prefix>/lib/cmake/Qt6.
        get_filename_component(_qt_prefix "${Qt6_DIR}/../../.." ABSOLUTE)
        set(_qt_gradle_template
            "${_qt_prefix}/src/3rdparty/gradle/gradle.properties")
    endif()

    if(EXISTS "${_qt_gradle_template}")
        file(READ "${_qt_gradle_template}" _qt_gradle_defaults)
        file(WRITE "${_pkg_staged}/gradle.properties" "${_qt_gradle_defaults}")
    else()
        # Refuse rather than produce a tree that fails minutes later in
        # Gradle with a message that names none of this. The one key
        # that is not optional is spelled out so the error is also the
        # workaround.
        message(FATAL_ERROR
            "Could not find Qt's gradle.properties template (looked in "
            "${_qt_gradle_template}). It is the source of "
            "android.useAndroidX=true and the Gradle daemon's heap "
            "settings, and staging a gradle.properties without it makes "
            "AGP fail in :mergeReleaseNativeLibs. Point "
            "BSFCHAT_ANDROID_GRADLE_TEMPLATE at it, or set "
            "-DBSFCHAT_ANDROID_JDK_HOME= to skip the staged file "
            "entirely and select the JDK through JAVA_HOME.")
    endif()

    file(APPEND "${_pkg_staged}/gradle.properties"
"
# Generated by cmake/AndroidJdk.cmake — do not edit, and do not copy
# this into the checked-in android/ directory: the path is specific to
# the machine that configured this build tree. Everything above this
# line is Qt's own template, copied verbatim; see the comment in
# cmake/AndroidJdk.cmake for why it has to be.
#
# Gradle runs javac and D8 in its daemon JVM, so this one line is what
# keeps AGP's dexer from choking on class files a too-new javac emitted.
org.gradle.java.home=${BSFCHAT_ANDROID_JDK_HOME}
")
endif()

set(BSFCHAT_ANDROID_PACKAGE_SOURCE_DIR "${_pkg_staged}")

# --- 3. Drop intermediates compiled by a different JDK ----------------
#
# Changing the daemon JVM does not invalidate Gradle's incremental
# javac output, so a tree built once with the wrong JDK keeps handing
# D8 the same unreadable class files and keeps failing — the fix would
# look like it had not worked. Clearing the intermediates on a JDK
# change (including the first configure after this file landed, when
# there is no stamp yet) makes the next build recompile them.
set(_jdk_stamp "${CMAKE_CURRENT_BINARY_DIR}/android-build/.bsfchat-gradle-jdk")
set(_jdk_prev "")
if(EXISTS "${_jdk_stamp}")
    file(READ "${_jdk_stamp}" _jdk_prev)
    string(STRIP "${_jdk_prev}" _jdk_prev)
endif()
if(NOT _jdk_prev STREQUAL "${BSFCHAT_ANDROID_JDK_HOME}")
    set(_gradle_intermediates
        "${CMAKE_CURRENT_BINARY_DIR}/android-build/build/intermediates")
    if(IS_DIRECTORY "${_gradle_intermediates}")
        message(STATUS
            "Android: JDK changed, dropping Gradle intermediates compiled "
            "by the previous one")
        file(REMOVE_RECURSE "${_gradle_intermediates}")
    endif()
    file(WRITE "${_jdk_stamp}" "${BSFCHAT_ANDROID_JDK_HOME}\n")
endif()
