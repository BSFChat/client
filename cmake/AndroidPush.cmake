# Firebase Cloud Messaging for the Android build, WITHOUT the
# google-services Gradle plugin. Included from CMakeLists.txt after
# cmake/AndroidJdk.cmake has staged android/ into
# BSFCHAT_ANDROID_PACKAGE_SOURCE_DIR; everything here edits that staged
# copy, never the checked-in tree. See docs/push-notifications.md.
#
# INPUTS (cache variables, or the same-named environment variables, which
# is how CI passes them from repository variables):
#
#   BSFCHAT_FCM_PROJECT_ID   Firebase project id        e.g. bsfchat-5074b
#   BSFCHAT_FCM_APP_ID       Android app id             1:<number>:android:<hex>
#   BSFCHAT_FCM_API_KEY      Web API key of the project AIza...
#   BSFCHAT_FCM_SENDER_ID    project number / sender id <number>
#
# These are Firebase's public client configuration — the same values the
# plugin would bake into every APK from google-services.json — not secrets.
# All four or nothing: with any missing, push is compiled out and the app
# behaves as it did before push existed (persistent SyncService on by
# default).
#
# The app id names ONE package. A Debug build's application id is
# com.bsfchat.app.dev (BSFCHAT_BUNDLE_ID), which needs its own Android app
# registration in the Firebase project and its own BSFCHAT_FCM_APP_ID.
#
# WITH the values, in the staged dir:
#   * src/com/bsfchat/client/{FcmPush,PushMessagingService}.java from
#     android-push/fcm/, and a generated PushConfig.java;
#   * res/values/bsfchat_push.xml with the string resources
#     FirebaseInitProvider initialises the default FirebaseApp from;
#   * the messaging service + auto-init-off meta-data in the manifest;
#   * build.gradle = Qt's template + the firebase-messaging dependency.
# WITHOUT: src/com/bsfchat/client/FcmPush.java from android-push/stub/, the
# manifest marker removed, Qt's own build.gradle untouched.
#
# EXPOSES: BSFCHAT_ANDROID_FCM (ON/OFF).

set(BSFCHAT_FCM_MESSAGING_VERSION "24.1.1" CACHE STRING
    "com.google.firebase:firebase-messaging version for the Android build")

set(_fcm_vars PROJECT_ID APP_ID API_KEY SENDER_ID)
set(_fcm_missing "")
foreach(_v IN LISTS _fcm_vars)
    if(NOT DEFINED BSFCHAT_FCM_${_v} AND DEFINED ENV{BSFCHAT_FCM_${_v}})
        set(BSFCHAT_FCM_${_v} "$ENV{BSFCHAT_FCM_${_v}}")
    endif()
    set(BSFCHAT_FCM_${_v} "${BSFCHAT_FCM_${_v}}" CACHE STRING
        "Firebase client config for Android push (public; see cmake/AndroidPush.cmake)")
    string(STRIP "${BSFCHAT_FCM_${_v}}" BSFCHAT_FCM_${_v})
    if(BSFCHAT_FCM_${_v} STREQUAL "")
        list(APPEND _fcm_missing "BSFCHAT_FCM_${_v}")
    endif()
endforeach()

# Shape checks, so a pasted value with a quote or a stray character cannot
# produce broken Java/XML, and a swapped pair fails here rather than as a
# silent "no token" on a device.
if(NOT _fcm_missing)
    set(_fcm_bad "")
    if(NOT BSFCHAT_FCM_PROJECT_ID MATCHES "^[a-z0-9-]+$")
        list(APPEND _fcm_bad "BSFCHAT_FCM_PROJECT_ID")
    endif()
    if(NOT BSFCHAT_FCM_APP_ID MATCHES "^1:[0-9]+:android:[0-9a-f]+$")
        list(APPEND _fcm_bad "BSFCHAT_FCM_APP_ID (want 1:<number>:android:<hex>)")
    endif()
    if(NOT BSFCHAT_FCM_API_KEY MATCHES "^[A-Za-z0-9_-]+$")
        list(APPEND _fcm_bad "BSFCHAT_FCM_API_KEY")
    endif()
    if(NOT BSFCHAT_FCM_SENDER_ID MATCHES "^[0-9]+$")
        list(APPEND _fcm_bad "BSFCHAT_FCM_SENDER_ID")
    endif()
    if(_fcm_bad)
        message(FATAL_ERROR "Malformed Firebase config: ${_fcm_bad}")
    endif()
    string(REGEX MATCH "^1:([0-9]+):" _m "${BSFCHAT_FCM_APP_ID}")
    if(NOT CMAKE_MATCH_1 STREQUAL BSFCHAT_FCM_SENDER_ID)
        message(FATAL_ERROR
            "BSFCHAT_FCM_APP_ID (${BSFCHAT_FCM_APP_ID}) belongs to project number "
            "${CMAKE_MATCH_1}, not BSFCHAT_FCM_SENDER_ID ${BSFCHAT_FCM_SENDER_ID}")
    endif()
endif()

set(_push_src "${CMAKE_CURRENT_SOURCE_DIR}/android-push")
set(_pkg "${BSFCHAT_ANDROID_PACKAGE_SOURCE_DIR}")
set(_java_dir "${_pkg}/src/com/bsfchat/client")

# Same re-configure rule as android/ in AndroidJdk.cmake.
file(GLOB_RECURSE _push_files CONFIGURE_DEPENDS LIST_DIRECTORIES false "${_push_src}/*")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_push_files})

