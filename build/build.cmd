@echo off
rem ===========================================================================
rem  Build senren-banka-a11y plugin (32-bit).
rem
rem  Requires:
rem    VCVARS   full path to vcvars32.bat (Visual Studio 32-bit build env)
rem    TJS_INC  directory with the TJS2 / krkrz interface headers.
rem             Defaults to third_party\tjs2 shipped in this repo
rem             (see third_party\README.md). Set it to override.
rem
rem  Usage:  set VCVARS=...   &   build\build.cmd          release build (default)
rem          set VCVARS=...   &   build\build.cmd dev      dev build (/DA11Y_DEV)
rem
rem  Release (default): A11Y_DEV is NOT defined. The dev REPL command-file
rem  channel and the hard-coded D:\a11yb diagnostic log are compiled out, so the
rem  shipped DLL contains no dev path at all (Framework G40).
rem  Dev: defines A11Y_DEV (the historical dev channel works unchanged), keeps
rem  debug info (/Od /Z7) and does not define /DNDEBUG.
rem
rem  NOTE: keep this file ASCII-only and CRLF-terminated - cmd.exe parses it in
rem  the OEM codepage and mis-handles UTF-8 comments and lone LF line endings.
rem ===========================================================================
setlocal
set ROOT=%~dp0..

if "%VCVARS%"=="" (
  echo [x] VCVARS is not set - point it at vcvars32.bat
  exit /b 1
)
if "%TJS_INC%"=="" set TJS_INC=%ROOT%\third_party\tjs2
if not exist "%TJS_INC%\tjsCommHead.h" (
  echo [x] tjsCommHead.h not found in TJS_INC: %TJS_INC%
  exit /b 1
)

call "%VCVARS%" >nul || exit /b 1

rem vcvars32.bat sets VSCMD_ARG_TGT_ARCH=x86. This guard matters: with a 64-bit
rem vcvars the build used to succeed and silently emit an x64 DLL that the 32-bit
rem game cannot load (the symptom would only be "the plugin does nothing").
if /i not "%VSCMD_ARG_TGT_ARCH%"=="x86" (
  echo [x] wrong target arch: VSCMD_ARG_TGT_ARCH="%VSCMD_ARG_TGT_ARCH%" - use vcvars32.bat
  exit /b 1
)

rem Build flavour: release is the default; "build\build.cmd dev" enables A11Y_DEV.
set DEV_BUILD=
if /i "%~1"=="dev" set DEV_BUILD=1

if "%DEV_BUILD%"=="1" (
  set MODE_OPT=/Od /Z7
  set MODE_DEFS=/DA11Y_DEV
) else (
  set MODE_OPT=/O1
  set MODE_DEFS=/DNDEBUG
)

if "%DEV_BUILD%"=="1" echo [i] dev build: A11Y_DEV defined - REPL channel and D:\a11yb diag log enabled

rem /MT links the CRT statically so we do not add another runtime to the game process
set CL=/nologo /EHsc /MT %MODE_OPT% /D_CRT_SECURE_NO_WARNINGS /DTJS_NO_REGEXP %MODE_DEFS% /DUNICODE /D_UNICODE /D__WIN32__ /utf-8
set INC=/I"%ROOT%\src" /I"%TJS_INC%"
set FI=/FI"%ROOT%\src\force.h"
set LOG=%ROOT%\build_log.txt

del "%LOG%" 2>nul

cl /c %CL% %INC% %FI% /Fo"%ROOT%\a11y.obj" "%ROOT%\src\a11y6.cpp" >>"%LOG%" 2>&1
if errorlevel 1 exit /b 1

link /nologo /DLL /MACHINE:X86 /OUT:"%ROOT%\a11y.dll" /DEF:"%ROOT%\src\a11y6.def" "%ROOT%\a11y.obj" user32.lib comctl32.lib gdi32.lib >>"%LOG%" 2>&1
if errorlevel 1 exit /b 1

echo BUILD_OK  -^>  %ROOT%\a11y.dll
