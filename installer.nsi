; ─────────────────────────────────────────────
; TripLang Installer (FINAL FIXED VERSION)
; ─────────────────────────────────────────────
!include "MUI2.nsh"
!include "nsDialogs.nsh" ;  FIXED: Added missing nsDialogs header
!include "StrFunc.nsh"

; Initialize StrFunc macros
${StrStr}
${UnStrRep} ;  FIXED: Initialized for clean uninstaller path removal

Name "Trip"
OutFile "Trip-Installer.exe"
InstallDir "$PROGRAMFILES64\TripLang"
RequestExecutionLevel admin

!define MUI_ABORTWARNING

Var AddToPath

; ── PAGES ────────────────────────────────────
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "LICENSE.txt"
!insertmacro MUI_PAGE_DIRECTORY
Page custom Page_SelectOptions Page_LeaveOptions
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\trip.exe"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; ─────────────────────────────────────────────
; CUSTOM PAGE
; ─────────────────────────────────────────────
Function Page_SelectOptions
    nsDialogs::Create 1018
    Pop $0

    ${NSD_CreateCheckbox} 20 50 250 20 "Add TripLang to PATH"
    Pop $AddToPath
    ${NSD_SetState} $AddToPath ${BST_CHECKED}

    nsDialogs::Show
FunctionEnd

Function Page_LeaveOptions
    ${NSD_GetState} $AddToPath $AddToPath
FunctionEnd

; ─────────────────────────────────────────────
; INSTALL
; ─────────────────────────────────────────────
Section "Install"

    SetOutPath "$INSTDIR"

    DetailPrint "Copying files..."

    ; COPY EVERYTHING (EXE + DLLs)
    ; NOTE: the old runtime check here (`IfFileExists "$EXEDIR\dist\triplang.exe"`)
    ; was broken — $EXEDIR resolves on the END USER's machine at install time
    ; (e.g. their Downloads folder), not the build machine. There is no
    ; dist\ folder there, so the check always failed and aborted the
    ; installer even though `File /r "dist\*"` below already embeds
    ; everything correctly at compile time (when YOU run makensis).
    ; If you want a "did I forget to build first" safety net, put it in
    ; the Makefile before calling makensis, not here.
    File /r "dist\*"

    ; ── Add to PATH
    StrCmp $AddToPath ${BST_CHECKED} 0 skip_path

        ReadRegStr $0 HKLM \
          "SYSTEM\CurrentControlSet\Control\Session Manager\Environment" \
          "Path"

        ; prevent duplicate
        ${StrStr} $1 $0 "$INSTDIR" ;  FIXED: Changed StrStr to ${StrStr}
        StrCmp $1 "" add_path skip_path

        add_path:
        StrCpy $0 "$0;$INSTDIR"

        WriteRegExpandStr HKLM \
          "SYSTEM\CurrentControlSet\Control\Session Manager\Environment" \
          "Path" "$0"

        ; Broadcast environment update (0x001A = WM_SETTINGCHANGE)
        System::Call 'User32::SendMessageTimeoutA(i 0xffff, i 0x001A, i 0, t "Environment", i 0, i 1000, *i .r0)'

    skip_path:

    ; ── Shortcuts
    CreateDirectory "$SMPROGRAMS\TripLang"

    CreateShortcut "$SMPROGRAMS\TripLang\TripLang.lnk" \
    "$INSTDIR\trip.exe"

    CreateShortcut "$SMPROGRAMS\TripLang\Uninstall.lnk" \
        "$INSTDIR\Uninstall.exe"

    ; ── Uninstaller
    WriteUninstaller "$INSTDIR\Uninstall.exe"

    ; ── Registry
    WriteRegStr HKLM \
      "Software\Microsoft\Windows\CurrentVersion\Uninstall\TripLang" \
      "DisplayName" "TripLang"

    WriteRegStr HKLM \
      "Software\Microsoft\Windows\CurrentVersion\Uninstall\TripLang" \
      "UninstallString" "$INSTDIR\Uninstall.exe"

SectionEnd

; ─────────────────────────────────────────────
; UNINSTALL
; ─────────────────────────────────────────────
Section "Uninstall"

    Delete "$INSTDIR\*.*"
    RMDir "$INSTDIR"

    ; ── Remove PATH (SAFE using StrFunc replacing the broken loop)
    ReadRegStr $0 HKLM \
      "SYSTEM\CurrentControlSet\Control\Session Manager\Environment" \
      "Path"

    ; Clean up variations of the string with its trailing/leading separators
    ${UnStrRep} $0 $0 "$INSTDIR;" ""
    ${UnStrRep} $0 $0 ";$INSTDIR" ""
    ${UnStrRep} $0 $0 "$INSTDIR" ""

    WriteRegExpandStr HKLM \
      "SYSTEM\CurrentControlSet\Control\Session Manager\Environment" \
      "Path" "$0"

    System::Call 'User32::SendMessageTimeoutA(i 0xffff, i 0x001A, i 0, t "Environment", i 0, i 1000, *i .r0)'

    Delete "$SMPROGRAMS\TripLang\TripLang.lnk"
    Delete "$SMPROGRAMS\TripLang\Uninstall.lnk"
    RMDir "$SMPROGRAMS\TripLang"

    DeleteRegKey HKLM \
      "Software\Microsoft\Windows\CurrentVersion\Uninstall\TripLang"

SectionEnd