file(READ "${_pkg}/AndroidManifest.xml" _manifest)
if(NOT _manifest MATCHES "<!-- BSFCHAT_PUSH_COMPONENTS[^>]*-->")
    message(FATAL_ERROR "android/AndroidManifest.xml lost its BSFCHAT_PUSH_COMPONENTS marker")
endif()

if(_fcm_missing)
    set(BSFCHAT_ANDROID_FCM OFF)
    message(STATUS "Android push: OFF (unset: ${_fcm_missing}); "
                   "the persistent background connection stays the default")
    file(COPY "${_push_src}/stub/com/bsfchat/client/FcmPush.java" DESTINATION "${_java_dir}")
    string(REGEX REPLACE "<!-- BSFCHAT_PUSH_COMPONENTS[^>]*-->" "" _manifest "${_manifest}")
    file(WRITE "${_pkg}/AndroidManifest.xml" "${_manifest}")
    return()
endif()

set(BSFCHAT_ANDROID_FCM ON)
message(STATUS "Android push: FCM project ${BSFCHAT_FCM_PROJECT_ID}, "
               "firebase-messaging ${BSFCHAT_FCM_MESSAGING_VERSION}")

file(COPY "${_push_src}/fcm/com/bsfchat/client/FcmPush.java"
          "${_push_src}/fcm/com/bsfchat/client/PushMessagingService.java"
     DESTINATION "${_java_dir}")

file(WRITE "${_java_dir}/PushConfig.java"
"package com.bsfchat.client;

// Generated by cmake/AndroidPush.cmake from the BSFCHAT_FCM_* values.
// Firebase public client configuration; not secrets.
final class PushConfig {
    private PushConfig() { }
    static final String PROJECT_ID = \"${BSFCHAT_FCM_PROJECT_ID}\";
    static final String APP_ID = \"${BSFCHAT_FCM_APP_ID}\";
    static final String API_KEY = \"${BSFCHAT_FCM_API_KEY}\";
    static final String SENDER_ID = \"${BSFCHAT_FCM_SENDER_ID}\";
}
")

file(MAKE_DIRECTORY "${_pkg}/res/values")
file(WRITE "${_pkg}/res/values/bsfchat_push.xml"
"<?xml version=\"1.0\" encoding=\"utf-8\"?>
<!-- Generated by cmake/AndroidPush.cmake. The resource names are the ones
     the google-services plugin would generate and FirebaseOptions.fromResource
     reads, so FirebaseInitProvider sets up the default app at process start,
     including a cold start caused by a push. -->
<resources>
    <string name=\"google_app_id\" translatable=\"false\">${BSFCHAT_FCM_APP_ID}</string>
    <string name=\"gcm_defaultSenderId\" translatable=\"false\">${BSFCHAT_FCM_SENDER_ID}</string>
    <string name=\"google_api_key\" translatable=\"false\">${BSFCHAT_FCM_API_KEY}</string>
    <string name=\"project_id\" translatable=\"false\">${BSFCHAT_FCM_PROJECT_ID}</string>
</resources>
")

set(_push_manifest
"<service
            android:name=\"com.bsfchat.client.PushMessagingService\"
            android:exported=\"false\">
            <intent-filter>
                <action android:name=\"com.google.firebase.MESSAGING_EVENT\" />
            </intent-filter>
        </service>
        <!-- No token, and so no contact with Firebase, until the app asks
             for one: it does so only when the user is on push rather than a
             background connection (PushBridge.start). -->
        <meta-data
            android:name=\"firebase_messaging_auto_init_enabled\"
            android:value=\"false\" />")
string(REGEX REPLACE "<!-- BSFCHAT_PUSH_COMPONENTS[^>]*-->" "${_push_manifest}"
       _manifest "${_manifest}")
file(WRITE "${_pkg}/AndroidManifest.xml" "${_manifest}")

# build.gradle: a package-source-dir build.gradle REPLACES Qt's template
# (androiddeployqt copies per file, the same trap AndroidJdk.cmake documents
# for gradle.properties), so it has to be the template plus our lines.
# Appending a second repositories/dependencies block is valid Groovy and
# keeps us independent of the template's exact text.
set(_gradle_candidates
    "${QT6_INSTALL_PREFIX}/src/android/templates/build.gradle")
get_filename_component(_qt_prefix_push "${Qt6_DIR}/../../.." ABSOLUTE)
list(APPEND _gradle_candidates "${_qt_prefix_push}/src/android/templates/build.gradle")
set(_gradle_template "")
foreach(_c IN LISTS _gradle_candidates)
    if(EXISTS "${_c}")
        set(_gradle_template "${_c}")
        break()
    endif()
endforeach()
if(NOT _gradle_template)
    message(FATAL_ERROR
        "Android push is configured but Qt's build.gradle template was not found "
        "(looked in: ${_gradle_candidates}). Unset the BSFCHAT_FCM_* values to "
        "build without push.")
endif()
file(READ "${_gradle_template}" _gradle)
file(WRITE "${_pkg}/build.gradle" "${_gradle}
// ── Added by cmake/AndroidPush.cmake ──────────────────────────────────
// Firebase Cloud Messaging for push notifications. No google-services
// plugin: FirebaseApp is configured from generated resources/PushConfig.
repositories {
    google()
    mavenCentral()
}
dependencies {
    implementation 'com.google.firebase:firebase-messaging:${BSFCHAT_FCM_MESSAGING_VERSION}'
}
")
