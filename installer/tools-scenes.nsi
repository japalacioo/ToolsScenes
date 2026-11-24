; Tools Scenes - instalador para OBS Studio (Windows x64)
; Se compila con makensis (NSIS 3) desde scripts/build-windows.sh.
Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"

!ifndef VERSION
  !define VERSION "1.0.0"
!endif
!ifndef DLL_PATH
  !error "Defina DLL_PATH"
!endif
!ifndef DATA_DIR
  !error "Defina DATA_DIR"
!endif
!ifndef OUT_FILE
  !define OUT_FILE "ToolsScenes-${VERSION}-Windows-x64-Instalador.exe"
!endif

!define PLUGIN "tools-scenes"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\ToolsScenes"
; Versión anterior del plugin (antes se llamaba ScenePrevio)
!define OLD_PLUGIN "scene-previo"
!define OLD_UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\ScenePrevio"

Name "Tools Scenes ${VERSION} para OBS Studio"
OutFile "${OUT_FILE}"
InstallDir "$PROGRAMFILES64\Tools Scenes"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
BrandingText "Tools Scenes ${VERSION}"
VIProductVersion "${VERSION}.0"
VIAddVersionKey /LANG=1034 "ProductName" "Tools Scenes"
VIAddVersionKey /LANG=1034 "FileDescription" "Vista previa de escenas para OBS Studio"
VIAddVersionKey /LANG=1034 "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=1034 "ProductVersion" "${VERSION}"
VIAddVersionKey /LANG=1034 "LegalCopyright" ""

!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "Instalar Tools Scenes ${VERSION}"
!define MUI_WELCOMEPAGE_TEXT "Este asistente instalará Tools Scenes, un conjunto de paneles para OBS Studio que muestra una vista previa en vivo de todas las escenas, al estilo vMix.$\r$\n$\r$\nRequisitos: OBS Studio 31 o superior (64 bits).$\r$\n$\r$\nCierre OBS Studio antes de continuar."
!define MUI_FINISHPAGE_TEXT "Tools Scenes se instaló correctamente.$\r$\n$\r$\nAbra OBS Studio y active los paneles desde el menú Paneles (Docks): Vista de Escenas, Multimedia y Capas, Salida Limpia."
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "Spanish"

Var PluginRoot
Var ObsMajor

; Devuelve en $ObsMajor la versión mayor de OBS instalada (0 si no se encuentra).
Function DetectObs
  SetRegView 64
  StrCpy $ObsMajor 0
  ReadRegStr $0 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OBS Studio" "DisplayVersion"
  ${If} $0 == ""
    ReadRegStr $0 HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\OBS Studio" "DisplayVersion"
  ${EndIf}
  StrCpy $1 0
  StrCpy $2 ""
  loop:
    StrCpy $3 $0 1 $1
    StrCmp $3 "" done
    StrCmp $3 "." done
    StrCpy $2 "$2$3"
    IntOp $1 $1 + 1
    Goto loop
  done:
  ${If} $2 != ""
    IntOp $ObsMajor $2 + 0
  ${EndIf}

  ; Respaldo: versión del ejecutable de OBS (obs64.exe).
  ${If} $ObsMajor == 0
    ReadRegStr $0 HKLM "SOFTWARE\OBS Studio" ""
    ${If} $0 == ""
      StrCpy $0 "$PROGRAMFILES64\obs-studio"
    ${EndIf}
    ClearErrors
    GetDLLVersion "$0\bin\64bit\obs64.exe" $1 $2
    ${IfNot} ${Errors}
      IntOp $ObsMajor $1 >> 16
      IntOp $ObsMajor $ObsMajor & 0xFFFF
    ${EndIf}
  ${EndIf}
FunctionEnd

; $2 = 1 si el texto en $1 contiene "obs64.exe" (sin distinguir mayúsculas).
Function ContainsObs64
  StrCpy $2 0
  StrCpy $3 0
  scan:
    StrCpy $4 $1 9 $3
    StrCmp $4 "" scan_end
    StrCmp $4 "obs64.exe" 0 +3
      StrCpy $2 1
      Goto scan_end
    IntOp $3 $3 + 1
    Goto scan
  scan_end:
