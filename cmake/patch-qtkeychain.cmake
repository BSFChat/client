# iOS keychain accessibility for QtKeychain 0.15.0 (audit M2-6).
#
# QtKeychain writes generic-password items with no kSecAttrAccessible, so
# iOS gives them the default, kSecAttrAccessibleWhenUnlocked. BSFChat keeps
# its BSFChat ID refresh token there and rotates it on every refresh, and a
# refresh can run while the phone is locked — during a background voice call,
# for one. The write then fails with errSecInteractionNotAllowed while the
# provider has already spent the old token: the next launch presents a used
# refresh token, which the provider treats as theft and answers by revoking
# the whole grant.
#
# QtKeychain 0.15 has no API for item attributes, so this patch sets
# kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly on both the insert and the
# update in StartWritePassword (the update migrates items written before this
# patch the next time they are saved). "ThisDeviceOnly" also keeps the token
# out of iCloud Keychain and encrypted-backup restores to other devices.
# iOS only: the macOS file keychain does not use data-protection classes.
#
# Idempotent (FetchContent re-runs PATCH_COMMAND whenever it re-populates).
# Fails the configure if an anchor stops matching, so moving the pin cannot
# silently drop it. Run with the qtkeychain source tree as the working dir.

set(_f "qtkeychain/keychain_apple.mm")
if(NOT EXISTS "${_f}")
    message(FATAL_ERROR "patch-qtkeychain: ${_f} not found; the QtKeychain "
        "layout changed. Re-check the iOS keychain accessibility fix (M2-6).")
endif()

file(READ "${_f}" _content)
set(_marker "BSFCHAT: keychain accessibility")
if(_content MATCHES "${_marker}")
    return()
endif()

set(_helper_anchor [=[using namespace QKeychain;
]=])
set(_helper [=[using namespace QKeychain;

// BSFCHAT: keychain accessibility. See cmake/patch-qtkeychain.cmake.
#include <TargetConditionals.h>
static void BSFChatApplyAccessibility(NSMutableDictionary *dict)
{
#if TARGET_OS_IPHONE
    dict[(__bridge NSString *)kSecAttrAccessible] =
        (__bridge NSString *)kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly;
#else
    (void)dict;
#endif
}
]=])

set(_update_before [=[            NSDictionary * const update = @{
                    (__bridge NSString *)kSecValueData: data.toNSData(),
            };
]=])
set(_update_after [=[            NSMutableDictionary * const update = [NSMutableDictionary dictionaryWithDictionary:@{
                    (__bridge NSString *)kSecValueData: data.toNSData(),
            }];
            BSFChatApplyAccessibility(update);
]=])

set(_insert_before [=[            NSDictionary * const insert = @{
                    (__bridge NSString *)kSecClass: (__bridge NSString *)kSecClassGenericPassword,
                    (__bridge NSString *)kSecAttrService: service.toNSString(),
                    (__bridge NSString *)kSecAttrAccount: key.toNSString(),
                    (__bridge NSString *)kSecValueData: data.toNSData(),
            };
]=])
set(_insert_after [=[            NSMutableDictionary * const insert = [NSMutableDictionary dictionaryWithDictionary:@{
                    (__bridge NSString *)kSecClass: (__bridge NSString *)kSecClassGenericPassword,
                    (__bridge NSString *)kSecAttrService: service.toNSString(),
                    (__bridge NSString *)kSecAttrAccount: key.toNSString(),
                    (__bridge NSString *)kSecValueData: data.toNSData(),
            }];
            BSFChatApplyAccessibility(insert);
]=])

foreach(_anchor _helper_anchor _update_before _insert_before)
    string(FIND "${_content}" "${${_anchor}}" _pos)
    if(_pos EQUAL -1)
        message(FATAL_ERROR "patch-qtkeychain: anchor ${_anchor} not found in "
            "${_f}. The QtKeychain pin moved; check whether it now sets "
            "kSecAttrAccessible itself (then delete this patch and its "
            "PATCH_COMMAND in cmake/Dependencies.cmake) or re-anchor it.")
    endif()
endforeach()

string(REPLACE "${_helper_anchor}" "${_helper}" _content "${_content}")
string(REPLACE "${_update_before}" "${_update_after}" _content "${_content}")
string(REPLACE "${_insert_before}" "${_insert_after}" _content "${_content}")
file(WRITE "${_f}" "${_content}")
message(STATUS "Patched QtKeychain: iOS items AfterFirstUnlockThisDeviceOnly")
