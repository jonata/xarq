; XarQ's Windows installer (NSIS, Modern UI 2).
;
;   makensis -DVERSION=26.10-alpha -DSOURCE=dist\XarQ -DOUTFILE=XarQ-26.10-alpha-Setup.exe xarq-installer.nsi
;
; Installs just for the current user, with no administrator rights, always in %LOCALAPPDATA%\Programs\XarQ:
; the folder is emptied on install and removed on uninstall, so it can't be one the user picked. .xar, XarQ's
; own files, open in XarQ. Command line: /S installs silently.

Unicode True
ManifestDPIAware true

!define APPNAME "XarQ"
!define WEBSITE "https://github.com/jonata/xarq"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}"
!define CLASSES "Software\Classes"
!define PROGID "XarQ.xar"

; Paths in this script are relative to the repository's root, where it runs from.
!cd "..\.."

Name "${APPNAME} ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$LOCALAPPDATA\Programs\${APPNAME}"
RequestExecutionLevel user
BrandingText "${APPNAME} ${VERSION}"

!include MUI2.nsh

!define MUI_ICON "src\assets\logo\xarq-appicon.ico"
!define MUI_UNICON "src\assets\logo\xarq-appicon.ico"
!define MUI_ABORTWARNING

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\xarq.exe"
!define MUI_FINISHPAGE_SHOWREADME
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Create a desktop shortcut"
!define MUI_FINISHPAGE_SHOWREADME_FUNCTION CreateDesktopShortcut
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Function CreateDesktopShortcut
  CreateShortcut "$DESKTOP\${APPNAME}.lnk" "$INSTDIR\xarq.exe"
FunctionEnd

Section
  ; An earlier version's files must not linger beside the new ones.
  RMDir /r "$INSTDIR"
  SetOutPath "$INSTDIR"
  File /r "${SOURCE}\*.*"
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  CreateShortcut "$SMPROGRAMS\${APPNAME}.lnk" "$INSTDIR\xarq.exe"

  WriteRegStr HKCU "${CLASSES}\.xar" "" "${PROGID}"
  WriteRegStr HKCU "${CLASSES}\${PROGID}" "" "XarQ document"
  WriteRegStr HKCU "${CLASSES}\${PROGID}\DefaultIcon" "" "$INSTDIR\xarq.exe,0"
  WriteRegStr HKCU "${CLASSES}\${PROGID}\shell\open\command" "" '"$INSTDIR\xarq.exe" --open "%1"'
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'

  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "${APPNAME}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\xarq.exe,0"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "Publisher" "${APPNAME}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "URLInfoAbout" "${WEBSITE}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  Delete "$SMPROGRAMS\${APPNAME}.lnk"
  Delete "$DESKTOP\${APPNAME}.lnk"
  RMDir /r "$INSTDIR"
  ReadRegStr $0 HKCU "${CLASSES}\.xar" ""
  StrCmp $0 "${PROGID}" 0 +2
  DeleteRegKey HKCU "${CLASSES}\.xar"
  StrCmp $0 "${PROGID}" 0 +2
  DeleteRegKey HKCU "${CLASSES}\${PROGID}"
  DeleteRegKey HKCU "${UNINSTALL_KEY}"
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
SectionEnd