FunctionEnd

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "Tools Scenes requiere Windows de 64 bits."
    Abort
  ${EndIf}
  SetShellVarContext all
  StrCpy $PluginRoot "$APPDATA\obs-studio\plugins\${PLUGIN}"

  ; ¿OBS abierto?
  check_obs:
  nsExec::ExecToStack 'cmd.exe /c tasklist /FI "IMAGENAME eq obs64.exe" /NH'
  Pop $0
  Pop $1
  Call ContainsObs64
  ${If} $2 == 1
    MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "OBS Studio está abierto. Ciérrelo y pulse Reintentar." IDRETRY check_obs
    Abort
  ${EndIf}

  Call DetectObs
  ${If} $ObsMajor == 0
    MessageBox MB_YESNO|MB_ICONQUESTION "No se encontró OBS Studio instalado en este equipo.$\r$\n¿Desea instalar Tools Scenes de todas formas?" IDYES +2
    Abort
  ${ElseIf} $ObsMajor < 31
    MessageBox MB_YESNO|MB_ICONEXCLAMATION "Se detectó OBS Studio $ObsMajor. Tools Scenes requiere OBS 31 o superior.$\r$\n¿Instalar de todas formas?" IDYES +2
    Abort
  ${EndIf}
FunctionEnd

Section "Tools Scenes" SecMain
  SetShellVarContext all
  SetOverwrite on

  ; Quitar instalaciones previas en cualquiera de las dos estructuras.
  Delete "$PluginRoot\${PLUGIN}.dll"
  Delete "$PluginRoot\bin\64bit\${PLUGIN}.dll"
  RMDir /r "$PluginRoot\data"

  ; Quitar la versión anterior (ScenePrevio) para que no se carguen dos copias.
  RMDir /r "$APPDATA\obs-studio\plugins\${OLD_PLUGIN}"
  Delete "$PROGRAMFILES64\ScenePrevio\Desinstalar.exe"
  RMDir "$PROGRAMFILES64\ScenePrevio"
  SetRegView 64
  DeleteRegKey HKLM "${OLD_UNINST_KEY}"
  ; Quitar la carpeta heredada si existía (bin\64bit).
  RMDir /r "$PluginRoot\bin"

  ; OBS 33+: estructura nueva plugins\tools-scenes\tools-scenes.dll (no "heredado").
  ; OBS 31-32 solo cargan plugins\tools-scenes\bin\64bit\tools-scenes.dll.
  ${If} $ObsMajor >= 33
    SetOutPath "$PluginRoot"
  ${Else}
    SetOutPath "$PluginRoot\bin\64bit"
  ${EndIf}
  File "/oname=${PLUGIN}.dll" "${DLL_PATH}"
  ${If} $ObsMajor >= 33
    DetailPrint "OBS $ObsMajor detectado: estructura nueva (plugins\${PLUGIN}\${PLUGIN}.dll)"
  ${Else}
    DetailPrint "OBS $ObsMajor detectado: estructura bin\64bit (OBS 31-32)"
  ${EndIf}

  SetOutPath "$PluginRoot\data"
  File /r "${DATA_DIR}\*.*"

  ; Desinstalador fuera de la carpeta de plugins
  SetOutPath "$INSTDIR"
  WriteUninstaller "$INSTDIR\Desinstalar.exe"

  SetRegView 64
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "Tools Scenes para OBS Studio"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "Tools Scenes"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\Desinstalar.exe"'
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$PluginRoot"
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
SectionEnd

Function un.onInit
  SetShellVarContext all
  StrCpy $PluginRoot "$APPDATA\obs-studio\plugins\${PLUGIN}"
FunctionEnd

Section "Uninstall"
  SetShellVarContext all
  Delete "$PluginRoot\${PLUGIN}.dll"
  Delete "$PluginRoot\bin\64bit\${PLUGIN}.dll"
  RMDir "$PluginRoot\bin\64bit"
  RMDir "$PluginRoot\bin"
  RMDir /r "$PluginRoot\data"
  RMDir "$PluginRoot"
  Delete "$INSTDIR\Desinstalar.exe"
  RMDir "$INSTDIR"
  SetRegView 64
  DeleteRegKey HKLM "${UNINST_KEY}"
SectionEnd
