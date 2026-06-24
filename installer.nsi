; ─── Trip Language Installer ───────────────────────────────────────────────
; NSIS installer script for the Trip programming language

!define APP_NAME     "Trip"
!define APP_VERSION  "1.0.0"
!define APP_EXE      "trip.exe"
!define INSTALL_DIR  "$PROGRAMFILES64\Trip"
!define REG_KEY      "Software\Microsoft\Windows\CurrentVersion\Uninstall\Trip"

; Metadata
Name "${APP_NAME} ${APP_VERSION}"
OutFile "TripSetup.exe"
InstallDir "${INSTALL_DIR}"
InstallDirRegKey HKLM "${REG_KEY}" "InstallLocation"
RequestExecutionLevel admin
SetCompressor /SOLID lzma

; Modern UI
!include "MUI2.nsh"

!define MUI_ICON "trip-icon.ico"
!define MUI_UNICON "trip-icon.ico"
!define MUI_WELCOMEPAGE_TITLE "Welcome to the Trip Installer"
!define MUI_WELCOMEPAGE_TEXT "This will install Trip ${APP_VERSION} on your computer.$\n$\nTrip is a simple, expressive programming language.$\n$\nClick Next to continue."
!define MUI_FINISHPAGE_RUN "$INSTDIR\trip.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Run Trip REPL after install"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; ─── Install ────────────────────────────────────────────────────────────────
Section "Trip (required)" SecMain
  SectionIn RO

  SetOutPath "$INSTDIR"
  File "trip.exe"
  File "trip-icon.ico"

  ; Add to system PATH
  EnVar::AddValue "PATH" "$INSTDIR"

  ; Register file association for .tp files
  WriteRegStr HKCR ".tp"              "" "TripScript"
  WriteRegStr HKCR "TripScript"       "" "Trip Script"
  WriteRegStr HKCR "TripScript\DefaultIcon" "" "$INSTDIR\trip-icon.ico,0"
  WriteRegStr HKCR "TripScript\shell\open\command" "" '"$INSTDIR\trip.exe" "%1"'

  ; Write uninstall info (shows in Add/Remove Programs)
  WriteRegStr   HKLM "${REG_KEY}" "DisplayName"          "Trip Programming Language"
  WriteRegStr   HKLM "${REG_KEY}" "DisplayVersion"       "${APP_VERSION}"
  WriteRegStr   HKLM "${REG_KEY}" "Publisher"            "Trip"
  WriteRegStr   HKLM "${REG_KEY}" "InstallLocation"      "$INSTDIR"
  WriteRegStr   HKLM "${REG_KEY}" "DisplayIcon"          "$INSTDIR\trip-icon.ico"
  WriteRegStr   HKLM "${REG_KEY}" "UninstallString"      "$INSTDIR\uninstall.exe"
  WriteRegStr   HKLM "${REG_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKLM "${REG_KEY}" "NoModify"             1
  WriteRegDWORD HKLM "${REG_KEY}" "NoRepair"             1

  WriteUninstaller "$INSTDIR\uninstall.exe"
SectionEnd

; ─── Uninstall ──────────────────────────────────────────────────────────────
Section "Uninstall"
  ; Remove from PATH
  EnVar::DeleteValue "PATH" "$INSTDIR"

  ; Remove files
  Delete "$INSTDIR\trip.exe"
  Delete "$INSTDIR\trip-icon.ico"
  Delete "$INSTDIR\uninstall.exe"
  RMDir  "$INSTDIR"

  ; Remove file association
  DeleteRegKey HKCR ".tp"
  DeleteRegKey HKCR "TripScript"

  ; Remove from Add/Remove Programs
  DeleteRegKey HKLM "${REG_KEY}"
SectionEnd