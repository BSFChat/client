!include "MUI2.nsh"
!include "LogicLib.nsh"

; Version metadata. CI passes both with `/D` so the strict 4-part
; form (required by VIProductVersion) and the user-visible form
; (which may carry a "-suffix") stay in sync. Local invocations
; (`makensis installer.nsi`) fall through to the dev defaults.
!ifndef BSFCHAT_VERSION
    !define BSFCHAT_VERSION "0.0.0-dev"
!endif
!ifndef BSFCHAT_VERSION_NSIS
    !define BSFCHAT_VERSION_NSIS "0.0.0.0"
!endif

Name "BSFChat"
OutFile "BSFChat-Setup.exe"
InstallDir "$PROGRAMFILES64\BSFChat"
; Pick up the path written by a prior install so silent
; (auto-update) runs land on top of the existing install
; instead of in the default Program Files dir.
InstallDirRegKey HKLM "Software\BSFChat" "InstallLocation"
RequestExecutionLevel admin

; Windows file-version resource block. Shows up in Explorer →
; Properties → Details and in Add/Remove Programs. Required for
; SmartScreen reputation to accrue against this installer rather
; than treating every build as a brand-new unknown binary.
VIProductVersion "${BSFCHAT_VERSION_NSIS}"
VIAddVersionKey  "ProductName"      "BSFChat"
VIAddVersionKey  "CompanyName"      "BSFChat"
VIAddVersionKey  "FileDescription"  "BSFChat Installer"
VIAddVersionKey  "FileVersion"      "${BSFCHAT_VERSION}"
VIAddVersionKey  "ProductVersion"   "${BSFCHAT_VERSION}"
VIAddVersionKey  "OriginalFilename" "BSFChat-Setup.exe"
VIAddVersionKey  "LegalCopyright"   "(c) BSFChat"

; Brand icons for the installer + uninstaller. The CI workflow
; copies branding\BSFChat.ico into the dist\ dir before invoking
; makensis so this relative path resolves.
!define MUI_ICON "BSFChat.ico"
!define MUI_UNICON "BSFChat.ico"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; Required for an unattended (`/S`) install: a running
; bsfchat-app.exe holds locks on its own .exe / .dll files and
; would otherwise make File /r fail mid-copy.
!macro StopRunningApp
    nsExec::Exec 'taskkill /IM bsfchat-app.exe'
    Pop $0
    Sleep 500
    nsExec::Exec 'taskkill /IM bsfchat-app.exe /F'
    Pop $0
    Sleep 200
!macroend

Section "Install"
    !insertmacro StopRunningApp

    SetOutPath "$INSTDIR"
    File /r "*.*"

    ; Create uninstaller
    WriteUninstaller "$INSTDIR\Uninstall.exe"

    ; Start menu shortcuts
    CreateDirectory "$SMPROGRAMS\BSFChat"
    CreateShortcut "$SMPROGRAMS\BSFChat\BSFChat.lnk" "$INSTDIR\bsfchat-app.exe"
    CreateShortcut "$SMPROGRAMS\BSFChat\Uninstall.lnk" "$INSTDIR\Uninstall.exe"

    ; Desktop shortcut
    CreateShortcut "$DESKTOP\BSFChat.lnk" "$INSTDIR\bsfchat-app.exe"

    ; Record install location so the next installer (silent
    ; auto-update or manual reinstall) overwrites in place.
    WriteRegStr HKLM "Software\BSFChat" "InstallLocation" "$INSTDIR"

    ; Registry for Add/Remove Programs
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\BSFChat" "DisplayName" "BSFChat"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\BSFChat" "DisplayVersion" "${BSFCHAT_VERSION}"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\BSFChat" "UninstallString" '"$INSTDIR\Uninstall.exe"'
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\BSFChat" "Publisher" "BSFChat"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\BSFChat" "InstallLocation" "$INSTDIR"
SectionEnd

; Auto-update relaunch: only fire when /S was passed so a manual
; install still ends on the FINISH page and the user decides.
Function .onInstSuccess
    ${If} ${Silent}
        Exec '"$INSTDIR\bsfchat-app.exe"'
    ${EndIf}
FunctionEnd

Section "Uninstall"
    !insertmacro StopRunningApp

    RMDir /r "$INSTDIR"
    RMDir /r "$SMPROGRAMS\BSFChat"
    Delete "$DESKTOP\BSFChat.lnk"
    DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\BSFChat"
    DeleteRegKey HKLM "Software\BSFChat"
SectionEnd
