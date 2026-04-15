!include "MUI2.nsh"

Name "BSFChat"
OutFile "BSFChat-Setup.exe"
InstallDir "$PROGRAMFILES64\BSFChat"
RequestExecutionLevel admin

!define MUI_ICON "${NSISDIR}\Contrib\Graphics\Icons\modern-install.ico"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Section "Install"
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

    ; Registry for Add/Remove Programs
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\BSFChat" "DisplayName" "BSFChat"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\BSFChat" "UninstallString" '"$INSTDIR\Uninstall.exe"'
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\BSFChat" "Publisher" "BSFChat"
SectionEnd

Section "Uninstall"
    RMDir /r "$INSTDIR"
    RMDir /r "$SMPROGRAMS\BSFChat"
    Delete "$DESKTOP\BSFChat.lnk"
    DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\BSFChat"
SectionEnd
